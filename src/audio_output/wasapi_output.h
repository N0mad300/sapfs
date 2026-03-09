#ifndef WASAPI_OUTPUT_H
#define WASAPI_OUTPUT_H

#include "audio_output.h"

typedef enum {
    WASAPI_MODE_SHARED = 0,      /* Shared mode (default) */
    WASAPI_MODE_EXCLUSIVE = 1    /* Exclusive mode (bit-perfect, lower latency) */
} WasapiMode;

typedef struct {
    WasapiMode mode;
    unsigned int buffer_duration_ms; /* Buffer duration in milliseconds (only for shared mode) */
} WasapiConfig;

AudioOutput* wasapi_output_create(const AudioFormat* format, const WasapiConfig* config);

#endif /* WASAPI_OUTPUT_H */
