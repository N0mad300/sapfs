#ifndef WASAPI_OUTPUT_H
#define WASAPI_OUTPUT_H

#include "audio_output.h"

/**
 * WASAPI-specific implementation of audio output interface.
 * This header exposes the WASAPI implementation which can be used
 * to create an AudioOutput instance on Windows.
 */

/**
 * Create a WASAPI audio output instance.
 * This is the Windows-specific constructor that implements the AudioOutput interface.
 * 
 * @param format Audio format specification
 * @return Pointer to AudioOutput handle, or NULL on failure
 */
AudioOutput* wasapi_output_create(const AudioFormat* format);

#endif /* WASAPI_OUTPUT_H */