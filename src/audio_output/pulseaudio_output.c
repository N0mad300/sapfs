#include "pulseaudio_output.h"

#if defined(__linux__) && defined(SAPFS_HAVE_PULSEAUDIO)

#include <pulse/pulseaudio.h>
#include <pulse/rtclock.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct PulseaudioOutput {
    pa_threaded_mainloop* loop;
    pa_context* context;
    pa_stream* stream;
    pa_operation* drain_op;
    pa_sample_spec spec;
    pa_channel_map map;
    AudioState state;
    unsigned int timeout_ms, capacity, underflows;
    size_t frame_size;
    int loop_started, failed, timed_out, operation_result, drain_result;
    int has_data;
    int64_t last_read;
    pa_usec_t progress_at, drain_deadline, pause_at;
    char error[512];
};

static _Thread_local char last_error[512];

/* Called with the mainloop locked once it is running. Recoverable caller errors
 * do not poison the stream. Fatal errors preserve their first useful diagnostic. */
static int error(PulseaudioOutput* o, const char* message, int fatal, int pulse_error) {
    if (!o->failed) snprintf(o->error, sizeof(o->error), "%s%s%s", message,
        pulse_error ? ": " : "", pulse_error ? pa_strerror(pa_context_errno(o->context)) : "");
    if (fatal) o->failed = 1;
    return -1;
}

static void wake(PulseaudioOutput* o) { pa_threaded_mainloop_signal(o->loop, 0); }
static void context_changed(pa_context* c, void* data) {
    PulseaudioOutput* o = data;
    pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED)
        error(o, "PulseAudio server connection lost or refused", 1, 1);
    wake(o);
}
static void stream_changed(pa_stream* s, void* data) {
    PulseaudioOutput* o = data;
    pa_stream_state_t state = pa_stream_get_state(s);
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED)
        error(o, "PulseAudio playback stream failed; check the selected sink and server", 1, 1);
    wake(o);
}
static void underflow(pa_stream* s, void* data) {
    (void)s;
    PulseaudioOutput* o = data;
    ++o->underflows;
}
static void operation_done(pa_stream* s, int success, void* data) {
    (void)s;
    PulseaudioOutput* o = data;
    o->operation_result = success ? 1 : -1;
    wake(o);
}
static void drain_done(pa_stream* s, int success, void* data) {
    (void)s;
    PulseaudioOutput* o = data;
    o->drain_result = success ? 1 : -1;
    wake(o);
}
static void timeout(pa_mainloop_api* api, pa_time_event* event,
                    const struct timeval* tv, void* data) {
    (void)api; (void)event; (void)tv;
    PulseaudioOutput* o = data;
    o->timed_out = 1;
    wake(o);
}
static pa_time_event* start_timer(PulseaudioOutput* o) {
    o->timed_out = 0;
    pa_time_event* event = pa_context_rttime_new(o->context,
        pa_rtclock_now() + (pa_usec_t)o->timeout_ms * 1000, timeout, o);
    if (!event) error(o, "Cannot allocate PulseAudio timeout", 1, 0);
    return event;
}
static void free_timer(PulseaudioOutput* o, pa_time_event* event) {
    pa_threaded_mainloop_get_api(o->loop)->time_free(event);
}
/* A cancelled operation cannot invoke callbacks later with stale userdata. */
static void cancel_operation(pa_operation** operation) {
    if (!*operation) return;
    if (pa_operation_get_state(*operation) == PA_OPERATION_RUNNING) pa_operation_cancel(*operation);
    pa_operation_unref(*operation);
    *operation = NULL;
}
static int wait_operation(PulseaudioOutput* o, pa_operation* operation, const char* message) {
    if (!operation) return error(o, message, 1, 1);
    pa_time_event* timer = start_timer(o);
    if (timer) {
        while (!o->failed && !o->timed_out && pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
            pa_threaded_mainloop_wait(o->loop);
        free_timer(o, timer);
        if (!o->failed && (o->timed_out || o->operation_result != 1))
            error(o, o->timed_out ? "PulseAudio control operation timed out" : message, 1, !o->timed_out);
    }
    cancel_operation(&operation);
    return o->failed ? -1 : 0;
}
static int cork(PulseaudioOutput* o, int value) {
    o->operation_result = 0;
    return wait_operation(o, pa_stream_cork(o->stream, value, operation_done, o),
                          "Cannot change PulseAudio playback state");
}

static int prepare_format(PulseaudioOutput* o, const AudioFormat* f) {
    static const pa_channel_position_t positions[] = {
        PA_CHANNEL_POSITION_FRONT_LEFT, PA_CHANNEL_POSITION_FRONT_RIGHT,
        PA_CHANNEL_POSITION_FRONT_CENTER, PA_CHANNEL_POSITION_LFE,
        PA_CHANNEL_POSITION_REAR_LEFT, PA_CHANNEL_POSITION_REAR_RIGHT,
        PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER, PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
        PA_CHANNEL_POSITION_REAR_CENTER, PA_CHANNEL_POSITION_SIDE_LEFT, PA_CHANNEL_POSITION_SIDE_RIGHT,
        PA_CHANNEL_POSITION_TOP_CENTER, PA_CHANNEL_POSITION_TOP_FRONT_LEFT,
        PA_CHANNEL_POSITION_TOP_FRONT_CENTER, PA_CHANNEL_POSITION_TOP_FRONT_RIGHT,
        PA_CHANNEL_POSITION_TOP_REAR_LEFT, PA_CHANNEL_POSITION_TOP_REAR_CENTER, PA_CHANNEL_POSITION_TOP_REAR_RIGHT
    };
    static const uint32_t masks[] = {0, 4, 3, 7, 0x33, 0x37, 0x3f, 0x70f, 0x63f};
    if (!f || f->num_channels < 1 || f->num_channels > 8 ||
        f->sample_rate < 8000 || f->sample_rate > 192000 || !f->block_align ||
        f->block_align % f->num_channels || f->valid_bits_per_sample > f->bits_per_sample)
        return error(o, "Invalid PulseAudio PCM format", 1, 0);
    size_t bytes = f->block_align / f->num_channels;
    o->spec = (pa_sample_spec){PA_SAMPLE_INVALID, f->sample_rate, (uint8_t)f->num_channels};
    if (f->is_float && f->bits_per_sample == 32 && bytes == 4) o->spec.format = PA_SAMPLE_FLOAT32LE;
    else if (!f->is_float) {
        if (f->bits_per_sample == 8 && bytes == 1) o->spec.format = PA_SAMPLE_U8;
        if (f->bits_per_sample == 16 && bytes == 2) o->spec.format = PA_SAMPLE_S16LE;
        if (f->bits_per_sample == 24 && bytes == 3) o->spec.format = PA_SAMPLE_S24LE;
        if (f->bits_per_sample == 24 && bytes == 4) o->spec.format = PA_SAMPLE_S24_32LE;
        if (f->bits_per_sample == 32 && bytes == 4) o->spec.format = PA_SAMPLE_S32LE;
    }
    if (!pa_sample_spec_valid(&o->spec)) return error(o, "Unsupported PulseAudio sample representation", 1, 0);
    o->frame_size = pa_frame_size(&o->spec);
    pa_channel_map_init(&o->map);
    o->map.channels = o->spec.channels;
    uint32_t mask = f->channel_mask ? f->channel_mask : masks[f->num_channels];
    unsigned int channel = 0;
    for (unsigned int i = 0; i < sizeof(positions) / sizeof(*positions); ++i)
        if (mask & (1u << i)) {
            if (channel == f->num_channels) return error(o, "Invalid PCM channel mask", 1, 0);
            o->map.map[channel++] = positions[i];
        }
    if (channel != f->num_channels || mask >> (sizeof(positions) / sizeof(*positions)))
        return error(o, "Unsupported PCM channel mask", 1, 0);
    if (!f->channel_mask && f->num_channels == 1) o->map.map[0] = PA_CHANNEL_POSITION_MONO;
    return pa_channel_map_valid(&o->map) ? 0 : error(o, "Invalid PulseAudio channel map", 1, 0);
}

/* Returns with the loop running, unlocked. Each client has independent lifetime. */
static int connect_server(PulseaudioOutput* o, const char* server, const char* name) {
    o->loop = pa_threaded_mainloop_new();
    if (!o->loop) return error(o, "Cannot allocate PulseAudio mainloop", 1, 0);
    o->context = pa_context_new(pa_threaded_mainloop_get_api(o->loop), name ? name : "sapfs");
    if (!o->context) return error(o, "Cannot allocate PulseAudio context", 1, 0);
    pa_context_set_state_callback(o->context, context_changed, o);
    if (pa_threaded_mainloop_start(o->loop) < 0) return error(o, "Cannot start PulseAudio mainloop", 1, 0);
    o->loop_started = 1;
    pa_threaded_mainloop_lock(o->loop);
    if (pa_context_connect(o->context, server, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0)
        error(o, "Cannot connect to PulseAudio server", 1, 1);
    pa_time_event* timer = o->failed ? NULL : start_timer(o);
    if (timer) {
        while (!o->failed && !o->timed_out && pa_context_get_state(o->context) != PA_CONTEXT_READY)
            pa_threaded_mainloop_wait(o->loop);
        free_timer(o, timer);
        if (o->timed_out) error(o, "Timed out connecting to PulseAudio server", 1, 0);
    }
    pa_threaded_mainloop_unlock(o->loop);
    return o->failed ? -1 : 0;
}

PulseaudioOutput* pulseaudio_output_create(const AudioFormat* format, const PulseaudioConfig* config) {
    last_error[0] = '\0';
    PulseaudioOutput* o = calloc(1, sizeof(*o));
    if (!o) { snprintf(last_error, sizeof(last_error), "Cannot allocate PulseAudio output"); return NULL; }
    unsigned int ms = config && config->buffer_duration_ms ? config->buffer_duration_ms : 100;
    o->timeout_ms = config && config->timeout_ms ? config->timeout_ms : 5000;
    if (ms < 10 || ms > 5000 || o->timeout_ms > 60000 ||
        (config && ((config->device && !*config->device) || (config->server && !*config->server) ||
                    (config->application_name && !*config->application_name) || (config->stream_name && !*config->stream_name)))) {
        error(o, "Invalid PulseAudio configuration", 1, 0);
        goto failure;
    }
    if (prepare_format(o, format) < 0) goto failure;
    o->capacity = (unsigned int)(((uint64_t)o->spec.rate * ms + 999) / 1000);
    if (connect_server(o, config ? config->server : NULL, config ? config->application_name : NULL) < 0) goto failure;
    pa_threaded_mainloop_lock(o->loop);
    pa_proplist* props = pa_proplist_new();
    if (!props) error(o, "Cannot allocate PulseAudio stream properties", 1, 0);
    else {
        pa_proplist_sets(props, PA_PROP_MEDIA_ROLE, "music");
        o->stream = pa_stream_new_with_proplist(o->context,
            config && config->stream_name ? config->stream_name : "sapfs", &o->spec, &o->map, props);
        pa_proplist_free(props);
        if (!o->stream) error(o, "Cannot create PulseAudio playback stream", 1, 1);
    }
    if (!o->failed) {
        pa_stream_set_state_callback(o->stream, stream_changed, o);
        pa_stream_set_underflow_callback(o->stream, underflow, o);
        uint32_t length = (uint32_t)(o->capacity * o->frame_size);
        uint32_t request = length / 4 / (uint32_t)o->frame_size * (uint32_t)o->frame_size;
        /* Automatic prebuffering protects samples after underruns. Server drain
         * forces playback even for a segment shorter than the prebuffer. */
        pa_buffer_attr attr = {length, length, UINT32_MAX, request, UINT32_MAX};
        pa_stream_flags_t flags = PA_STREAM_START_CORKED | PA_STREAM_AUTO_TIMING_UPDATE |
                                  PA_STREAM_INTERPOLATE_TIMING;
        if (config && config->device) flags |= PA_STREAM_DONT_MOVE;
        if (pa_stream_connect_playback(o->stream, config ? config->device : NULL, &attr, flags, NULL, NULL) < 0)
            error(o, "Cannot connect PulseAudio playback sink", 1, 1);
        pa_time_event* timer = o->failed ? NULL : start_timer(o);
        if (timer) {
            while (!o->failed && !o->timed_out && pa_stream_get_state(o->stream) != PA_STREAM_READY)
                pa_threaded_mainloop_wait(o->loop);
            free_timer(o, timer);
            if (o->timed_out) error(o, "Timed out connecting PulseAudio playback sink", 1, 0);
        }
        if (!o->failed && (!pa_sample_spec_equal(&o->spec, pa_stream_get_sample_spec(o->stream)) ||
                           !pa_channel_map_equal(&o->map, pa_stream_get_channel_map(o->stream))))
            error(o, "PulseAudio changed the requested application PCM format or channel map", 1, 0);
    }
    pa_threaded_mainloop_unlock(o->loop);
    if (o->failed) goto failure;
    return o;
failure:
    snprintf(last_error, sizeof(last_error), "%s", o->error);
    pulseaudio_output_close(o);
    return NULL;
}

/* Server timing indexes advance in source bytes. Only time out when the caller
 * cannot submit more data or is draining, so deliberate application gaps and
 * automatic prebuffering never count as a stalled server. */
static int healthy(PulseaudioOutput* o) {
    if (o->failed) return -1;
    if (o->state != AUDIO_STATE_PLAYING || !o->has_data || o->drain_result == 1) return 0;
    size_t available = pa_stream_writable_size(o->stream);
    if (available == (size_t)-1) return error(o, "Cannot query PulseAudio writable size", 1, 1);
    const pa_timing_info* timing = pa_stream_get_timing_info(o->stream);
    pa_usec_t now = pa_rtclock_now();
    if (o->drain_op && now > o->drain_deadline)
        return error(o, "PulseAudio drain timed out", 1, 0);
    if (!o->drain_op && available >= o->frame_size) o->progress_at = now;
    else if (timing && !timing->read_index_corrupt && timing->read_index != o->last_read) {
        o->last_read = timing->read_index;
        o->progress_at = now;
    } else if (now - o->progress_at > (pa_usec_t)o->timeout_ms * 1000)
        return error(o, "PulseAudio playback stalled; check sink suspension and server health", 1, 0);
    return 0;
}

static void mark_playing(PulseaudioOutput* o) {
    pa_usec_t now = pa_rtclock_now();
    if (o->state == AUDIO_STATE_PAUSED && o->drain_op) o->drain_deadline += now - o->pause_at;
    o->state = AUDIO_STATE_PLAYING;
    o->progress_at = now;
}

int pulseaudio_output_start(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = healthy(o);
    if (!result && o->state != AUDIO_STATE_PLAYING) {
        result = cork(o, 0);
        if (!result) mark_playing(o);
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
int pulseaudio_output_pause(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = healthy(o);
    if (!result && o->state != AUDIO_STATE_PLAYING) result = error(o, "PulseAudio output is not playing", 0, 0);
    if (!result) {
        result = cork(o, 1);
        if (!result) { o->state = AUDIO_STATE_PAUSED; o->pause_at = pa_rtclock_now(); }
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
int pulseaudio_output_resume(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = o->failed ? -1 : o->state != AUDIO_STATE_PAUSED ? error(o, "PulseAudio output is not paused", 0, 0) : 0;
    if (!result) {
        result = cork(o, 0);
        if (!result) mark_playing(o);
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
int pulseaudio_output_stop(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = o->failed ? -1 : 0;
    cancel_operation(&o->drain_op);
    if (!result && o->state != AUDIO_STATE_STOPPED) {
        result = cork(o, 1);
        if (!result) {
            o->operation_result = 0;
            result = wait_operation(o, pa_stream_flush(o->stream, operation_done, o), "Cannot flush PulseAudio playback");
        }
    }
    if (!result) { o->state = AUDIO_STATE_STOPPED; o->has_data = 0; o->drain_result = 0; }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
int pulseaudio_output_write(PulseaudioOutput* o, const void* data, size_t frames) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = healthy(o);
    if (!result && o->state != AUDIO_STATE_PLAYING) result = error(o, "PulseAudio output is not playing", 0, 0);
    if (!result && frames && !data) result = error(o, "Invalid PulseAudio PCM data", 0, 0);
    if (!result && frames && o->drain_op && o->drain_result != 1)
        result = error(o, "PulseAudio drain is still pending", 0, 0);
    if (!result && frames) {
        size_t available = pa_stream_writable_size(o->stream);
        if (available == (size_t)-1) result = error(o, "Cannot query PulseAudio writable size", 1, 1);
        else {
            if (frames > available / o->frame_size) frames = available / o->frame_size;
            if (frames > o->capacity) frames = o->capacity;
            if (frames > 4096) frames = 4096;
            if (frames) {
                if (o->drain_result == 1) { cancel_operation(&o->drain_op); o->drain_result = 0; o->has_data = 0; }
                if (!o->has_data) o->progress_at = pa_rtclock_now();
                if (pa_stream_write(o->stream, data, frames * o->frame_size, NULL, 0, PA_SEEK_RELATIVE) < 0)
                    result = error(o, "Cannot write PulseAudio PCM", 1, 1);
                else { o->has_data = 1; result = (int)frames; }
            }
        }
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
AudioState pulseaudio_output_get_state(PulseaudioOutput* o) {
    if (!o) return AUDIO_STATE_STOPPED;
    pa_threaded_mainloop_lock(o->loop);
    AudioState state = healthy(o) < 0 ? AUDIO_STATE_STOPPED : o->state;
    pa_threaded_mainloop_unlock(o->loop);
    return state;
}
int pulseaudio_output_get_available_frames(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = healthy(o);
    if (!result) {
        size_t bytes = pa_stream_writable_size(o->stream);
        if (bytes == (size_t)-1) result = error(o, "Cannot query PulseAudio writable size", 1, 1);
        else result = (int)(bytes / o->frame_size > o->capacity ? o->capacity : bytes / o->frame_size);
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
int pulseaudio_output_get_queued_frames(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = healthy(o);
    if (!result && o->has_data && o->drain_result != 1) {
        pa_usec_t latency;
        int negative;
        if (pa_stream_get_latency(o->stream, &latency, &negative) == 0) {
            if (negative) result = 0;
            else if (latency > (pa_usec_t)INT_MAX * 1000000 / o->spec.rate) result = INT_MAX;
            else result = (int)((latency * o->spec.rate + 999999) / 1000000);
        } else {
            /* Timing arrives asynchronously; writable size gives a conservative
             * per-stream estimate until the first timing reply is available. */
            const pa_buffer_attr* attr = pa_stream_get_buffer_attr(o->stream);
            size_t available = pa_stream_writable_size(o->stream);
            if (!attr || available == (size_t)-1) result = error(o, "Cannot query PulseAudio queue", 1, 1);
            else {
                size_t frames = available >= attr->tlength ? 0 : (attr->tlength - available) / o->frame_size;
                result = frames > INT_MAX ? INT_MAX : (int)frames;
            }
        }
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
int pulseaudio_output_drain(PulseaudioOutput* o) {
    if (!o) return -1;
    pa_threaded_mainloop_lock(o->loop);
    int result = healthy(o);
    if (!result && o->state != AUDIO_STATE_PLAYING) result = error(o, "PulseAudio output is not playing", 0, 0);
    if (!result && !o->has_data) result = 1;
    else if (!result) {
        if (!o->drain_op) {
            o->drain_result = 0;
            o->progress_at = pa_rtclock_now();
            const pa_buffer_attr* attr = pa_stream_get_buffer_attr(o->stream);
            pa_usec_t queued = pa_bytes_to_usec(attr ? attr->maxlength : o->capacity * o->frame_size, &o->spec);
            pa_usec_t latency = 0;
            int negative;
            if (pa_stream_get_latency(o->stream, &latency, &negative) == 0 && !negative && latency > queued)
                queued = latency;
            /* A live server may keep sending timing replies even if it never
             * acknowledges EOF. Bound that case as well as a frozen server. */
            o->drain_deadline = o->progress_at + queued + (pa_usec_t)o->timeout_ms * 1000;
            o->drain_op = pa_stream_drain(o->stream, drain_done, o);
            if (!o->drain_op) error(o, "Cannot drain PulseAudio stream", 1, 1);
        }
        if (o->drain_result < 0) error(o, "PulseAudio stream drain failed", 1, 1);
        result = o->failed ? -1 : o->drain_result;
    }
    pa_threaded_mainloop_unlock(o->loop);
    return result;
}
const char* pulseaudio_output_get_error(PulseaudioOutput* o) {
    if (!o) return pulseaudio_output_get_last_error();
    /* API calls have one owner; callbacks can update errors under the lock. */
    static _Thread_local char snapshot[512];
    pa_threaded_mainloop_lock(o->loop);
    healthy(o);
    snprintf(snapshot, sizeof(snapshot), "%s", o->error);
    pa_threaded_mainloop_unlock(o->loop);
    return snapshot;
}
const char* pulseaudio_output_get_last_error(void) { return last_error; }
unsigned int pulseaudio_output_get_underflow_count(PulseaudioOutput* o) {
    if (!o) return 0;
    pa_threaded_mainloop_lock(o->loop);
    unsigned int count = o->underflows;
    pa_threaded_mainloop_unlock(o->loop);
    return count;
}
void pulseaudio_output_close(PulseaudioOutput* o) {
    if (!o) return;
    if (o->loop_started) pa_threaded_mainloop_lock(o->loop);
    cancel_operation(&o->drain_op);
    if (o->stream) {
        pa_stream_set_state_callback(o->stream, NULL, NULL);
        pa_stream_set_underflow_callback(o->stream, NULL, NULL);
        pa_stream_disconnect(o->stream);
        pa_stream_unref(o->stream);
    }
    if (o->context) {
        pa_context_set_state_callback(o->context, NULL, NULL);
        pa_context_disconnect(o->context);
        pa_context_unref(o->context);
    }
    if (o->loop_started) {
        pa_threaded_mainloop_unlock(o->loop);
        pa_threaded_mainloop_stop(o->loop);
    }
    if (o->loop) pa_threaded_mainloop_free(o->loop);
    free(o);
}

typedef struct Device {
    char* name;
    char* description;
    unsigned int index;
    struct Device* next;
} Device;
typedef struct {
    PulseaudioOutput* output;
    Device* head;
    Device** tail;
    int done;
} DeviceList;
static void sink_info(pa_context* c, const pa_sink_info* info, int eol, void* data) {
    (void)c;
    DeviceList* list = data;
    if (eol) {
        list->done = 1;
        if (eol < 0) error(list->output, "Cannot enumerate PulseAudio sinks", 1, 1);
    } else if (!list->output->failed && info) {
        Device* device = calloc(1, sizeof(*device));
        if (device) {
            device->name = strdup(info->name ? info->name : "");
            device->description = strdup(info->description ? info->description : device->name ? device->name : "");
        }
        if (!device || !device->name || !device->description) {
            if (device) { free(device->name); free(device->description); free(device); }
            error(list->output, "Cannot allocate PulseAudio sink list", 1, 0);
        } else {
            device->index = info->index;
            *list->tail = device;
            list->tail = &device->next;
        }
    }
    wake(list->output);
}
int pulseaudio_output_list_devices(const char* server, PulseaudioDeviceCallback callback, void* data) {
    last_error[0] = '\0';
    if ((server && !*server) || !callback) {
        snprintf(last_error, sizeof(last_error), "Invalid PulseAudio sink listing arguments");
        return -1;
    }
    PulseaudioOutput* o = calloc(1, sizeof(*o));
    if (!o) { snprintf(last_error, sizeof(last_error), "Cannot allocate PulseAudio listing context"); return -1; }
    o->timeout_ms = 5000;
    DeviceList list = {.output = o};
    list.tail = &list.head;
    if (connect_server(o, server, "sapfs-list") == 0) {
        pa_threaded_mainloop_lock(o->loop);
        pa_operation* operation = pa_context_get_sink_info_list(o->context, sink_info, &list);
        if (!operation) error(o, "Cannot request PulseAudio sink list", 1, 1);
        pa_time_event* timer = o->failed ? NULL : start_timer(o);
        if (timer) {
            while (!o->failed && !o->timed_out && !list.done && pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
                pa_threaded_mainloop_wait(o->loop);
            free_timer(o, timer);
            if (!o->failed && (!list.done || o->timed_out)) error(o, "Timed out enumerating PulseAudio sinks", 1, 0);
        }
        cancel_operation(&operation);
        pa_threaded_mainloop_unlock(o->loop);
    }
    int result = o->failed ? -1 : 0;
    if (result < 0) snprintf(last_error, sizeof(last_error), "%s", o->error);
    pulseaudio_output_close(o);
    while (list.head) {
        Device* device = list.head;
        list.head = device->next;
        if (!result) callback(device->name, device->description, device->index, data);
        free(device->name); free(device->description); free(device);
    }
    return result;
}
#endif
