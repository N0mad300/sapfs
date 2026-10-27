#include "audio_output/jack_output.h"
#include "audio_output/linux_output.h"
#include <jack/jack.h>
#include <assert.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static JackSampleRateCallback rate_callback;
static JackBufferSizeCallback size_callback;
static JackXRunCallback xrun_callback;
static void* rate_data;
static void* size_data;
static void* xrun_data;

int __real_jack_set_sample_rate_callback(jack_client_t*, JackSampleRateCallback, void*);
int __wrap_jack_set_sample_rate_callback(jack_client_t* client, JackSampleRateCallback fn, void* data) {
    rate_callback = fn; rate_data = data;
    return __real_jack_set_sample_rate_callback(client, fn, data);
}
int __real_jack_set_buffer_size_callback(jack_client_t*, JackBufferSizeCallback, void*);
int __wrap_jack_set_buffer_size_callback(jack_client_t* client, JackBufferSizeCallback fn, void* data) {
    size_callback = fn; size_data = data;
    return __real_jack_set_buffer_size_callback(client, fn, data);
}
int __real_jack_set_xrun_callback(jack_client_t*, JackXRunCallback, void*);
int __wrap_jack_set_xrun_callback(jack_client_t* client, JackXRunCallback fn, void* data) {
    xrun_callback = fn; xrun_data = data;
    return __real_jack_set_xrun_callback(client, fn, data);
}

typedef struct {
    jack_client_t* client;
    jack_port_t* ports[8];
    float* samples;
    unsigned int channels;
    unsigned int capacity;
    atomic_uint written;
} Capture;

static volatile sig_atomic_t running = 1;
static void interrupted(int signal) { (void)signal; running = 0; }

static int capture_process(jack_nframes_t frames, void* data) {
    Capture* capture = data;
    unsigned int start = atomic_load_explicit(&capture->written, memory_order_relaxed);
    float* inputs[8];
    for (unsigned int ch = 0; ch < capture->channels; ++ch)
        inputs[ch] = jack_port_get_buffer(capture->ports[ch], frames);
    unsigned int count = frames;
    if (count > capture->capacity - start) count = capture->capacity - start;
    for (unsigned int f = 0; f < count; ++f)
        for (unsigned int ch = 0; ch < capture->channels; ++ch)
            capture->samples[(size_t)(start + f) * capture->channels + ch] = inputs[ch][f];
    atomic_store_explicit(&capture->written, start + count, memory_order_release);
    return 0;
}

static void open_capture(Capture* capture, const char* name, unsigned int channels) {
    memset(capture, 0, sizeof(*capture));
    capture->channels = channels;
    capture->capacity = 48000 * 30;
    capture->samples = calloc((size_t)capture->capacity * channels, sizeof(float));
    assert(capture->samples);
    atomic_init(&capture->written, 0);
    jack_status_t status;
    capture->client = jack_client_open(name, JackNoStartServer, &status);
    assert(capture->client);
    for (unsigned int ch = 0; ch < channels; ++ch) {
        char port[32];
        snprintf(port, sizeof(port), "in_%u", ch + 1);
        capture->ports[ch] = jack_port_register(capture->client, port, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        assert(capture->ports[ch]);
    }
    assert(jack_set_process_callback(capture->client, capture_process, capture) == 0);
    assert(jack_activate(capture->client) == 0);
}

static int contains(Capture* capture, const float* expected, unsigned int frames) {
    unsigned int available = atomic_load_explicit(&capture->written, memory_order_acquire);
    for (unsigned int i = 0; i + frames <= available; ++i)
        if (!memcmp(capture->samples + (size_t)i * capture->channels, expected,
                    (size_t)frames * capture->channels * sizeof(float))) return 1;
    return 0;
}

static void write_all(JackOutput* output, const void* data, size_t frames, size_t align) {
    size_t sent = 0;
    for (int i = 0; sent < frames && i < 3000; ++i) {
        int written = jack_output_write(output, (const unsigned char*)data + sent * align, frames - sent);
        if (written < 0) fprintf(stderr, "%s\n", jack_output_get_error(output));
        assert(written >= 0);
        sent += (size_t)written;
        if (!written) usleep(1000);
    }
    assert(sent == frames);
}

static void drain(JackOutput* output) {
    for (int i = 0; i < 3000; ++i) {
        int done = jack_output_drain(output);
        assert(done >= 0);
        if (done) return;
        usleep(1000);
    }
    assert(!"JACK drain timed out");
}

static void api_tests(void) {
    Capture capture;
    open_capture(&capture, "api-capture", 2);
    const char* ports[] = {jack_port_name(capture.ports[0]), jack_port_name(capture.ports[1])};
    JackConfig config = {0};
    config.ports = ports; config.num_ports = 2; config.buffer_duration_ms = 10; config.timeout_ms = 500;
    AudioFormat format = {.sample_rate = 48000, .num_channels = 2, .bits_per_sample = 16,
                          .valid_bits_per_sample = 16, .block_align = 4};
    JackOutput* output = jack_output_create(&format, &config);
    if (!output) fprintf(stderr, "%s\n", jack_output_get_last_error());
    assert(output);
    assert(jack_output_get_state(output) == AUDIO_STATE_STOPPED);
    assert(jack_output_get_available_frames(output) == 512);
    assert(jack_output_pause(output) == -1);
    assert(jack_output_write(output, NULL, 0) == -1);
    assert(jack_output_start(output) == 0);
    assert(jack_output_start(output) == 0);
    assert(jack_output_write(output, NULL, 0) == 0);
    assert(jack_output_write(output, NULL, 1) == -1);
    int16_t pcm[4800 * 2];
    float expected[4800 * 2];
    for (size_t i = 0; i < 4800 * 2; ++i) { pcm[i] = (int16_t)(i * 37 - 8192); expected[i] = pcm[i] / 32768.0f; }
    int first = jack_output_write(output, pcm, 4800);
    assert(first > 0 && first <= 512);
    assert(jack_output_pause(output) == 0);
    unsigned int available = (unsigned int)jack_output_get_available_frames(output);
    usleep(30000);
    assert((unsigned int)jack_output_get_available_frames(output) == available);
    assert(jack_port_connected_to(capture.ports[0], jack_output_get_port_name(output, 0)));
    assert(jack_output_resume(output) == 0);
    assert(jack_output_resume(output) == -1);
    write_all(output, pcm + first * 2, 4800 - (size_t)first, 4);
    drain(output);
    /* Pause intentionally inserts a gap, so use a fresh uninterrupted segment. */
    write_all(output, pcm, 4800, 4);
    drain(output);
    usleep(10000);
    assert(contains(&capture, expected, 4800));
    assert(jack_set_buffer_size(capture.client, 256) == 0);
    write_all(output, pcm, 4800, 4);
    drain(output);
    assert(jack_set_buffer_size(capture.client, 128) == 0);
    unsigned int xruns = jack_output_get_xrun_count(output);
    assert(xrun_callback(xrun_data) == 0);
    assert(jack_output_get_xrun_count(output) >= xruns + 1);
    assert(jack_output_stop(output) == 0);
    assert(jack_output_stop(output) == 0);
    assert(jack_output_get_available_frames(output) == 512);
    assert(jack_port_connected_to(capture.ports[0], jack_output_get_port_name(output, 0)));
    assert(jack_output_start(output) == 0);
    write_all(output, pcm, 1, 4);
    drain(output);
    /* The JACK library must keep another client's callbacks alive. */
    JackOutput* second = jack_output_create(&format, &config);
    assert(second && jack_output_start(second) == 0);
    jack_output_close(output);
    write_all(second, pcm, 4800, 4); drain(second);
    assert(rate_callback(44100, rate_data) == 0);
    assert(jack_output_write(second, pcm, 1) == -1);
    assert(strstr(jack_output_get_error(second), "sample rate changed"));
    jack_output_close(second);

    config.ports = NULL; config.num_ports = 0; config.connection_mode = JACK_CONNECT_MANUAL;
    output = jack_output_create(&format, &config);
    assert(output && jack_output_start(output) == 0);
    assert(jack_output_write(output, pcm, 4800) == 512);
    usleep(20000);
    assert(jack_output_get_available_frames(output) == 0);
    assert(jack_connect(capture.client, jack_output_get_port_name(output, 0), ports[0]) == 0);
    usleep(20000);
    assert(jack_output_get_available_frames(output) == 0); /* No partial channel loss. */
    assert(jack_connect(capture.client, jack_output_get_port_name(output, 1), ports[1]) == 0);
    write_all(output, pcm + 512 * 2, 4800 - 512, 4); drain(output);
    jack_output_close(output);

    config.connection_mode = JACK_CONNECT_AUTO; config.ports = ports; config.num_ports = 2;
    output = jack_output_create(&format, &config);
    assert(output && jack_output_start(output) == 0);
    assert(jack_disconnect(capture.client, jack_output_get_port_name(output, 0), ports[0]) == 0);
    for (int i = 0; i < 1000 && jack_output_get_available_frames(output) >= 0; ++i) usleep(1000);
    assert(jack_output_get_available_frames(output) == -1);
    assert(strstr(jack_output_get_error(output), "disconnected"));
    jack_output_close(output);

    output = jack_output_create(&format, &config);
    assert(output);
    assert(size_callback(0, size_data) == 0);
    assert(jack_output_start(output) == -1);
    assert(strstr(jack_output_get_error(output), "period size"));
    jack_output_close(output);
    assert(jack_set_buffer_size(capture.client, 1024) == 0);
    output = jack_output_create(&format, &config);
    assert(output && jack_output_get_available_frames(output) >= 2048);
    assert(jack_output_start(output) == 0);
    write_all(output, pcm, 4800, 4); drain(output);
    jack_output_close(output);
    assert(jack_set_buffer_size(capture.client, 128) == 0);
    output = jack_output_create(&format, &config);
    assert(output);
    assert(size_callback(1024, size_data) == 0);
    assert(jack_output_start(output) == -1);
    assert(strstr(jack_output_get_error(output), "preallocated queue"));
    jack_output_close(output);
    format.sample_rate = 44100;
    assert(!jack_output_create(&format, &config));
    assert(strstr(jack_output_get_last_error(), "48000 Hz"));
    format.sample_rate = 48000;
    config.num_ports = 1; assert(!jack_output_create(&format, &config));
    config.num_ports = 2; config.port_pattern = ".*"; assert(!jack_output_create(&format, &config));
    config.port_pattern = NULL; config.buffer_duration_ms = 9; assert(!jack_output_create(&format, &config));
    config.buffer_duration_ms = 10;
    format.block_align = 7; assert(!jack_output_create(&format, &config));
    jack_output_close(NULL);
    jack_client_close(capture.client);
    free(capture.samples);
    puts("JACK API, SPSC buffering, pause/restart, drain, routing and failure tests passed");
}

int main(int argc, char** argv) {
    if (argc == 1) { api_tests(); return 0; }
    assert(argc == 3);
    Capture capture;
    unsigned int channels = (unsigned int)strtoul(argv[2], NULL, 10);
    assert(channels > 0 && channels <= 8);
    open_capture(&capture, "capture", channels);
    signal(SIGINT, interrupted); signal(SIGTERM, interrupted);
    puts("READY"); fflush(stdout);
    while (running) usleep(10000);
    jack_client_close(capture.client);
    FILE* file = fopen(argv[1], "wb"); assert(file);
    unsigned int written = atomic_load_explicit(&capture.written, memory_order_acquire);
    assert(fwrite(capture.samples, capture.channels * sizeof(float), written, file) == written);
    fclose(file); free(capture.samples);
    return 0;
}
