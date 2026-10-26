#include "pipewire_output.h"

#if defined(__linux__) && defined(SAPFS_HAVE_PIPEWIRE)

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct PipewireOutput {
    struct pw_thread_loop* loop;
    struct pw_context* context;
    struct pw_core* core;
    struct pw_stream* stream;
    struct spa_hook core_listener;
    struct spa_hook stream_listener;
    int initialized;
    int loop_started;
    int core_listening;
    int stream_listening;
    int closing;
    int failed;
    int sync_seq;
    int sync_done;
    enum pw_stream_state stream_state;
    AudioState state;
    AudioFormat format;
    struct spa_audio_info_raw raw;
    PipewireMode mode;
    unsigned int timeout_ms;
    int format_ready;
    int graph_rate_verified;
    uint64_t last_process_ns;
    uint8_t* queue;
    size_t capacity; /* Frames; every operation preserves complete frames. */
    size_t filled;
    size_t read_pos;
    size_t write_pos;
    int drain_requested;
    int drain_sent;
    int drained;
    char error_msg[512];
};

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local char last_error[512];
static _Thread_local char error_copy[512];

/* All mutable state, FIFO operations and callbacks are serialized by the
 * thread loop. Deliberately omit RT_PROCESS: no mutex or allocation is performed
 * by our code on PipeWire's realtime data thread. */
static int fail(PipewireOutput* output, const char* message, int err) {
    if (!output->failed) {
        if (err < 0)
            snprintf(output->error_msg, sizeof(output->error_msg), "%s: %s", message, spa_strerror(err));
        else
            snprintf(output->error_msg, sizeof(output->error_msg), "%s", message);
    }
    output->failed = 1;
    if (output->loop_started) pw_thread_loop_signal(output->loop, false);
    return -1;
}

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static int check_health(PipewireOutput* output) {
    if (output->failed) return -1;
    if (output->state == AUDIO_STATE_PLAYING && !output->drained &&
        (output->filled || output->drain_requested) &&
        monotonic_ns() - output->last_process_ns > (uint64_t)output->timeout_ms * SPA_NSEC_PER_MSEC)
        return fail(output, "PipeWire graph stopped consuming audio; the server or target may be unavailable", 0);
    return 0;
}

static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
    PipewireOutput* output = data;
    (void)id; (void)seq;
    if (!output->closing) fail(output, message ? message : "PipeWire server error", res);
}

static void on_core_done(void* data, uint32_t id, int seq) {
    PipewireOutput* output = data;
    if (id == PW_ID_CORE && seq == output->sync_seq) {
        output->sync_done = 1;
        pw_thread_loop_signal(output->loop, false);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .done = on_core_done,
    .error = on_core_error,
};

static void on_state_changed(void* data, enum pw_stream_state old,
                             enum pw_stream_state state, const char* error) {
    PipewireOutput* output = data;
    (void)old;
    output->stream_state = state;
    if (!output->closing) {
        if (state == PW_STREAM_STATE_ERROR)
            fail(output, error ? error : "PipeWire stream error", 0);
        else if (state == PW_STREAM_STATE_UNCONNECTED)
            fail(output, "PipeWire stream disconnected (server or target disappeared)", 0);
    }
    if (output->loop_started) pw_thread_loop_signal(output->loop, false);
}

static void on_param_changed(void* data, uint32_t id, const struct spa_pod* param) {
    PipewireOutput* output = data;
    struct spa_audio_info_raw info = {0};
    if (id != SPA_PARAM_Format) return;
    output->format_ready = 0;
    if (!param) return;
    if (spa_format_audio_raw_parse(param, &info) < 0 ||
        info.format != output->raw.format || info.rate != output->raw.rate ||
        info.channels != output->raw.channels ||
        memcmp(info.position, output->raw.position, info.channels * sizeof(info.position[0]))) {
        fail(output, "PipeWire changed the requested application PCM format or channel layout", 0);
        return;
    }
    output->format_ready = 1;
    pw_thread_loop_signal(output->loop, false);
}

static void on_drained(void* data) {
    PipewireOutput* output = data;
    if (output->drain_sent) output->drained = 1;
    pw_thread_loop_signal(output->loop, false);
}

static int send_drain(PipewireOutput* output) {
    int err;
    if (!output->drain_requested || output->filled || output->drain_sent) return 0;
    output->drain_sent = 1;
    err = pw_stream_flush(output->stream, true);
    return err < 0 ? fail(output, "Cannot drain PipeWire stream", err) : 0;
}

static void on_process(void* data) {
    PipewireOutput* output = data;
    struct pw_buffer* buffer;
    struct spa_data* dst;
    size_t frames, first, count;
    if (output->failed || output->state != AUDIO_STATE_PLAYING || !output->format_ready) return;
    output->last_process_ns = monotonic_ns();
    if (output->mode == PIPEWIRE_MODE_EXCLUSIVE) {
        struct pw_time time = {0};
        if (pw_stream_get_time_n(output->stream, &time, sizeof(time)) < 0 || !time.rate.num || !time.rate.denom)
            return; /* Do not submit audio until the actual graph rate is known. */
        if ((uint64_t)time.rate.num * output->format.sample_rate != time.rate.denom) {
            fail(output, "PipeWire graph rate differs from the source in exclusive mode", 0);
            return;
        }
        if (!output->graph_rate_verified) {
            output->graph_rate_verified = 1;
            pw_thread_loop_signal(output->loop, false);
        }
    }
    if (output->drain_requested && !output->filled) {
        send_drain(output);
        return;
    }
    buffer = pw_stream_dequeue_buffer(output->stream);
    if (!buffer) return;
    if (buffer->buffer->n_datas != 1 || !buffer->buffer->datas[0].data ||
        !buffer->buffer->datas[0].chunk || buffer->buffer->datas[0].maxsize < output->format.block_align) {
        fail(output, "PipeWire supplied an invalid interleaved audio buffer", 0);
        pw_stream_queue_buffer(output->stream, buffer);
        return;
    }
    dst = &buffer->buffer->datas[0];
    frames = dst->maxsize / output->format.block_align;
    if (buffer->requested && buffer->requested < frames) frames = (size_t)buffer->requested;
    count = frames < output->filled ? frames : output->filled;
    first = output->capacity - output->read_pos;
    if (first > count) first = count;
    memcpy(dst->data, output->queue + output->read_pos * output->format.block_align,
           first * output->format.block_align);
    memcpy((uint8_t*)dst->data + first * output->format.block_align, output->queue,
           (count - first) * output->format.block_align);
    output->read_pos = (output->read_pos + count) % output->capacity;
    output->filled -= count;
    if (output->drain_requested) frames = count;
    else if (frames > count) {
        /* PCM U8 silence is 128, rather than zero. */
        memset((uint8_t*)dst->data + count * output->format.block_align,
               output->raw.format == SPA_AUDIO_FORMAT_U8 ? 0x80 : 0,
               (frames - count) * output->format.block_align);
    }
    dst->chunk->offset = 0;
    dst->chunk->stride = output->format.block_align;
    dst->chunk->size = (uint32_t)(frames * output->format.block_align);
    dst->chunk->flags = 0;
    buffer->size = frames;
    int err = pw_stream_queue_buffer(output->stream, buffer);
    if (err < 0) fail(output, "Cannot queue PipeWire audio buffer", err);
    else send_drain(output);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
    .drained = on_drained,
};

static int prepare_format(PipewireOutput* output, const AudioFormat* format) {
    size_t bytes;
    static const uint32_t wave_channels[] = {
        SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC, SPA_AUDIO_CHANNEL_LFE,
        SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR, SPA_AUDIO_CHANNEL_FLC, SPA_AUDIO_CHANNEL_FRC,
        SPA_AUDIO_CHANNEL_RC, SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR, SPA_AUDIO_CHANNEL_TC,
        SPA_AUDIO_CHANNEL_TFL, SPA_AUDIO_CHANNEL_TFC, SPA_AUDIO_CHANNEL_TFR,
        SPA_AUDIO_CHANNEL_TRL, SPA_AUDIO_CHANNEL_TRC, SPA_AUDIO_CHANNEL_TRR,
    };
    /* WAVE/FLAC standard ordering for files without an explicit mask. */
    static const uint32_t masks[] = {0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3f, 0x70f, 0x63f};
    if (!format || !format->num_channels || format->num_channels > 8 ||
        format->sample_rate < 8000 || format->sample_rate > 192000 ||
        !format->block_align || format->block_align % format->num_channels ||
        format->valid_bits_per_sample > format->bits_per_sample)
        return fail(output, "Invalid PipeWire PCM format", 0);
    bytes = format->block_align / format->num_channels;
    output->format = *format;
    output->raw.rate = format->sample_rate;
    output->raw.channels = format->num_channels;
    if (format->is_float && format->bits_per_sample == 32 && bytes == 4)
        output->raw.format = SPA_AUDIO_FORMAT_F32_LE;
    else if (!format->is_float) {
        if (format->bits_per_sample == 8 && bytes == 1) output->raw.format = SPA_AUDIO_FORMAT_U8;
        if (format->bits_per_sample == 16 && bytes == 2) output->raw.format = SPA_AUDIO_FORMAT_S16_LE;
        if (format->bits_per_sample == 24 && bytes == 3) output->raw.format = SPA_AUDIO_FORMAT_S24_LE;
        if (format->bits_per_sample == 24 && bytes == 4) output->raw.format = SPA_AUDIO_FORMAT_S24_32_LE;
        if (format->bits_per_sample == 32 && bytes == 4) output->raw.format = SPA_AUDIO_FORMAT_S32_LE;
    }
    if (!output->raw.format) return fail(output, "Unsupported PipeWire sample representation", 0);
    uint32_t mask = format->channel_mask ? format->channel_mask : masks[format->num_channels];
    uint32_t channels = 0;
    for (size_t i = 0; i < SPA_N_ELEMENTS(wave_channels); ++i)
        if (mask & (1u << i)) {
            if (channels >= format->num_channels) return fail(output, "Invalid PCM channel mask", 0);
            output->raw.position[channels++] = wave_channels[i];
        }
    if (channels != format->num_channels || (mask >> SPA_N_ELEMENTS(wave_channels)))
        return fail(output, "Unsupported PCM channel mask", 0);
    if (!format->channel_mask && format->num_channels == 1) output->raw.position[0] = SPA_AUDIO_CHANNEL_MONO;
    return 0;
}

/* Called before starting the loop. Every wait below has an absolute deadline. */
static int connect_server(PipewireOutput* output, const char* remote) {
    /* Since 0.3.49 the library reference-counts init/deinit. Balance each
     * connection while keeping other live outputs' references intact. */
    pthread_mutex_lock(&init_lock);
    pw_init(NULL, NULL);
    output->initialized = 1;
    pthread_mutex_unlock(&init_lock);
    output->loop = pw_thread_loop_new("sapfs-pipewire", NULL);
    if (!output->loop) return fail(output, "Cannot allocate PipeWire thread loop", -errno);
    output->context = pw_context_new(pw_thread_loop_get_loop(output->loop), NULL, 0);
    if (!output->context) return fail(output, "Cannot allocate PipeWire context", -errno);
    struct pw_properties* props = remote ? pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL) : NULL;
    if (remote && !props) return fail(output, "Cannot allocate PipeWire remote properties", -ENOMEM);
    output->core = pw_context_connect(output->context, props, 0);
    if (!output->core) return fail(output, "Cannot connect to PipeWire server", -errno);
    pw_core_add_listener(output->core, &output->core_listener, &core_events, output);
    output->core_listening = 1;
    return 0;
}

static int start_loop(PipewireOutput* output) {
    int err = pw_thread_loop_start(output->loop);
    if (err < 0) return fail(output, "Cannot start PipeWire thread loop", err);
    output->loop_started = 1;
    return 0;
}

static int wait_ready(PipewireOutput* output, int active) {
    struct timespec deadline;
    pw_thread_loop_get_time(output->loop, &deadline, (int64_t)output->timeout_ms * SPA_NSEC_PER_MSEC);
    while (!output->failed && (active ? output->stream_state != PW_STREAM_STATE_STREAMING || !output->format_ready ||
                                       (output->mode == PIPEWIRE_MODE_EXCLUSIVE && !output->graph_rate_verified)
                                    : output->stream_state < PW_STREAM_STATE_PAUSED)) {
        int err = pw_thread_loop_timed_wait_full(output->loop, &deadline);
        if (err < 0) return fail(output, active
            ? "PipeWire playback did not start; check the target, session manager, exclusive access and supported graph rate"
            : "Timed out connecting the PipeWire stream", err);
    }
    return output->failed ? -1 : 0;
}

PipewireOutput* pipewire_output_create(const AudioFormat* format, const PipewireConfig* config) {
    PipewireOutput* output = calloc(1, sizeof(*output));
    unsigned int buffer_ms = config && config->buffer_duration_ms ? config->buffer_duration_ms : 100;
    struct pw_properties* props;
    enum pw_stream_flags flags = PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_MAP_BUFFERS;
    uint8_t pod_buffer[2048];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    const struct spa_pod* params[1];
    int err;
    last_error[0] = '\0';
    if (!output) {
        snprintf(last_error, sizeof(last_error), "Cannot allocate PipeWire output");
        return NULL;
    }
    output->mode = config ? config->mode : PIPEWIRE_MODE_SHARED;
    output->timeout_ms = config && config->connect_timeout_ms ? config->connect_timeout_ms : 5000;
    if ((output->mode != PIPEWIRE_MODE_SHARED && output->mode != PIPEWIRE_MODE_EXCLUSIVE) ||
        buffer_ms < 10 || buffer_ms > 5000 || output->timeout_ms > 60000) {
        fail(output, "Invalid PipeWire configuration", 0);
        goto error;
    }
    if (config && ((config->target && !config->target[0]) ||
                   (config->remote_name && !config->remote_name[0]))) {
        fail(output, "PipeWire target and remote names must be nonempty or NULL", 0);
        goto error;
    }
    if (prepare_format(output, format) < 0) goto error;
    if (output->mode == PIPEWIRE_MODE_EXCLUSIVE && !format->is_float &&
        (format->valid_bits_per_sample ? format->valid_bits_per_sample : format->bits_per_sample) > 24) {
        fail(output, "PipeWire's float32 graph cannot preserve full 32-bit integer precision; use exclusive ALSA", 0);
        goto error;
    }
    output->capacity = ((size_t)format->sample_rate * buffer_ms + 999) / 1000;
    output->queue = malloc(output->capacity * format->block_align);
    if (!output->queue) {
        fail(output, "Cannot allocate PipeWire PCM queue", -ENOMEM);
        goto error;
    }
    if (connect_server(output, config ? config->remote_name : NULL) < 0) goto error;
    props = pw_properties_new(PW_KEY_APP_NAME, "sapfs", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_MEDIA_NAME, config && config->stream_name ? config->stream_name : "sapfs",
        PW_KEY_NODE_NAME, "sapfs-playback", "state.restore-props", "false", NULL);
    if (!props) {
        fail(output, "Cannot allocate PipeWire stream properties", -ENOMEM);
        goto error;
    }
    if (config && config->target) pw_properties_set(props, PW_KEY_TARGET_OBJECT, config->target);
    pw_properties_setf(props, PW_KEY_NODE_LATENCY, "%zu/%u", output->capacity, format->sample_rate);
    pw_properties_setf(props, PW_KEY_NODE_RATE, "1/%u", format->sample_rate);
    if (output->mode == PIPEWIRE_MODE_EXCLUSIVE) {
        flags |= PW_STREAM_FLAG_EXCLUSIVE;
        pw_properties_set(props, PW_KEY_NODE_EXCLUSIVE, "true");
        pw_properties_set(props, "node.dont-move", "true");
        pw_properties_set(props, PW_KEY_NODE_LOCK_RATE, "true");
        pw_properties_set(props, "resample.disable", "true");
        pw_properties_set(props, "stream.dont-remix", "true");
        pw_properties_set(props, "channelmix.disable", "true");
        pw_properties_set(props, "dither.noise", "0");
        pw_properties_set(props, "dither.method", "none");
    }
    if (output->mode == PIPEWIRE_MODE_EXCLUSIVE || (config && config->target)) {
        flags |= PW_STREAM_FLAG_DONT_RECONNECT;
        pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
        pw_properties_set(props, "node.dont-fallback", "true");
    }
    output->stream = pw_stream_new(output->core, "sapfs", props);
    if (!output->stream) {
        fail(output, "Cannot create PipeWire playback stream", -errno);
        goto error;
    }
    pw_stream_add_listener(output->stream, &output->stream_listener, &stream_events, output);
    output->stream_listening = 1;
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &output->raw);
    err = pw_stream_connect(output->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
    if (err < 0) {
        fail(output, "Cannot connect PipeWire playback stream", err);
        goto error;
    }
    pw_thread_loop_lock(output->loop);
    err = start_loop(output);
    if (err == 0) err = wait_ready(output, 0);
    pw_thread_loop_unlock(output->loop);
    if (err < 0) goto error;
    return output;
error:
    snprintf(last_error, sizeof(last_error), "%s", output->error_msg);
    pipewire_output_close(output);
    return NULL;
}

static int activate(PipewireOutput* output) {
    output->graph_rate_verified = 0;
    output->last_process_ns = monotonic_ns();
    int err = pw_stream_set_active(output->stream, true);
    if (err < 0) return fail(output, "Cannot activate PipeWire playback", err);
    output->state = AUDIO_STATE_PLAYING;
    return wait_ready(output, 1);
}

int pipewire_output_start(PipewireOutput* output) {
    int result;
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    result = output->failed ? -1 : output->state == AUDIO_STATE_PLAYING ? 0 : activate(output);
    pw_thread_loop_unlock(output->loop);
    return result;
}

int pipewire_output_stop(PipewireOutput* output) {
    int err;
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    err = pw_stream_set_active(output->stream, false);
    if (err >= 0) err = pw_stream_flush(output->stream, false);
    output->state = AUDIO_STATE_STOPPED;
    output->filled = output->read_pos = output->write_pos = 0;
    output->drain_requested = output->drain_sent = output->drained = 0;
    if (err < 0) fail(output, "Cannot stop PipeWire playback", err);
    pw_thread_loop_unlock(output->loop);
    return err < 0 ? -1 : 0;
}

int pipewire_output_pause(PipewireOutput* output) {
    int err = -1;
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    if (!output->failed && output->state == AUDIO_STATE_PLAYING) {
        err = pw_stream_set_active(output->stream, false);
        if (err < 0) fail(output, "Cannot pause PipeWire playback", err);
        else output->state = AUDIO_STATE_PAUSED;
    }
    pw_thread_loop_unlock(output->loop);
    return err < 0 ? -1 : 0;
}

int pipewire_output_resume(PipewireOutput* output) {
    int result = -1;
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    if (!output->failed && output->state == AUDIO_STATE_PAUSED) result = activate(output);
    pw_thread_loop_unlock(output->loop);
    return result;
}

int pipewire_output_write(PipewireOutput* output, const void* data, size_t num_frames) {
    size_t first;
    int result = -1;
    if (!output || (!data && num_frames)) return -1;
    pw_thread_loop_lock(output->loop);
    if (check_health(output) < 0 || output->state != AUDIO_STATE_PLAYING) goto done;
    if (!num_frames) { result = 0; goto done; }
    if (output->drain_requested) {
        if (!output->drained) { result = 0; goto done; }
        output->drain_requested = output->drain_sent = output->drained = 0;
        if (activate(output) < 0) goto done;
    }
    if (num_frames > output->capacity - output->filled) num_frames = output->capacity - output->filled;
    if (num_frames > 4096) num_frames = 4096;
    first = output->capacity - output->write_pos;
    if (first > num_frames) first = num_frames;
    memcpy(output->queue + output->write_pos * output->format.block_align, data, first * output->format.block_align);
    memcpy(output->queue, (const uint8_t*)data + first * output->format.block_align,
           (num_frames - first) * output->format.block_align);
    output->write_pos = (output->write_pos + num_frames) % output->capacity;
    output->filled += num_frames;
    result = (int)num_frames;
done:
    pw_thread_loop_unlock(output->loop);
    return result;
}

AudioState pipewire_output_get_state(PipewireOutput* output) {
    AudioState state;
    if (!output) return AUDIO_STATE_STOPPED;
    pw_thread_loop_lock(output->loop);
    state = output->failed ? AUDIO_STATE_STOPPED : output->state;
    pw_thread_loop_unlock(output->loop);
    return state;
}

int pipewire_output_get_available_frames(PipewireOutput* output) {
    int frames;
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    frames = check_health(output) < 0 ? -1 : (int)(output->capacity - output->filled);
    pw_thread_loop_unlock(output->loop);
    return frames;
}

int pipewire_output_get_queued_frames(PipewireOutput* output) {
    int frames;
    struct pw_time time = {0};
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    double queued = (double)output->filled;
    if (!output->drained && output->state != AUDIO_STATE_STOPPED &&
        pw_stream_get_time_n(output->stream, &time, sizeof(time)) == 0) {
        queued += (double)time.queued + (double)time.buffered;
        if (time.delay > 0 && time.rate.denom)
            queued += (double)time.delay * time.rate.num * output->format.sample_rate / time.rate.denom;
    }
    frames = check_health(output) < 0 ? -1 : queued >= INT_MAX ? INT_MAX : (int)queued;
    pw_thread_loop_unlock(output->loop);
    return frames;
}

int pipewire_output_drain(PipewireOutput* output) {
    int result = -1;
    if (!output) return -1;
    pw_thread_loop_lock(output->loop);
    if (check_health(output) == 0 && output->state == AUDIO_STATE_PLAYING) {
        output->drain_requested = 1;
        if (send_drain(output) == 0) result = output->drained ? 1 : 0;
    }
    pw_thread_loop_unlock(output->loop);
    return result;
}

const char* pipewire_output_get_error(PipewireOutput* output) {
    if (!output) return pipewire_output_get_last_error();
    pw_thread_loop_lock(output->loop);
    snprintf(error_copy, sizeof(error_copy), "%s", output->error_msg);
    pw_thread_loop_unlock(output->loop);
    return error_copy[0] ? error_copy : NULL;
}

const char* pipewire_output_get_last_error(void) {
    return last_error[0] ? last_error : "Failed to initialize PipeWire output";
}

void pipewire_output_close(PipewireOutput* output) {
    if (!output) return;
    if (output->loop_started) pw_thread_loop_stop(output->loop);
    output->closing = 1;
    if (output->stream_listening) spa_hook_remove(&output->stream_listener);
    if (output->stream) pw_stream_destroy(output->stream);
    if (output->core_listening) spa_hook_remove(&output->core_listener);
    if (output->core) pw_core_disconnect(output->core);
    if (output->context) pw_context_destroy(output->context);
    if (output->loop) pw_thread_loop_destroy(output->loop);
    if (output->initialized) {
        pthread_mutex_lock(&init_lock);
        pw_deinit();
        pthread_mutex_unlock(&init_lock);
    }
    free(output->queue);
    free(output);
}

struct DeviceList {
    PipewireDeviceCallback callback;
    void* user_data;
};

static void on_global(void* data, uint32_t id, uint32_t permissions, const char* type,
                      uint32_t version, const struct spa_dict* props) {
    struct DeviceList* list = data;
    const char* media_class;
    (void)id; (void)permissions; (void)version;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) || !props) return;
    media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class || strcmp(media_class, "Audio/Sink")) return;
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    const char* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    if (name) list->callback(name, description ? description : name, serial ? serial : "", list->user_data);
}

int pipewire_output_list_devices(const char* remote_name, PipewireDeviceCallback callback, void* user_data) {
    PipewireOutput* output = calloc(1, sizeof(*output));
    struct pw_registry* registry = NULL;
    struct spa_hook listener = {0};
    struct DeviceList list = {callback, user_data};
    const struct pw_registry_events events = {PW_VERSION_REGISTRY_EVENTS, .global = on_global};
    struct timespec deadline;
    int listening = 0, result = -1;
    if (!output || !callback) {
        snprintf(last_error, sizeof(last_error), "Invalid PipeWire device list arguments or allocation failure");
        free(output);
        return -1;
    }
    if (connect_server(output, remote_name) < 0) goto done;
    registry = pw_core_get_registry(output->core, PW_VERSION_REGISTRY, 0);
    if (!registry) { fail(output, "Cannot query PipeWire registry", -errno); goto done; }
    pw_registry_add_listener(registry, &listener, &events, &list);
    listening = 1;
    output->sync_seq = pw_core_sync(output->core, PW_ID_CORE, 0);
    if (output->sync_seq < 0) { fail(output, "Cannot synchronize PipeWire registry", output->sync_seq); goto done; }
    pw_thread_loop_lock(output->loop);
    if (start_loop(output) == 0) {
        pw_thread_loop_get_time(output->loop, &deadline, 5 * SPA_NSEC_PER_SEC);
        while (!output->failed && !output->sync_done) {
            int err = pw_thread_loop_timed_wait_full(output->loop, &deadline);
            if (err < 0) { fail(output, "Timed out listing PipeWire sinks", err); break; }
        }
        if (!output->failed) result = 0;
    }
    pw_thread_loop_unlock(output->loop);
done:
    if (output->loop_started) { pw_thread_loop_stop(output->loop); output->loop_started = 0; }
    if (listening) spa_hook_remove(&listener);
    if (registry) pw_proxy_destroy((struct pw_proxy*)registry);
    if (result < 0) snprintf(last_error, sizeof(last_error), "%s", output->error_msg);
    else last_error[0] = '\0';
    pipewire_output_close(output);
    return result;
}

#endif
