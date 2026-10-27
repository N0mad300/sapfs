#include "audio_output/audio_output.h"
#include "audio_output/linux_output.h"
#include "audio_output/pulseaudio_output.h"
#include <pulse/pulseaudio.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Verify actual client format and submitted bytes, independently of server
 * mixing/conversion. Forward every write to the real PulseAudio connection. */
static const unsigned char* expected;
static size_t expected_bytes, sent_bytes;
static pa_sample_format_t expected_format;
int __real_pa_stream_write(pa_stream*, const void*, size_t, pa_free_cb_t, int64_t, pa_seek_mode_t);
int __wrap_pa_stream_write(pa_stream* stream, const void* data, size_t bytes,
                          pa_free_cb_t free_cb, int64_t offset, pa_seek_mode_t seek) {
    if (expected) {
        assert(pa_stream_get_sample_spec(stream)->format == expected_format);
        assert(bytes <= expected_bytes - sent_bytes);
        assert(memcmp(data, expected + sent_bytes, bytes) == 0);
        sent_bytes += bytes;
    }
    return __real_pa_stream_write(stream, data, bytes, free_cb, offset, seek);
}
static int swallow_drain_ack;
pa_operation* __real_pa_stream_drain(pa_stream*, pa_stream_success_cb_t, void*);
pa_operation* __wrap_pa_stream_drain(pa_stream* stream, pa_stream_success_cb_t callback, void* data) {
    return __real_pa_stream_drain(stream, swallow_drain_ack ? NULL : callback, data);
}
static AudioFormat stereo16(void) {
    AudioFormat f = {48000, 2, 16, 16, 4, 3, 0};
    return f;
}
static void drain(AudioOutput* o) {
    for (int i = 0; i < 5000; ++i) {
        int result = audio_output_drain(o);
        if (result < 0) fprintf(stderr, "%s\n", audio_output_get_error(o));
        assert(result >= 0);
        if (result) { assert(audio_output_get_queued_frames(o) == 0); return; }
        usleep(1000);
    }
    assert(!"PulseAudio drain timed out");
}
static void write_frames(AudioOutput* o, const void* samples, size_t frames, size_t align) {
    for (int i = 0; frames && i < 10000; ++i) {
        int written = audio_output_write(o, samples, frames);
        if (written < 0) fprintf(stderr, "%s\n", audio_output_get_error(o));
        assert(written >= 0 && (size_t)written <= frames);
        samples = (const char*)samples + (size_t)written * align;
        frames -= (size_t)written;
        if (!written) usleep(1000);
    }
    assert(!frames);
}
static void count_device(const char* name, const char* description, unsigned int index, void* data) {
    (void)index;
    assert(name && *name && description);
    ++*(int*)data;
}
int main(int argc, char** argv) {
    AudioFormat f = stereo16();
    PulseaudioConfig config = {.device = "test-sink", .buffer_duration_ms = 100, .timeout_ms = 2000};
    if (argc > 1 && strcmp(argv[1], "connect") == 0) {
        assert(argc == 3);
        config.server = argv[2]; config.timeout_ms = 150;
        assert(!pulseaudio_output_create(&f, &config));
        assert(strstr(pulseaudio_output_get_last_error(), "Timed out"));
        puts("PulseAudio connection timeout check passed");
        return 0;
    }
    if (argc > 2) config.device = argv[2];
    LinuxAudioConfig linux_config = {LINUX_AUDIO_BACKEND_PULSEAUDIO, &config};
    AudioOutput* o = audio_output_init(&f, &linux_config);
    if (!o) fprintf(stderr, "%s\n", audio_output_get_error(NULL));
    assert(o);
    if (argc > 1) {
        assert(audio_output_start(o) == 0);
        int16_t audio[4800 * 2] = {0};
        write_frames(o, audio, 2400, 4);
        puts("READY"); fflush(stdout);
        assert(getchar() != EOF);
        int result;
        swallow_drain_ack = strcmp(argv[1], "drain-ack") == 0;
        if (strcmp(argv[1], "control") == 0) result = audio_output_pause(o);
        else {
            result = 0;
            for (int i = 0; i < 5000 && result >= 0; ++i) {
                result = audio_output_drain(o);
                /* For a removed pinned sink the first call may observe the
                 * error; for a frozen/suspended server the watchdog detects it. */
                if (result == 1) { fprintf(stderr, "Unexpected drain completion\n"); break; }
                usleep(1000);
            }
        }
        assert(result == -1);
        assert(*audio_output_get_error(o));
        audio_output_close(o);
        puts("PulseAudio bounded failure check passed");
        return 0;
    }
    assert(audio_output_get_state(o) == AUDIO_STATE_STOPPED);
    assert(audio_output_get_available_frames(o) > 0);
    assert(audio_output_pause(o) == -1);
    assert(audio_output_write(o, NULL, 0) == -1);
    assert(audio_output_start(o) == 0 && audio_output_start(o) == 0);
    assert(audio_output_write(o, NULL, 0) == 0);
    assert(audio_output_write(o, NULL, 1) == -1);
    assert(audio_output_drain(o) == 1); /* Empty EOF. */
    int16_t samples[48000 * 2];
    for (size_t i = 0; i < 48000 * 2; ++i) samples[i] = (int16_t)(i * 71);
    int first = audio_output_write(o, samples, 48000);
    assert(first > 0 && first <= 4096);
    assert(audio_output_pause(o) == 0);
    assert(audio_output_get_state(o) == AUDIO_STATE_PAUSED);
    assert(audio_output_write(o, samples, 1) == -1);
    int available = audio_output_get_available_frames(o);
    usleep(100000); /* Let asynchronous writable-size/timing reports settle. */
    available = audio_output_get_available_frames(o);
    usleep(50000);
    assert(audio_output_get_available_frames(o) == available);
    assert(audio_output_resume(o) == 0 && audio_output_resume(o) == -1);
    write_frames(o, samples + first * 2, 48000 - (size_t)first, 4);
    drain(o);
    /* Reuse after EOF, including repeated one-frame segments. */
    for (int i = 0; i < 3; ++i) { write_frames(o, samples, 1, 4); drain(o); }
    /* Draining and pause must preserve the segment; stop cancels drain. */
    write_frames(o, samples, 2400, 4);
    assert(audio_output_drain(o) >= 0);
    assert(audio_output_pause(o) == 0);
    assert(audio_output_resume(o) == 0);
    drain(o);
    write_frames(o, samples, 2400, 4);
    assert(audio_output_drain(o) >= 0);
    assert(audio_output_stop(o) == 0 && audio_output_stop(o) == 0);
    assert(audio_output_get_queued_frames(o) == 0);
    assert(audio_output_start(o) == 0);
    write_frames(o, samples, 1, 4);
    drain(o);
    AudioOutput* second = audio_output_init(&f, &linux_config);
    assert(second && audio_output_start(second) == 0);
    audio_output_close(o);
    write_frames(second, samples, 2400, 4);
    drain(second);
    audio_output_close(second);

    /* Native PCM must reach libpulse untouched, including full S32 low bits,
     * packed/padded 24-bit and left-aligned WAVE extensible containers. */
    static const struct { int bits, bytes, valid, is_float; pa_sample_format_t pulse; } formats[] = {
        {8, 1, 8, 0, PA_SAMPLE_U8}, {16, 2, 16, 0, PA_SAMPLE_S16LE},
        {24, 3, 24, 0, PA_SAMPLE_S24LE}, {24, 4, 24, 0, PA_SAMPLE_S24_32LE},
        {32, 4, 24, 0, PA_SAMPLE_S32LE}, {32, 4, 32, 0, PA_SAMPLE_S32LE},
        {32, 4, 32, 1, PA_SAMPLE_FLOAT32LE}
    };
    unsigned char data[24000];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 79 + 3);
    for (size_t i = 0; i < sizeof(formats) / sizeof(*formats); ++i) {
        f = stereo16();
        f.bits_per_sample = formats[i].bits; f.valid_bits_per_sample = formats[i].valid;
        f.block_align = formats[i].bytes * 2; f.is_float = formats[i].is_float;
        if (f.is_float) {
            for (size_t n = 0; n < sizeof(data) / 4; ++n) {
                float value = (float)((int)(n % 1024) - 512) / 512;
                memcpy(data + n * 4, &value, 4);
            }
        }
        o = audio_output_init(&f, &linux_config);
        assert(o && audio_output_start(o) == 0);
        expected = data; expected_format = formats[i].pulse;
        sent_bytes = 0; expected_bytes = sizeof(data) / f.block_align * f.block_align;
        write_frames(o, data, expected_bytes / f.block_align, f.block_align);
        assert(sent_bytes == expected_bytes);
        drain(o); expected = NULL;
        audio_output_close(o);
    }
    f = stereo16();
    config.device = "missing-sapfs-test-sink";
    assert(!audio_output_init(&f, &linux_config));
    assert(strstr(audio_output_get_error(NULL), "sink"));
    config.device = "test-sink";
    config.buffer_duration_ms = 9;
    assert(!audio_output_init(&f, &linux_config));
    config.buffer_duration_ms = 10;
    config.timeout_ms = 60001;
    assert(!audio_output_init(&f, &linux_config));
    config.timeout_ms = 2000;
    f.channel_mask = 4;
    assert(!audio_output_init(&f, &linux_config));
    f.channel_mask = 1u << 31;
    assert(!audio_output_init(&f, &linux_config));
    f = stereo16(); f.block_align = 7;
    assert(!audio_output_init(&f, &linux_config));
    f = stereo16(); f.valid_bits_per_sample = 32;
    assert(!audio_output_init(&f, &linux_config));
    f = stereo16(); f.is_float = 1;
    assert(!audio_output_init(&f, &linux_config));
    assert(!pulseaudio_output_create(NULL, &config));
    int count = 0;
    assert(pulseaudio_output_list_devices(NULL, count_device, &count) == 0 && count >= 2);
    assert(pulseaudio_output_list_devices(NULL, NULL, NULL) == -1);
    pulseaudio_output_close(NULL);
    puts("PulseAudio API, native PCM delivery, backpressure, pause/restart, drain and validation tests passed");
    return 0;
}
