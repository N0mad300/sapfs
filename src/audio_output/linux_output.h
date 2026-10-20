#ifndef LINUX_OUTPUT_H
#define LINUX_OUTPUT_H

#include "audio_output.h"

typedef enum {
    LINUX_AUDIO_BACKEND_ALSA = 0
    /* PipeWire, PulseAudio and JACK backends can be added here. */
} LinuxAudioBackend;

typedef struct {
    LinuxAudioBackend backend;
    const void* backend_config;
} LinuxAudioConfig;

AudioOutput* linux_output_create(const AudioFormat* format, const LinuxAudioConfig* config);

#endif
