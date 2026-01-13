#ifndef WASAPI_OUTPUT_H
#define WASAPI_OUTPUT_H

#include "audio_output.h"

/**
 * WASAPI-specific implementation of audio output interface.
 * This header exposes the WASAPI implementation which can be used
 * to create an AudioOutput instance on Windows.
 */

/**
 * WASAPI output mode
 */
typedef enum {
    WASAPI_MODE_SHARED = 0,      /* Shared mode (default) */
    WASAPI_MODE_EXCLUSIVE = 1    /* Exclusive mode (bit-perfect, lower latency) */
} WasapiMode;

/**
 * WASAPI configuration options
 */
typedef struct {
    WasapiMode mode;             /* Shared or exclusive mode */
    unsigned int buffer_duration_ms; /* Buffer duration in milliseconds (only for shared mode) */
} WasapiConfig;

/**
 * Create a WASAPI audio output instance.
 * This is the Windows-specific constructor that implements the AudioOutput interface.
 * Allows specifying exclusive or shared mode with custom settings.
 * 
 * @param format Audio format specification
 * @return Pointer to AudioOutput handle, or NULL on failure
 */
AudioOutput* wasapi_output_create(const AudioFormat* format, const WasapiConfig* config);

#endif /* WASAPI_OUTPUT_H */