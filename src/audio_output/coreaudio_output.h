#ifndef COREAUDIO_OUTPUT_H
#define COREAUDIO_OUTPUT_H

#include "audio_output.h"

typedef enum {
    COREAUDIO_MODE_SHARED = 0,
    COREAUDIO_MODE_HOG    = 1
} CoreAudioMode;

typedef struct {
    CoreAudioMode mode;
} CoreAudioConfig;

AudioOutput* coreaudio_output_create(const AudioFormat* format, const CoreAudioConfig* config);

#endif /* COREAUDIO_OUTPUT_H */
