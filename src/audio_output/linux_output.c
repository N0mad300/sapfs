#include "linux_output.h"

#ifdef __linux__

#include "alsa_output.h"
#include <stdio.h>
#include <stdlib.h>

struct AudioOutput {
    LinuxAudioBackend backend;
    AlsaOutput* impl;
};

static _Thread_local char last_error[512];

AudioOutput* linux_output_create(const AudioFormat* format, const LinuxAudioConfig* config) {
    AudioOutput* output;
    last_error[0] = '\0';
    if (config && config->backend != LINUX_AUDIO_BACKEND_ALSA) {
        snprintf(last_error, sizeof(last_error), "Unsupported Linux audio backend");
        return NULL;
    }
    output = calloc(1, sizeof(*output));
    if (!output) {
        snprintf(last_error, sizeof(last_error), "Failed to allocate Linux audio output");
        return NULL;
    }
    output->backend = LINUX_AUDIO_BACKEND_ALSA;
    output->impl = alsa_output_create(format, config ? config->backend_config : NULL);
    if (!output->impl) {
        snprintf(last_error, sizeof(last_error), "%s", alsa_output_get_last_error());
        free(output);
        return NULL;
    }
    return output;
}

int audio_output_start(AudioOutput* output) {
    return output ? alsa_output_start(output->impl) : -1;
}

int audio_output_stop(AudioOutput* output) {
    return output ? alsa_output_stop(output->impl) : -1;
}

int audio_output_pause(AudioOutput* output) {
    return output ? alsa_output_pause(output->impl) : -1;
}

int audio_output_resume(AudioOutput* output) {
    return output ? alsa_output_resume(output->impl) : -1;
}

int audio_output_write(AudioOutput* output, const void* data, size_t num_frames) {
    return output ? alsa_output_write(output->impl, data, num_frames) : -1;
}

AudioState audio_output_get_state(AudioOutput* output) {
    return output ? alsa_output_get_state(output->impl) : AUDIO_STATE_STOPPED;
}

int audio_output_get_available_frames(AudioOutput* output) {
    return output ? alsa_output_get_available_frames(output->impl) : -1;
}

int audio_output_get_queued_frames(AudioOutput* output) {
    return output ? alsa_output_get_queued_frames(output->impl) : -1;
}

const char* audio_output_get_error(AudioOutput* output) {
    if (output) return alsa_output_get_error(output->impl);
    return last_error[0] ? last_error : "Invalid audio output handle or format";
}

void audio_output_close(AudioOutput* output) {
    if (!output) return;
    alsa_output_close(output->impl);
    free(output);
}

#endif
