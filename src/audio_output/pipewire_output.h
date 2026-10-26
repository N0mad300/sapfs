#ifndef PIPEWIRE_OUTPUT_H
#define PIPEWIRE_OUTPUT_H

#include "audio_output.h"

typedef enum {
    PIPEWIRE_MODE_SHARED = 0,
    /* Exclusive session-manager linking and no client resampling/remixing.
     * This does not certify the server/device path as bit-perfect. */
    PIPEWIRE_MODE_EXCLUSIVE = 1
} PipewireMode;

typedef struct {
    PipewireMode mode;
    const char* target;               /* NULL: session-manager default sink;
                                        otherwise node.name or object.serial. */
    const char* remote_name;          /* NULL: default server. Native PipeWire
                                        PIPEWIRE_REMOTE environment precedence applies. */
    const char* stream_name;          /* NULL: sapfs. */
    unsigned int buffer_duration_ms; /* Application queue: 0 = 100; 10..5000. */
    unsigned int connect_timeout_ms; /* Connection/stall timeout; 0 = 5000; max 60000. */
} PipewireConfig;

typedef struct PipewireOutput PipewireOutput;

PipewireOutput* pipewire_output_create(const AudioFormat* format, const PipewireConfig* config);
int pipewire_output_start(PipewireOutput* output);
int pipewire_output_stop(PipewireOutput* output);
int pipewire_output_pause(PipewireOutput* output);
int pipewire_output_resume(PipewireOutput* output);
/* Nonblocking, frame-aligned partial writes; 0 means backpressure. */
int pipewire_output_write(PipewireOutput* output, const void* data, size_t num_frames);
AudioState pipewire_output_get_state(PipewireOutput* output);
int pipewire_output_get_available_frames(PipewireOutput* output);
int pipewire_output_get_queued_frames(PipewireOutput* output);
/* Signal EOF and poll: 1 = drained, 0 = pending, -1 = failure.
 * A later write starts another segment (for looping). */
int pipewire_output_drain(PipewireOutput* output);
const char* pipewire_output_get_error(PipewireOutput* output);
const char* pipewire_output_get_last_error(void);
void pipewire_output_close(PipewireOutput* output);

typedef void (*PipewireDeviceCallback)(const char* name, const char* description,
                                      const char* serial, void* user_data);
/* Snapshot playback sinks. Strings are valid only during the callback. */
int pipewire_output_list_devices(const char* remote_name, PipewireDeviceCallback callback,
                                void* user_data);

#endif
