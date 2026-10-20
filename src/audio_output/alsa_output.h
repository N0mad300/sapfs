#ifndef ALSA_OUTPUT_H
#define ALSA_OUTPUT_H

#include "audio_output.h"

typedef enum {
    ALSA_MODE_EXCLUSIVE = 0, /* Direct hardware only; default. */
    ALSA_MODE_SHARED = 1     /* Explicitly allow ALSA plugins; no bit-perfect guarantee. */
} AlsaMode;

typedef struct {
    AlsaMode mode;
    const char* device;               /* NULL: hw:0,0 (exclusive), default (shared). */
    unsigned int buffer_duration_ms;  /* 0: 100 ms; otherwise 10..5000 ms. */
} AlsaConfig;

typedef struct AlsaOutput AlsaOutput;

AlsaOutput* alsa_output_create(const AudioFormat* format, const AlsaConfig* config);
int alsa_output_start(AlsaOutput* output);
int alsa_output_stop(AlsaOutput* output);
int alsa_output_pause(AlsaOutput* output);
int alsa_output_resume(AlsaOutput* output);
/* Nonblocking: returns accepted frames, 0 for backpressure, or -1 on error. */
int alsa_output_write(AlsaOutput* output, const void* data, size_t num_frames);
AudioState alsa_output_get_state(AlsaOutput* output);
int alsa_output_get_available_frames(AlsaOutput* output);
int alsa_output_get_queued_frames(AlsaOutput* output);
const char* alsa_output_get_error(AlsaOutput* output);
const char* alsa_output_get_last_error(void);
void alsa_output_close(AlsaOutput* output);

#endif
