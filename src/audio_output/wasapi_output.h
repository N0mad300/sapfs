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

typedef struct WasapiOutput WasapiOutput;

WasapiOutput* wasapi_output_create(const AudioFormat* format, const WasapiConfig* config);
int wasapi_output_start(WasapiOutput* output);
int wasapi_output_stop(WasapiOutput* output);
int wasapi_output_pause(WasapiOutput* output);
int wasapi_output_resume(WasapiOutput* output);
int wasapi_output_write(WasapiOutput* output, const void* data, size_t num_samples);
AudioState wasapi_output_get_state(WasapiOutput* output);
int wasapi_output_get_available_frames(WasapiOutput* output);
const char* wasapi_output_get_error(WasapiOutput* output);
void wasapi_output_close(WasapiOutput* output);

#endif /* WASAPI_OUTPUT_H */
