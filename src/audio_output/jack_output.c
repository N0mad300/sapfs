#include "jack_output.h"

#if defined(__linux__) && defined(SAPFS_HAVE_JACK)

#include <jack/jack.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(UINT_MAX == UINT32_MAX, "JACK FIFO requires 32-bit unsigned indices");
_Static_assert(sizeof(jack_default_audio_sample_t) == sizeof(float) && sizeof(float) == 4,
               "JACK audio must use 32-bit floats");

enum JackFault { FAULT_NONE, FAULT_SHUTDOWN, FAULT_RATE, FAULT_BUFFER, FAULT_LATENCY, FAULT_PERIOD };

struct JackOutput {
    jack_client_t* client;
    jack_port_t* outputs[8];
    char* destinations[8];
    AudioFormat format;
    JackConnectionMode connection_mode;
    unsigned int timeout_ms;
    int active;
    int failed;
    char error_msg[512];
    float* queue;                 /* Interleaved; consumer deinterleaves to JACK ports. */
    unsigned int capacity;        /* Power of two, less than half the index range. */
    atomic_uint read_index;
    atomic_uint write_index;
    atomic_uint requested_state;
    atomic_uint actual_state;     /* Acknowledged at the END of a process callback. */
    atomic_uint fault;
    atomic_uint shutdown;
    atomic_uint server_rate;
    atomic_uint period;
    atomic_uint latency;
    atomic_uint tail_frames;
    atomic_uint cycles;
    atomic_uint xruns;
    atomic_uint graph_version;
    atomic_uint graph_seen;
    atomic_uint connected;
    unsigned int last_cycles;     /* Owner-thread watchdog state. */
    uint64_t last_progress_ns;
    uint64_t disconnected_ns;
};

static _Thread_local char last_error[512];

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void sleep_ms(unsigned int ms) {
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}

static int fail(JackOutput* output, const char* message) {
    if (!output->failed) snprintf(output->error_msg, sizeof(output->error_msg), "%s", message);
    output->failed = 1;
    atomic_store_explicit(&output->requested_state, AUDIO_STATE_STOPPED, memory_order_release);
    return -1;
}

/* Callback code only records lock-free scalar state. No logging, allocation,
 * mutexes, sleeping, conversion, port discovery, or server control in RT code. */
static void record_fault(JackOutput* output, unsigned int fault) {
    unsigned int expected = FAULT_NONE;
    atomic_compare_exchange_strong_explicit(&output->fault, &expected, fault,
                                            memory_order_release, memory_order_relaxed);
}

static void on_shutdown(void* data) {
    JackOutput* output = data;
    atomic_store_explicit(&output->shutdown, 1, memory_order_release);
    record_fault(output, FAULT_SHUTDOWN);
}

static int on_sample_rate(jack_nframes_t rate, void* data) {
    JackOutput* output = data;
    atomic_store_explicit(&output->server_rate, rate, memory_order_relaxed);
    if (rate != output->format.sample_rate) record_fault(output, FAULT_RATE);
    return 0;
}

static int on_buffer_size(jack_nframes_t frames, void* data) {
    JackOutput* output = data;
    atomic_store_explicit(&output->period, frames, memory_order_relaxed);
    if (!frames || frames > INT_MAX / 2) record_fault(output, FAULT_BUFFER);
    else if (frames > output->capacity) record_fault(output, FAULT_PERIOD);
    return 0;
}

static void on_port_connect(jack_port_id_t a, jack_port_id_t b, int connect, void* data) {
    JackOutput* output = data;
    (void)a; (void)b; (void)connect;
    atomic_fetch_add_explicit(&output->graph_version, 1, memory_order_release);
}

static void on_latency(jack_latency_callback_mode_t mode, void* data) {
    JackOutput* output = data;
    jack_nframes_t max = 0;
    if (mode != JackPlaybackLatency) return;
    for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
        jack_latency_range_t range;
        jack_port_get_latency_range(output->outputs[ch], mode, &range);
        if (range.max > max) max = range.max;
    }
    if (max > INT_MAX / 2) record_fault(output, FAULT_LATENCY);
    else atomic_store_explicit(&output->latency, max, memory_order_release);
}

static int on_xrun(void* data) {
    JackOutput* output = data;
    atomic_fetch_add_explicit(&output->xruns, 1, memory_order_relaxed);
    return 0; /* JACK resumes processing after xruns; no callback-side recovery I/O. */
}

static int all_connected(JackOutput* output) {
    unsigned int version = atomic_load_explicit(&output->graph_version, memory_order_acquire);
    unsigned int seen = atomic_load_explicit(&output->graph_seen, memory_order_acquire);
    unsigned int mask = atomic_load_explicit(&output->connected, memory_order_relaxed);
    return seen == version && mask == (1u << output->format.num_channels) - 1 &&
           version == atomic_load_explicit(&output->graph_version, memory_order_acquire);
}

static int process(jack_nframes_t frames, void* data) {
    JackOutput* output = data;
    float* ports[8] = {0};
    unsigned int state = atomic_load_explicit(&output->requested_state, memory_order_acquire);
    unsigned int count = 0, read = 0;
    atomic_fetch_add_explicit(&output->cycles, 1, memory_order_relaxed);
    for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
        ports[ch] = jack_port_get_buffer(output->outputs[ch], frames);
        if (!ports[ch]) record_fault(output, FAULT_BUFFER);
        else memset(ports[ch], 0, (size_t)frames * sizeof(float));
    }
    if (state == AUDIO_STATE_PLAYING && !atomic_load_explicit(&output->fault, memory_order_relaxed) &&
        all_connected(output)) {
        read = atomic_load_explicit(&output->read_index, memory_order_relaxed);
        unsigned int write = atomic_load_explicit(&output->write_index, memory_order_acquire);
        count = write - read;
        if (count > frames) count = frames;
        for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
            float* destination = ports[ch];
            if (!destination) continue; /* Defensive even when the fault flag is set. */
            for (unsigned int f = 0; f < count; ++f) {
                size_t index = (size_t)((read + f) & (output->capacity - 1)) * output->format.num_channels;
                destination[f] = output->queue[index + ch];
            }
        }
    }
    unsigned int tail = atomic_load_explicit(&output->tail_frames, memory_order_relaxed);
    if (count) {
        tail = frames + atomic_load_explicit(&output->latency, memory_order_acquire);
    } else {
        tail = tail > frames ? tail - frames : 0;
    }
    if (state == AUDIO_STATE_STOPPED) tail = 0;
    atomic_store_explicit(&output->tail_frames, tail, memory_order_release);
    /* Publish downstream latency before retiring the final queue frames, so
     * drain cannot observe an empty FIFO with an unpublished playback tail. */
    if (count) atomic_store_explicit(&output->read_index, read + count, memory_order_release);
    atomic_store_explicit(&output->actual_state, state, memory_order_release);
    return 0;
}

static unsigned int queued_frames(JackOutput* output) {
    unsigned int read = atomic_load_explicit(&output->read_index, memory_order_acquire);
    unsigned int write = atomic_load_explicit(&output->write_index, memory_order_acquire);
    return write - read;
}

/* Discovery/connection queries belong to the owner thread, never process().
 * The version handshake stops consumption while a graph snapshot is stale. */
static void refresh_connections(JackOutput* output) {
    unsigned int version = atomic_load_explicit(&output->graph_version, memory_order_acquire);
    if (version == atomic_load_explicit(&output->graph_seen, memory_order_acquire)) return;
    unsigned int mask = 0;
    for (unsigned int ch = 0; ch < output->format.num_channels; ++ch)
        if (jack_port_connected(output->outputs[ch]) > 0) mask |= 1u << ch;
    atomic_store_explicit(&output->connected, mask, memory_order_relaxed);
    atomic_store_explicit(&output->graph_seen, version, memory_order_release);
}

static int check_health(JackOutput* output) {
    if (output->failed) return -1;
    if (atomic_load_explicit(&output->shutdown, memory_order_acquire))
        return fail(output, "JACK server disconnected or shut down this client");
    switch (atomic_load_explicit(&output->fault, memory_order_acquire)) {
    case FAULT_SHUTDOWN: return fail(output, "JACK server disconnected or shut down this client");
    case FAULT_RATE: {
        char message[256];
        snprintf(message, sizeof(message), "JACK server sample rate changed to %u Hz; source is %u Hz; resampling is not performed",
                 atomic_load_explicit(&output->server_rate, memory_order_relaxed), output->format.sample_rate);
        return fail(output, message);
    }
    case FAULT_BUFFER: return fail(output, "JACK supplied an invalid audio buffer or period size");
    case FAULT_LATENCY: return fail(output, "JACK reported an unsupported playback latency");
    case FAULT_PERIOD: return fail(output, "JACK period exceeds the preallocated queue; reopen with a larger --buffer");
    default: break;
    }
    refresh_connections(output);
    uint64_t now = monotonic_ns();
    unsigned int cycles = atomic_load_explicit(&output->cycles, memory_order_relaxed);
    if (cycles != output->last_cycles) {
        output->last_cycles = cycles;
        output->last_progress_ns = now;
    }
    unsigned int state = atomic_load_explicit(&output->requested_state, memory_order_acquire);
    if (output->active && state == AUDIO_STATE_PLAYING) {
        uint64_t timeout = (uint64_t)output->timeout_ms * 1000000u;
        if (now - output->last_progress_ns > timeout)
            return fail(output, "JACK graph stopped processing audio");
        if (output->connection_mode == JACK_CONNECT_AUTO && !all_connected(output)) {
            if (!output->disconnected_ns) output->disconnected_ns = now;
            if (now - output->disconnected_ns > timeout)
                return fail(output, "JACK output ports disconnected; every source channel needs a destination");
        } else output->disconnected_ns = 0;
    }
    return 0;
}

static jack_client_t* open_client(const char* name, const char* server, char* error, size_t size) {
    jack_status_t status = 0;
    jack_options_t options = JackNoStartServer;
    jack_client_t* client;
    if (server) options |= JackServerName;
    client = server ? jack_client_open(name, options, &status, server)
                    : jack_client_open(name, options, &status);
    if (!client) snprintf(error, size, "Cannot open JACK client on server '%s' (status 0x%x); start a JACK server first",
                          server ? server : "default", (unsigned int)status);
    return client;
}

static int select_ports(JackOutput* output, const JackConfig* config) {
    const char** names = NULL;
    if (output->connection_mode == JACK_CONNECT_MANUAL) return 0;
    if (config && config->ports) {
        for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
            output->destinations[ch] = strdup(config->ports[ch]);
            if (!output->destinations[ch]) return fail(output, "Cannot allocate JACK destination names");
        }
        return 0;
    }
    const char* pattern = config ? config->port_pattern : NULL;
    unsigned long flags = JackPortIsInput | (pattern ? 0 : JackPortIsPhysical);
    names = jack_get_ports(output->client, pattern, JACK_DEFAULT_AUDIO_TYPE, flags);
    size_t count = 0;
    if (names) while (names[count]) ++count;
    if (count < output->format.num_channels) {
        if (names) jack_free(names);
        return fail(output, "Not enough JACK destination ports; select --device <regex>, explicit --jack-port mappings, or --jack-no-connect");
    }
    for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
        output->destinations[ch] = strdup(names[ch]);
        if (!output->destinations[ch]) { jack_free(names); return fail(output, "Cannot allocate JACK destination names"); }
    }
    jack_free(names);
    return 0;
}

JackOutput* jack_output_create(const AudioFormat* format, const JackConfig* config) {
    JackOutput* output = calloc(1, sizeof(*output));
    unsigned int buffer_ms = config && config->buffer_duration_ms ? config->buffer_duration_ms : 100;
    last_error[0] = '\0';
    if (!output) { snprintf(last_error, sizeof(last_error), "Cannot allocate JACK output"); return NULL; }
    atomic_init(&output->read_index, 0); atomic_init(&output->write_index, 0);
    atomic_init(&output->requested_state, AUDIO_STATE_STOPPED); atomic_init(&output->actual_state, AUDIO_STATE_STOPPED);
    atomic_init(&output->fault, FAULT_NONE); atomic_init(&output->shutdown, 0);
    atomic_init(&output->server_rate, 0); atomic_init(&output->period, 0);
    atomic_init(&output->latency, 0); atomic_init(&output->tail_frames, 0);
    atomic_init(&output->cycles, 0); atomic_init(&output->xruns, 0);
    atomic_init(&output->graph_version, 1); atomic_init(&output->graph_seen, 0); atomic_init(&output->connected, 0);
    output->connection_mode = config ? config->connection_mode : JACK_CONNECT_AUTO;
    output->timeout_ms = config && config->timeout_ms ? config->timeout_ms : 5000;
    if (!format || !format->num_channels || format->num_channels > 8 ||
        format->sample_rate < 8000 || format->sample_rate > 192000 ||
        !format->block_align || format->block_align % format->num_channels ||
        format->valid_bits_per_sample > format->bits_per_sample ||
        buffer_ms < 10 || buffer_ms > 5000 || output->timeout_ms > 60000 ||
        (output->connection_mode != JACK_CONNECT_AUTO && output->connection_mode != JACK_CONNECT_MANUAL)) {
        fail(output, "Invalid JACK format or configuration"); goto error;
    }
    output->format = *format;
    size_t bytes = format->block_align / format->num_channels;
    if ((format->is_float && (format->bits_per_sample != 32 || bytes != 4)) ||
        (!format->is_float && !((format->bits_per_sample == 8 && bytes == 1) ||
                               (format->bits_per_sample == 16 && bytes == 2) ||
                               (format->bits_per_sample == 24 && (bytes == 3 || bytes == 4)) ||
                               (format->bits_per_sample == 32 && bytes == 4)))) {
        fail(output, "Unsupported JACK input PCM representation"); goto error;
    }
    if (!atomic_is_lock_free(&output->read_index)) {
        fail(output, "JACK requires lock-free unsigned atomics on this platform"); goto error;
    }
    if (config && ((config->server_name && !config->server_name[0]) ||
        (config->client_name && !config->client_name[0]) ||
        (config->port_pattern && !config->port_pattern[0]) ||
        (config->ports && config->num_ports != format->num_channels) ||
        (!config->ports && config->num_ports) || (config->ports && config->port_pattern) ||
        (output->connection_mode == JACK_CONNECT_MANUAL && (config->ports || config->port_pattern)))) {
        fail(output, "Invalid JACK names, channel mapping, or manual-routing configuration"); goto error;
    }
    if (config && config->ports) {
        for (size_t ch = 0; ch < config->num_ports; ++ch) {
            if (!config->ports[ch] || !config->ports[ch][0]) { fail(output, "Empty JACK destination port"); goto error; }
            for (size_t i = 0; i < ch; ++i)
                if (!strcmp(config->ports[i], config->ports[ch])) { fail(output, "JACK source channels need distinct destination ports"); goto error; }
        }
    }
    output->client = open_client(config && config->client_name ? config->client_name : "sapfs",
                                 config ? config->server_name : NULL, output->error_msg, sizeof(output->error_msg));
    if (!output->client) goto error;
    jack_on_shutdown(output->client, on_shutdown, output);
    unsigned int rate = jack_get_sample_rate(output->client);
    if (rate != format->sample_rate) {
        snprintf(output->error_msg, sizeof(output->error_msg), "JACK server is %u Hz but source is %u Hz; resampling is not performed", rate, format->sample_rate);
        goto error;
    }
    atomic_store_explicit(&output->server_rate, rate, memory_order_relaxed);
    unsigned int period = jack_get_buffer_size(output->client);
    if (!period || period > INT_MAX / 2) { fail(output, "Unsupported initial JACK period size"); goto error; }
    atomic_store_explicit(&output->period, period, memory_order_relaxed);
    unsigned int frames = (format->sample_rate * buffer_ms + 999) / 1000;
    if (frames < period * 2) frames = period * 2;
    output->capacity = 1;
    while (output->capacity < frames) output->capacity <<= 1;
    if (output->capacity > INT_MAX) { fail(output, "Unsupported JACK application queue size"); goto error; }
    output->queue = calloc((size_t)output->capacity * format->num_channels, sizeof(float));
    if (!output->queue) { fail(output, "Cannot allocate JACK PCM queue"); goto error; }
    for (unsigned int ch = 0; ch < format->num_channels; ++ch) {
        char name[32];
        snprintf(name, sizeof(name), "out_%u", ch + 1);
        output->outputs[ch] = jack_port_register(output->client, name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!output->outputs[ch]) { fail(output, "Cannot register JACK output port"); goto error; }
    }
    if (jack_set_process_callback(output->client, process, output) ||
        jack_set_sample_rate_callback(output->client, on_sample_rate, output) ||
        jack_set_buffer_size_callback(output->client, on_buffer_size, output) ||
        jack_set_port_connect_callback(output->client, on_port_connect, output) ||
        jack_set_latency_callback(output->client, on_latency, output) ||
        jack_set_xrun_callback(output->client, on_xrun, output)) {
        fail(output, "Cannot register JACK client callbacks"); goto error;
    }
    if (select_ports(output, config) < 0) goto error;
    return output;
error:
    snprintf(last_error, sizeof(last_error), "%s", output->error_msg);
    jack_output_close(output);
    return NULL;
}

static int change_state(JackOutput* output, AudioState state) {
    output->last_progress_ns = monotonic_ns();
    output->last_cycles = atomic_load_explicit(&output->cycles, memory_order_relaxed);
    atomic_store_explicit(&output->requested_state, state, memory_order_release);
    uint64_t deadline = monotonic_ns() + (uint64_t)output->timeout_ms * 1000000u;
    do {
        if (check_health(output) < 0) return -1;
        if (atomic_load_explicit(&output->actual_state, memory_order_acquire) == (unsigned int)state &&
            (state != AUDIO_STATE_PLAYING || output->connection_mode == JACK_CONNECT_MANUAL || all_connected(output))) return 0;
        sleep_ms(1);
    } while (monotonic_ns() < deadline);
    return fail(output, "Timed out waiting for JACK playback state or destination connections");
}

int jack_output_start(JackOutput* output) {
    if (!output || check_health(output) < 0) return -1;
    if (atomic_load_explicit(&output->requested_state, memory_order_acquire) == AUDIO_STATE_PLAYING) return 0;
    if (!output->active) {
        if (jack_activate(output->client)) return fail(output, "Cannot activate JACK client");
        output->active = 1;
        if (output->connection_mode == JACK_CONNECT_AUTO) {
            for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
                jack_port_t* dest = jack_port_by_name(output->client, output->destinations[ch]);
                if (!dest || !(jack_port_flags(dest) & JackPortIsInput) || strcmp(jack_port_type(dest), JACK_DEFAULT_AUDIO_TYPE))
                    return fail(output, "JACK destination is missing or is not an audio input port");
            }
            for (unsigned int ch = 0; ch < output->format.num_channels; ++ch) {
                int err = jack_connect(output->client, jack_port_name(output->outputs[ch]), output->destinations[ch]);
                if (err && err != EEXIST) return fail(output, "Cannot connect JACK output to its destination");
            }
        }
        atomic_fetch_add_explicit(&output->graph_version, 1, memory_order_release);
    }
    return change_state(output, AUDIO_STATE_PLAYING);
}

int jack_output_stop(JackOutput* output) {
    if (!output || check_health(output) < 0) return -1;
    if (output->active && change_state(output, AUDIO_STATE_STOPPED) < 0) return -1;
    /* The callback has acknowledged STOPPED and no longer reads the queue. */
    atomic_store_explicit(&output->read_index, 0, memory_order_relaxed);
    atomic_store_explicit(&output->write_index, 0, memory_order_relaxed);
    atomic_store_explicit(&output->tail_frames, 0, memory_order_relaxed);
    output->disconnected_ns = 0;
    return 0;
}

int jack_output_pause(JackOutput* output) {
    if (!output || check_health(output) < 0 ||
        atomic_load_explicit(&output->requested_state, memory_order_acquire) != AUDIO_STATE_PLAYING) return -1;
    return change_state(output, AUDIO_STATE_PAUSED);
}

int jack_output_resume(JackOutput* output) {
    if (!output || check_health(output) < 0 ||
        atomic_load_explicit(&output->requested_state, memory_order_acquire) != AUDIO_STATE_PAUSED) return -1;
    return change_state(output, AUDIO_STATE_PLAYING);
}

static float read_sample(const uint8_t* source, const AudioFormat* format, size_t bytes) {
    uint32_t value = 0;
    for (size_t b = 0; b < bytes; ++b) value |= (uint32_t)source[b] << (8 * b);
    if (format->is_float) {
        float result;
        memcpy(&result, &value, sizeof(result));
        return result;
    }
    if (format->bits_per_sample == 8) return ((int)value - 128) / 128.0f;
    unsigned int bits = format->bits_per_sample;
    if (bits == 24) value &= 0xffffff;
    int64_t signed_value = value;
    if (value & (UINT32_C(1) << (bits - 1))) signed_value -= INT64_C(1) << bits;
    float scale = bits == 16 ? 1.0f / 32768.0f : bits == 24 ? 1.0f / 8388608.0f : 1.0f / 2147483648.0f;
    return (float)signed_value * scale;
}

int jack_output_write(JackOutput* output, const void* data, size_t num_frames) {
    if (!output || (!data && num_frames) || check_health(output) < 0 ||
        atomic_load_explicit(&output->requested_state, memory_order_acquire) != AUDIO_STATE_PLAYING) return -1;
    if (!num_frames) return 0;
    unsigned int write = atomic_load_explicit(&output->write_index, memory_order_relaxed);
    unsigned int read = atomic_load_explicit(&output->read_index, memory_order_acquire);
    size_t free_frames = output->capacity - (write - read);
    if (num_frames > free_frames) num_frames = free_frames;
    if (num_frames > 4096) num_frames = 4096;
    size_t bytes = output->format.block_align / output->format.num_channels;
    for (size_t f = 0; f < num_frames; ++f) {
        size_t index = (size_t)((write + (unsigned int)f) & (output->capacity - 1)) * output->format.num_channels;
        for (size_t ch = 0; ch < output->format.num_channels; ++ch)
            output->queue[index + ch] = read_sample((const uint8_t*)data + f * output->format.block_align + ch * bytes, &output->format, bytes);
    }
    atomic_store_explicit(&output->write_index, write + (unsigned int)num_frames, memory_order_release);
    return (int)num_frames;
}

AudioState jack_output_get_state(JackOutput* output) {
    if (!output || check_health(output) < 0) return AUDIO_STATE_STOPPED;
    return (AudioState)atomic_load_explicit(&output->actual_state, memory_order_acquire);
}

int jack_output_get_available_frames(JackOutput* output) {
    return !output || check_health(output) < 0 ? -1 : (int)(output->capacity - queued_frames(output));
}

int jack_output_get_queued_frames(JackOutput* output) {
    if (!output || check_health(output) < 0) return -1;
    unsigned int queued = queued_frames(output);
    uint64_t frames = (uint64_t)queued + atomic_load_explicit(&output->tail_frames, memory_order_acquire);
    return frames > INT_MAX ? INT_MAX : (int)frames;
}

int jack_output_drain(JackOutput* output) {
    int queued = jack_output_get_queued_frames(output);
    if (queued < 0 || atomic_load_explicit(&output->requested_state, memory_order_acquire) != AUDIO_STATE_PLAYING) return -1;
    if (output->connection_mode == JACK_CONNECT_AUTO && !all_connected(output)) return 0;
    return queued == 0;
}

const char* jack_output_get_error(JackOutput* output) {
    if (!output) return jack_output_get_last_error();
    check_health(output);
    return output->error_msg[0] ? output->error_msg : NULL;
}

const char* jack_output_get_last_error(void) {
    return last_error[0] ? last_error : "Failed to initialize JACK output";
}

const char* jack_output_get_client_name(JackOutput* output) {
    return output && check_health(output) == 0 ? jack_get_client_name(output->client) : NULL;
}

const char* jack_output_get_port_name(JackOutput* output, unsigned int channel) {
    return output && channel < output->format.num_channels && check_health(output) == 0
         ? jack_port_name(output->outputs[channel]) : NULL;
}

unsigned int jack_output_get_xrun_count(JackOutput* output) {
    return output ? atomic_load_explicit(&output->xruns, memory_order_relaxed) : 0;
}

void jack_output_close(JackOutput* output) {
    if (!output) return;
    atomic_store_explicit(&output->requested_state, AUDIO_STATE_STOPPED, memory_order_release);
    /* Closing joins client callbacks before the queue or callback data is freed. */
    if (output->client) jack_client_close(output->client);
    for (size_t ch = 0; ch < 8; ++ch) free(output->destinations[ch]);
    free(output->queue);
    free(output);
}

int jack_output_list_ports(const char* server_name, const char* pattern,
                          JackPortCallback callback, void* user_data) {
    last_error[0] = '\0';
    if (!callback || (server_name && !server_name[0]) || (pattern && !pattern[0])) {
        snprintf(last_error, sizeof(last_error), "Invalid JACK port list arguments"); return -1;
    }
    jack_client_t* client = open_client("sapfs-list", server_name, last_error, sizeof(last_error));
    if (!client) return -1;
    const char** names = jack_get_ports(client, pattern, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput);
    if (names) {
        for (size_t i = 0; names[i]; ++i) {
            jack_port_t* port = jack_port_by_name(client, names[i]);
            if (port) callback(names[i], (jack_port_flags(port) & JackPortIsPhysical) != 0, user_data);
        }
        jack_free(names);
    }
    jack_client_close(client);
    return 0;
}

#endif
