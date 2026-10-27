#ifndef LINUX_OUTPUT_H
#define LINUX_OUTPUT_H

#include "audio_output.h"

typedef enum {
    LINUX_AUDIO_BACKEND_ALSA = 0,
    LINUX_AUDIO_BACKEND_PIPEWIRE = 1,
    LINUX_AUDIO_BACKEND_JACK = 2,
    LINUX_AUDIO_BACKEND_PULSEAUDIO = 3
} LinuxAudioBackend;

typedef struct {
    LinuxAudioBackend backend;
    const void* backend_config;
} LinuxAudioConfig;

AudioOutput* linux_output_create(const AudioFormat* format, const LinuxAudioConfig* config);

#endif
