#include "audio_output/alsa_output.h"
#include "audio_output/linux_output.h"
#include <alsa/asoundlib.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Keep real ALSA I/O; restrict capabilities and inject transient driver errors. */
static snd_pcm_format_t only_format = SND_PCM_FORMAT_UNKNOWN;
static int write_error;
static int suspend_pending;
static int delay_error;
static int reject_rate;
static int significant_bits;
static int reject_pause;

int __real_snd_pcm_hw_params_test_format(snd_pcm_t*, snd_pcm_hw_params_t*, snd_pcm_format_t);
int __wrap_snd_pcm_hw_params_test_format(snd_pcm_t* pcm, snd_pcm_hw_params_t* hw, snd_pcm_format_t format) {
    if (only_format != SND_PCM_FORMAT_UNKNOWN && format != only_format) return -EINVAL;
    return __real_snd_pcm_hw_params_test_format(pcm, hw, format);
}

int __real_snd_pcm_hw_params_set_rate(snd_pcm_t*, snd_pcm_hw_params_t*, unsigned int, int);
int __wrap_snd_pcm_hw_params_set_rate(snd_pcm_t* pcm, snd_pcm_hw_params_t* hw, unsigned int rate, int dir) {
    assert(rate == 48000 && dir == 0);
    return reject_rate ? -EINVAL : __real_snd_pcm_hw_params_set_rate(pcm, hw, rate, dir);
}

int __real_snd_pcm_hw_params_get_sbits(const snd_pcm_hw_params_t*);
int __wrap_snd_pcm_hw_params_get_sbits(const snd_pcm_hw_params_t* hw) {
    return significant_bits ? significant_bits : __real_snd_pcm_hw_params_get_sbits(hw);
}

int __real_snd_pcm_pause(snd_pcm_t*, int);
int __wrap_snd_pcm_pause(snd_pcm_t* pcm, int enable) {
    return reject_pause ? -ENOSYS : __real_snd_pcm_pause(pcm, enable);
}

snd_pcm_sframes_t __real_snd_pcm_writei(snd_pcm_t*, const void*, snd_pcm_uframes_t);
snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t* pcm, const void* data, snd_pcm_uframes_t frames) {
    if (write_error) {
        int err = write_error;
        write_error = 0;
        return err;
    }
    /* Force partial writes through every packing path. */
    return __real_snd_pcm_writei(pcm, data, frames > 3 ? 3 : frames);
}

int __real_snd_pcm_resume(snd_pcm_t*);
int __wrap_snd_pcm_resume(snd_pcm_t* pcm) {
    if (suspend_pending) {
        suspend_pending = 0;
        return -EAGAIN;
    }
    return __real_snd_pcm_resume(pcm);
}

int __real_snd_pcm_delay(snd_pcm_t*, snd_pcm_sframes_t*);
int __wrap_snd_pcm_delay(snd_pcm_t* pcm, snd_pcm_sframes_t* delay) {
    if (delay_error) {
        int err = delay_error;
        delay_error = 0;
        return err;
    }
    return __real_snd_pcm_delay(pcm, delay);
}

static AudioFormat make_format(unsigned bits, unsigned bytes, int is_float) {
    AudioFormat format = {0};
    format.sample_rate = 48000;
    format.num_channels = 2;
    format.bits_per_sample = (uint16_t)bits;
    format.valid_bits_per_sample = (uint16_t)bits;
    format.block_align = (uint16_t)(2 * bytes);
    format.is_float = is_float;
    return format;
}

static void check_capture(const char* path, const void* expected, size_t bytes) {
    unsigned char captured[256];
    FILE* file = fopen(path, "rb");
    assert(file && bytes <= sizeof(captured));
    assert(fread(captured, 1, sizeof(captured), file) == bytes);
    assert(memcmp(captured, expected, bytes) == 0);
    fclose(file);
}

static void check_samples(const char* path, AudioFormat format, const void* input,
                          size_t frames, const void* expected, size_t expected_bytes,
                          snd_pcm_format_t forced_format) {
    AlsaConfig config = {ALSA_MODE_SHARED, "capture", 100};
    LinuxAudioConfig linux_config = {LINUX_AUDIO_BACKEND_ALSA, &config};
    only_format = forced_format;
    AudioOutput* output = audio_output_init(&format, &linux_config);
    if (!output) fprintf(stderr, "%s\n", audio_output_get_error(NULL));
    assert(output);
    assert(audio_output_get_state(output) == AUDIO_STATE_STOPPED);
    assert(audio_output_get_queued_frames(output) == 0);
    assert(audio_output_write(output, input, frames) == -1);
    assert(audio_output_start(output) == 0);
    assert(audio_output_start(output) == 0);
    assert(audio_output_pause(output) == 0); /* Prepared, with no audio yet. */
    assert(audio_output_get_state(output) == AUDIO_STATE_PAUSED);
    assert(audio_output_resume(output) == 0);
    assert(audio_output_resume(output) == -1);
    assert(audio_output_write(output, NULL, 0) == 0);

    const int errors[] = {-EAGAIN, -EINTR, -EPIPE, -ESTRPIPE};
    for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
        write_error = errors[i];
        assert(audio_output_write(output, input, frames) == 0);
    }
    delay_error = -ESTRPIPE;
    suspend_pending = 1;
    assert(audio_output_get_queued_frames(output) > 0);

    size_t done = 0;
    while (done < frames) {
        int written = audio_output_write(output, (const uint8_t*)input + done * format.block_align, frames - done);
        assert(written > 0 && written <= 3);
        done += (size_t)written;
    }
    assert(audio_output_get_available_frames(output) >= 0);
    assert(audio_output_get_queued_frames(output) == 0);
    reject_pause = 1;
    assert(audio_output_pause(output) == -1);
    assert(audio_output_get_state(output) == AUDIO_STATE_PLAYING);
    reject_pause = 0;
    assert(audio_output_pause(output) == 0);
    assert(audio_output_get_state(output) == AUDIO_STATE_PAUSED);
    assert(audio_output_resume(output) == 0);
    write_error = -ENODEV;
    assert(audio_output_write(output, input, 1) == -1);
    assert(strstr(audio_output_get_error(output), "ALSA playback failed"));
    assert(audio_output_stop(output) == 0);
    assert(audio_output_start(output) == 0);
    assert(audio_output_stop(output) == 0);
    audio_output_close(output);
    check_capture(path, expected, expected_bytes);
}

int main(int argc, char** argv) {
    assert(argc == 2);
    const char* path = argv[1];
    const uint8_t pcm8[] = {0, 128, 255, 1, 127, 129, 2, 254};
    const uint8_t pcm16[] = {0, 0x80, 0, 0, 0xff, 0x7f, 1, 0, 0xff, 0xff, 0, 1, 0, 0xff, 0xff, 0x80};
    const uint8_t pcm24[] = {0, 0, 0x80, 0, 0, 0, 0xff, 0xff, 0x7f, 1, 0, 0,
                            0xff, 0xff, 0xff, 0, 0, 1, 0, 0, 0xff, 0xff, 0, 0x80};
    const uint8_t pcm32[] = {0, 0, 0, 0x80, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0x7f, 1, 0, 0, 0,
                            0xff, 0xff, 0xff, 0xff, 0, 0, 0, 1, 0, 0, 0, 0xff, 0xff, 0, 0, 0x80};
    const float floats[] = {-1.0f, 0, 0.5f, 0.25f, -0.75f, 1.0f, -0.0f, 0.125f};
    const uint8_t* inputs[] = {pcm8, pcm16, pcm24, pcm32};
    const unsigned widths[] = {1, 2, 3, 4};
    for (size_t w = 0; w < 4; ++w) {
        uint8_t widened[32] = {0};
        for (size_t i = 0; i < 8; ++i) {
            memcpy(widened + i * 4 + 4 - widths[w], inputs[w] + i * widths[w], widths[w]);
            if (w == 0) widened[i * 4 + 3] ^= 0x80;
        }
        AudioFormat format = make_format(widths[w] * 8, widths[w], 0);
        check_samples(path, format, inputs[w], 4, inputs[w], 8 * widths[w], SND_PCM_FORMAT_UNKNOWN);
        check_samples(path, format, inputs[w], 4, widened, sizeof(widened), SND_PCM_FORMAT_S32_LE);
    }
    uint8_t padded[32];
    for (size_t i = 0; i < 8; ++i) {
        memcpy(padded + i * 4, pcm24 + i * 3, 3);
        padded[i * 4 + 3] = (pcm24[i * 3 + 2] & 0x80) ? 0xff : 0;
    }
    check_samples(path, make_format(24, 3, 0), pcm24, 4, padded, sizeof(padded), SND_PCM_FORMAT_S24_LE);
    check_samples(path, make_format(24, 4, 0), padded, 4, padded, sizeof(padded), SND_PCM_FORMAT_UNKNOWN);
    check_samples(path, make_format(24, 4, 0), padded, 4, pcm24, sizeof(pcm24), SND_PCM_FORMAT_S24_3LE);
    uint8_t widened8[16] = {0};
    for (size_t i = 0; i < 8; ++i) widened8[i * 2 + 1] = pcm8[i] ^ 0x80;
    check_samples(path, make_format(8, 1, 0), pcm8, 4, widened8, sizeof(widened8), SND_PCM_FORMAT_S16_LE);
    check_samples(path, make_format(32, 4, 1), floats, 4, floats, sizeof(floats), SND_PCM_FORMAT_UNKNOWN);

    AudioFormat format = make_format(32, 4, 1);
    AlsaConfig config = {ALSA_MODE_SHARED, "null", 100};
    only_format = SND_PCM_FORMAT_S32_LE;
    assert(!alsa_output_create(&format, &config)); /* Never quantize float. */
    only_format = SND_PCM_FORMAT_UNKNOWN;
    config.mode = ALSA_MODE_EXCLUSIVE;
    assert(!alsa_output_create(&format, &config));
    assert(strstr(alsa_output_get_last_error(), "hardware PCM"));
    config.mode = ALSA_MODE_SHARED;
    reject_rate = 1;
    assert(!alsa_output_create(&format, &config));
    assert(strstr(alsa_output_get_last_error(), "exact sample rate"));
    reject_rate = 0;
    config.buffer_duration_ms = 9;
    assert(!alsa_output_create(&format, &config));
    config.buffer_duration_ms = 100;
    format.is_float = 0;
    significant_bits = 24;
    assert(!alsa_output_create(&format, &config));
    assert(strstr(alsa_output_get_last_error(), "insufficient significant bits"));
    significant_bits = 0;
    format.block_align = 7;
    assert(!alsa_output_create(&format, &config));
    LinuxAudioConfig invalid = {(LinuxAudioBackend)99, NULL};
    assert(!audio_output_init(&format, &invalid));
    assert(strstr(audio_output_get_error(NULL), "Unsupported Linux"));
    audio_output_close(NULL);
    puts("ALSA state, packing, partial write, recovery and rejection tests passed");
    return 0;
}
