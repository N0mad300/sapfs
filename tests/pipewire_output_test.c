#include "audio_output/audio_output.h"
#include "audio_output/linux_output.h"
#include "audio_output/pipewire_output.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static AudioFormat stereo16(void) {
    AudioFormat format = {0};
    format.sample_rate = 48000;
    format.num_channels = 2;
    format.bits_per_sample = format.valid_bits_per_sample = 16;
    format.block_align = 4;
    return format;
}

static void write_frames(AudioOutput* output, const int16_t* samples, size_t frames) {
    size_t sent = 0;
    for (int i = 0; sent < frames && i < 2000; ++i) {
        int written = audio_output_write(output, samples + sent * 2, frames - sent);
        assert(written >= 0);
        sent += (size_t)written;
        if (!written) usleep(1000);
    }
    assert(sent == frames);
}

static void drain(AudioOutput* output) {
    for (int i = 0; i < 2000; ++i) {
        int done = audio_output_drain(output);
        assert(done >= 0);
        if (done) {
            assert(audio_output_get_queued_frames(output) == 0);
            return;
        }
        usleep(1000);
    }
    assert(!"PipeWire drain timed out");
}

int main(void) {
    AudioFormat format = stereo16();
    PipewireConfig config = {0};
    config.target = "test-sink";
    config.buffer_duration_ms = 10;
    config.connect_timeout_ms = 1500;
    LinuxAudioConfig linux_config = {LINUX_AUDIO_BACKEND_PIPEWIRE, &config};
    AudioOutput* output = audio_output_init(&format, &linux_config);
    if (!output) fprintf(stderr, "%s\n", audio_output_get_error(NULL));
    assert(output);
    assert(audio_output_get_state(output) == AUDIO_STATE_STOPPED);
    assert(audio_output_get_available_frames(output) == 480);
    assert(audio_output_pause(output) == -1);
    assert(audio_output_write(output, NULL, 0) == -1);
    assert(audio_output_start(output) == 0);
    assert(audio_output_start(output) == 0);
    assert(audio_output_write(output, NULL, 0) == 0);
    assert(audio_output_write(output, NULL, 1) == -1);
    int16_t samples[4800 * 2];
    for (size_t i = 0; i < 4800 * 2; ++i) samples[i] = (int16_t)(i % 8192);
    int first = audio_output_write(output, samples, 4800);
    assert(first > 0 && first <= 480); /* Partial, frame-aligned backpressure. */
    assert(audio_output_pause(output) == 0);
    assert(audio_output_get_state(output) == AUDIO_STATE_PAUSED);
    assert(audio_output_write(output, samples, 1) == -1);
    int available = audio_output_get_available_frames(output);
    usleep(30000);
    assert(audio_output_get_available_frames(output) == available);
    assert(audio_output_resume(output) == 0);
    assert(audio_output_resume(output) == -1);
    write_frames(output, samples + first * 2, 4800 - (size_t)first);
    drain(output);
    /* Reuse the stream after a drain, as the player does when looping. */
    write_frames(output, samples, 4800);
    drain(output);
    assert(audio_output_stop(output) == 0);
    assert(audio_output_stop(output) == 0);
    assert(audio_output_get_available_frames(output) == 480);
    assert(audio_output_start(output) == 0);
    write_frames(output, samples, 1); /* A one-frame file must drain. */
    drain(output);
    /* Initialization/teardown must not disturb another live PipeWire output. */
    AudioOutput* second = audio_output_init(&format, &linux_config);
    assert(second && audio_output_start(second) == 0);
    audio_output_close(output);
    write_frames(second, samples, 4800);
    drain(second);
    audio_output_close(second);

    config.target = "missing-sapfs-test-sink";
    config.connect_timeout_ms = 150;
    output = audio_output_init(&format, &linux_config);
    if (output) assert(audio_output_start(output) == -1);
    /* The session manager may reject immediately instead of timing out. */
    assert(audio_output_get_error(output) && audio_output_get_error(output)[0]);
    audio_output_close(output);
    config.buffer_duration_ms = 9;
    assert(!audio_output_init(&format, &linux_config));
    config.buffer_duration_ms = 10;
    format.channel_mask = 0x4; /* Two channels but only one speaker. */
    assert(!audio_output_init(&format, &linux_config));
    format.channel_mask = 0;
    format.block_align = 7;
    assert(!audio_output_init(&format, &linux_config));
    format = stereo16();
    format.bits_per_sample = format.valid_bits_per_sample = 32;
    format.block_align = 8;
    config.mode = PIPEWIRE_MODE_EXCLUSIVE;
    assert(!audio_output_init(&format, &linux_config));
    assert(strstr(audio_output_get_error(NULL), "32-bit integer precision"));
    puts("PipeWire API state, backpressure, pause, restart, drain and validation tests passed");
    return 0;
}
