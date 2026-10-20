#include "alsa_output.h"

#ifdef __linux__

#include <alsa/asoundlib.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AlsaOutput {
    snd_pcm_t* pcm;
    AudioFormat format;
    AudioState state;
    snd_pcm_format_t wire_format;
    snd_pcm_uframes_t buffer_frames;
    size_t input_bytes_per_sample;
    size_t wire_bytes_per_sample;
    size_t chunk_frames;
    uint8_t* conversion_buffer;
    char error_msg[512];
};

static _Thread_local char last_error[512];

static int set_error(AlsaOutput* output, const char* message, int code) {
    if (code < 0)
        snprintf(output->error_msg, sizeof(output->error_msg), "%s: %s", message, snd_strerror(code));
    else
        snprintf(output->error_msg, sizeof(output->error_msg), "%s", message);
    return -1;
}

/* Recover without blocking the player's controls while a device is suspended. */
static int recover(AlsaOutput* output, int code) {
    int err;
    if (code == -EAGAIN || code == -EINTR) return 0;
    if (code == -EPIPE) {
        err = snd_pcm_prepare(output->pcm);
    } else if (code == -ESTRPIPE) {
        err = snd_pcm_resume(output->pcm);
        if (err == -EAGAIN) return 0;
        if (err < 0) err = snd_pcm_prepare(output->pcm);
    } else {
        return set_error(output, "ALSA playback failed", code);
    }
    return err < 0 ? set_error(output, "ALSA recovery failed", err) : 1;
}

static snd_pcm_format_t input_format(const AudioFormat* format, size_t bytes) {
    if (format->is_float)
        return format->bits_per_sample == 32 && bytes == 4
             ? SND_PCM_FORMAT_FLOAT_LE : SND_PCM_FORMAT_UNKNOWN;
    switch (format->bits_per_sample) {
        case 8:  return bytes == 1 ? SND_PCM_FORMAT_U8 : SND_PCM_FORMAT_UNKNOWN;
        case 16: return bytes == 2 ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_UNKNOWN;
        case 24: return bytes == 3 ? SND_PCM_FORMAT_S24_3LE
                                 : bytes == 4 ? SND_PCM_FORMAT_S24_LE : SND_PCM_FORMAT_UNKNOWN;
        case 32: return bytes == 4 ? SND_PCM_FORMAT_S32_LE : SND_PCM_FORMAT_UNKNOWN;
        default: return SND_PCM_FORMAT_UNKNOWN;
    }
}

AlsaOutput* alsa_output_create(const AudioFormat* format, const AlsaConfig* config) {
    AlsaOutput* output = calloc(1, sizeof(*output));
    snd_pcm_hw_params_t* hw = NULL;
    snd_pcm_sw_params_t* sw = NULL;
    snd_pcm_format_t native;
    AlsaMode mode = config ? config->mode : ALSA_MODE_EXCLUSIVE;
    const char* device = config && config->device ? config->device
                       : mode == ALSA_MODE_SHARED ? "default" : "hw:0,0";
    unsigned int buffer_ms = config && config->buffer_duration_ms ? config->buffer_duration_ms : 100;
    unsigned int buffer_us = buffer_ms * 1000;
    unsigned int period_us = buffer_us / 4;
    unsigned int rate_num, rate_den;
    int err;

    last_error[0] = '\0';
    if (!output) {
        snprintf(last_error, sizeof(last_error), "Failed to allocate ALSA output");
        return NULL;
    }
    if (!format || !format->num_channels || format->num_channels > 8 ||
        format->sample_rate < 8000 || format->sample_rate > 192000 ||
        !format->block_align || format->block_align % format->num_channels ||
        format->valid_bits_per_sample > format->bits_per_sample ||
        (mode != ALSA_MODE_EXCLUSIVE && mode != ALSA_MODE_SHARED) ||
        buffer_ms < 10 || buffer_ms > 5000) {
        set_error(output, "Invalid ALSA format or configuration", 0);
        goto fail;
    }
    output->format = *format;
    output->input_bytes_per_sample = format->block_align / format->num_channels;
    native = input_format(format, output->input_bytes_per_sample);
    if (native == SND_PCM_FORMAT_UNKNOWN) {
        set_error(output, "Unsupported ALSA input representation", 0);
        goto fail;
    }

    err = snd_pcm_open(&output->pcm, device, SND_PCM_STREAM_PLAYBACK,
                      SND_PCM_NONBLOCK | SND_PCM_NO_AUTO_RESAMPLE |
                      SND_PCM_NO_AUTO_CHANNELS | SND_PCM_NO_AUTO_FORMAT);
    if (err < 0) {
        snprintf(output->error_msg, sizeof(output->error_msg), "Cannot open ALSA device '%s': %s", device, snd_strerror(err));
        goto fail;
    }
    if (mode == ALSA_MODE_EXCLUSIVE && snd_pcm_type(output->pcm) != SND_PCM_TYPE_HW) {
        set_error(output, "Exclusive ALSA output requires a hardware PCM (e.g. hw:0,0); plugins are not bit-perfect", 0);
        goto fail;
    }

#define CHECK(call, message) do { err = (call); if (err < 0) { set_error(output, message, err); goto fail; } } while (0)
    CHECK(snd_pcm_hw_params_malloc(&hw), "Cannot allocate ALSA hardware parameters");
    CHECK(snd_pcm_sw_params_malloc(&sw), "Cannot allocate ALSA software parameters");
    CHECK(snd_pcm_hw_params_any(output->pcm, hw), "Cannot query ALSA hardware parameters");
    CHECK(snd_pcm_hw_params_set_rate_resample(output->pcm, hw, 0), "Cannot disable ALSA resampling");
    CHECK(snd_pcm_hw_params_set_access(output->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED), "Interleaved ALSA output unavailable");
    CHECK(snd_pcm_hw_params_set_channels(output->pcm, hw, format->num_channels), "ALSA device does not support the exact channel count");
    CHECK(snd_pcm_hw_params_set_rate(output->pcm, hw, format->sample_rate, 0), "ALSA device does not support the exact sample rate");

    output->wire_format = native;
    if (snd_pcm_hw_params_test_format(output->pcm, hw, native) < 0) {
        /* Only lossless integer widening is allowed; never truncate or dither. */
        if (!format->is_float && format->bits_per_sample == 24 &&
            snd_pcm_hw_params_test_format(output->pcm, hw, SND_PCM_FORMAT_S24_LE) == 0)
            output->wire_format = SND_PCM_FORMAT_S24_LE;
        else if (!format->is_float && format->bits_per_sample == 24 &&
                 snd_pcm_hw_params_test_format(output->pcm, hw, SND_PCM_FORMAT_S24_3LE) == 0)
            output->wire_format = SND_PCM_FORMAT_S24_3LE;
        else if (!format->is_float && format->bits_per_sample == 8 &&
                 snd_pcm_hw_params_test_format(output->pcm, hw, SND_PCM_FORMAT_S16_LE) == 0)
            output->wire_format = SND_PCM_FORMAT_S16_LE;
        else if (!format->is_float &&
                 snd_pcm_hw_params_test_format(output->pcm, hw, SND_PCM_FORMAT_S32_LE) == 0)
            output->wire_format = SND_PCM_FORMAT_S32_LE;
        else {
            set_error(output, "ALSA device has no lossless format for this input", 0);
            goto fail;
        }
    }
    CHECK(snd_pcm_hw_params_set_format(output->pcm, hw, output->wire_format), "Cannot set ALSA sample format");
    CHECK(snd_pcm_hw_params_set_period_time_near(output->pcm, hw, &period_us, NULL), "Cannot set ALSA period size");
    CHECK(snd_pcm_hw_params_set_buffer_time_near(output->pcm, hw, &buffer_us, NULL), "Cannot set ALSA buffer size");
    CHECK(snd_pcm_hw_params(output->pcm, hw), "Cannot apply ALSA hardware parameters");
    CHECK(snd_pcm_hw_params_get_rate_numden(hw, &rate_num, &rate_den), "Cannot verify ALSA sample rate");
    if (!rate_den || (uint64_t)rate_num != (uint64_t)format->sample_rate * rate_den) {
        set_error(output, "ALSA device changed the requested sample rate", 0);
        goto fail;
    }
    if (!format->is_float && snd_pcm_hw_params_get_sbits(hw) <
        (format->valid_bits_per_sample ? format->valid_bits_per_sample : format->bits_per_sample)) {
        set_error(output, "ALSA device has insufficient significant bits for lossless playback", 0);
        goto fail;
    }
    CHECK(snd_pcm_hw_params_get_buffer_size(hw, &output->buffer_frames), "Cannot query ALSA buffer size");
    if (!output->buffer_frames || output->buffer_frames > INT_MAX) {
        set_error(output, "Invalid ALSA buffer size", 0);
        goto fail;
    }
    CHECK(snd_pcm_sw_params_current(output->pcm, sw), "Cannot query ALSA software parameters");
    /* Start even a short final write, rather than waiting for a full buffer. */
    CHECK(snd_pcm_sw_params_set_start_threshold(output->pcm, sw, 1), "Cannot set ALSA start threshold");
    CHECK(snd_pcm_sw_params_set_avail_min(output->pcm, sw, 1), "Cannot set ALSA availability threshold");
    CHECK(snd_pcm_sw_params(output->pcm, sw), "Cannot apply ALSA software parameters");
#undef CHECK

    output->wire_bytes_per_sample = (size_t)snd_pcm_format_physical_width(output->wire_format) / 8;
    output->chunk_frames = output->buffer_frames < 4096 ? (size_t)output->buffer_frames : 4096;
    if (output->wire_format != native) {
        output->conversion_buffer = malloc(output->chunk_frames * format->num_channels * output->wire_bytes_per_sample);
        if (!output->conversion_buffer) {
            set_error(output, "Cannot allocate ALSA lossless packing buffer", 0);
            goto fail;
        }
    }
    snd_pcm_hw_params_free(hw);
    snd_pcm_sw_params_free(sw);
    return output;

fail:
    snprintf(last_error, sizeof(last_error), "%s", output->error_msg);
    if (hw) snd_pcm_hw_params_free(hw);
    if (sw) snd_pcm_sw_params_free(sw);
    alsa_output_close(output);
    return NULL;
}

int alsa_output_start(AlsaOutput* output) {
    int err;
    if (!output) return -1;
    if (output->state == AUDIO_STATE_PLAYING) return 0;
    if (output->state == AUDIO_STATE_PAUSED) return alsa_output_resume(output);
    err = snd_pcm_prepare(output->pcm);
    if (err < 0) return set_error(output, "Cannot prepare ALSA playback", err);
    /* The first write starts the device, avoiding an empty-buffer underrun. */
    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int alsa_output_stop(AlsaOutput* output) {
    int err;
    if (!output) return -1;
    err = snd_pcm_drop(output->pcm);
    if (err < 0) return set_error(output, "Cannot stop ALSA playback", err);
    output->state = AUDIO_STATE_STOPPED;
    return 0;
}

int alsa_output_pause(AlsaOutput* output) {
    int err;
    snd_pcm_state_t state;
    if (!output || output->state != AUDIO_STATE_PLAYING) return -1;
    state = snd_pcm_state(output->pcm);
    if (state == SND_PCM_STATE_XRUN) {
        err = snd_pcm_prepare(output->pcm);
        if (err < 0) return set_error(output, "Cannot prepare ALSA pause", err);
    } else if (state == SND_PCM_STATE_RUNNING) {
        err = snd_pcm_pause(output->pcm, 1);
        /* Preserve queued audio and state if hardware pause is unsupported. */
        if (err < 0) return set_error(output, "ALSA device cannot pause without discarding audio", err);
    } else if (state != SND_PCM_STATE_PREPARED) {
        return set_error(output, "ALSA device is not ready to pause", 0);
    }
    output->state = AUDIO_STATE_PAUSED;
    return 0;
}

int alsa_output_resume(AlsaOutput* output) {
    int err;
    if (!output || output->state != AUDIO_STATE_PAUSED) return -1;
    if (snd_pcm_state(output->pcm) == SND_PCM_STATE_PAUSED) {
        err = snd_pcm_pause(output->pcm, 0);
        if (err < 0) return set_error(output, "Cannot resume ALSA playback", err);
    }
    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

/* All packing uses unsigned arithmetic and explicit little-endian bytes. */
static void pack_samples(AlsaOutput* output, const uint8_t* input, size_t frames) {
    size_t count = frames * output->format.num_channels;
    for (size_t i = 0; i < count; ++i) {
        uint32_t value = 0;
        for (size_t b = 0; b < output->input_bytes_per_sample; ++b)
            value |= (uint32_t)input[i * output->input_bytes_per_sample + b] << (8 * b);
        if (output->format.bits_per_sample == 8) value ^= 0x80;
        if (output->wire_format == SND_PCM_FORMAT_S32_LE)
            value <<= 32 - output->format.bits_per_sample;
        else if (output->wire_format == SND_PCM_FORMAT_S16_LE)
            value <<= 16 - output->format.bits_per_sample;
        else if (value & 0x800000)
            value |= 0xff000000; /* Sign-extended, right-justified S24_LE. */
        for (size_t b = 0; b < output->wire_bytes_per_sample; ++b)
            output->conversion_buffer[i * output->wire_bytes_per_sample + b] = (uint8_t)(value >> (8 * b));
    }
}

int alsa_output_write(AlsaOutput* output, const void* data, size_t num_frames) {
    snd_pcm_sframes_t written;
    if (!output || (!data && num_frames) || output->state != AUDIO_STATE_PLAYING) return -1;
    if (!num_frames) return 0;
    if (num_frames > output->chunk_frames) num_frames = output->chunk_frames;
    if (output->conversion_buffer) {
        pack_samples(output, data, num_frames);
        data = output->conversion_buffer;
    }
    written = snd_pcm_writei(output->pcm, data, num_frames);
    if (written < 0) return recover(output, (int)written) < 0 ? -1 : 0;
    return (int)written;
}

AudioState alsa_output_get_state(AlsaOutput* output) {
    return output ? output->state : AUDIO_STATE_STOPPED;
}

int alsa_output_get_available_frames(AlsaOutput* output) {
    snd_pcm_sframes_t frames;
    if (!output) return -1;
    if (output->state == AUDIO_STATE_STOPPED) return (int)output->buffer_frames;
    frames = snd_pcm_avail_update(output->pcm);
    if (frames < 0) {
        if (recover(output, (int)frames) < 0) return -1;
        return 0;
    }
    return frames > (snd_pcm_sframes_t)output->buffer_frames ? (int)output->buffer_frames : (int)frames;
}

int alsa_output_get_queued_frames(AlsaOutput* output) {
    snd_pcm_sframes_t delay;
    int err;
    if (!output) return -1;
    if (output->state == AUDIO_STATE_STOPPED) return 0;
    err = snd_pcm_delay(output->pcm, &delay);
    if (err < 0) {
        int recovered = recover(output, err);
        /* A suspended device still owns queued audio while resume is pending. */
        return recovered < 0 ? -1 : recovered == 0 ? (int)output->buffer_frames : 0;
    }
    if (delay <= 0) return 0;
    return delay > INT_MAX ? INT_MAX : (int)delay;
}

const char* alsa_output_get_error(AlsaOutput* output) {
    return output ? (output->error_msg[0] ? output->error_msg : NULL) : alsa_output_get_last_error();
}

const char* alsa_output_get_last_error(void) {
    return last_error[0] ? last_error : "Failed to initialize ALSA output";
}

void alsa_output_close(AlsaOutput* output) {
    if (!output) return;
    if (output->pcm) {
        snd_pcm_drop(output->pcm);
        snd_pcm_close(output->pcm);
    }
    free(output->conversion_buffer);
    free(output);
}

#endif
