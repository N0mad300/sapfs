#ifndef COREAUDIO_OUTPUT_H
#define COREAUDIO_OUTPUT_H

#include "audio_output.h"

AudioOutput* coreaudio_output_create(const AudioFormat* format);

#endif /* COREAUDIO_OUTPUT_H */
