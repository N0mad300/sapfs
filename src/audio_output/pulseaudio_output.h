#ifndef PULSEAUDIO_OUTPUT_H
#define PULSEAUDIO_OUTPUT_H

#include "audio_output.h"

typedef struct {
    const char* server;             /* NULL: normal PulseAudio environment/configuration. */
    const char* device;             /* NULL: server default; explicit sink names are pinned. */
    const char* application_name;   /* NULL: sapfs. */
    const char* stream_name;        /* NULL: sapfs. */
    unsigned int buffer_duration_ms; /* Requested server queue: 0 = 100; 10..5000. */
    unsigned int timeout_ms;        /* Connection/control/stall timeout: 0 = 5000; max 60000. */
} PulseaudioConfig;

typedef struct PulseaudioOutput PulseaudioOutput;

/* Shared PCM playback; PulseAudio may mix, resample, remix and apply gain.
 * No exclusive/bit-perfect mode. No server autospawn or global configuration
 * changes. Native little-endian PCM is submitted without float conversion.
 * API calls use one owner thread; close requires no concurrent API calls.
 * Each handle owns a separate, synchronized threaded mainloop and context. */
PulseaudioOutput* pulseaudio_output_create(const AudioFormat* format, const PulseaudioConfig* config);
int pulseaudio_output_start(PulseaudioOutput* output);
int pulseaudio_output_stop(PulseaudioOutput* output);
int pulseaudio_output_pause(PulseaudioOutput* output);
int pulseaudio_output_resume(PulseaudioOutput* output);
/* Frame-aligned nonblocking partial writes; 0 = backpressure; -1 = failure. */
int pulseaudio_output_write(PulseaudioOutput* output, const void* data, size_t num_frames);
AudioState pulseaudio_output_get_state(PulseaudioOutput* output);
int pulseaudio_output_get_available_frames(PulseaudioOutput* output);
/* Estimated remaining frames, including reported sink latency. */
int pulseaudio_output_get_queued_frames(PulseaudioOutput* output);
/* Signal EOF and poll server drain: 1 done, 0 pending, -1 failure.
 * A later write starts another segment. Pause retains data; stop discards it. */
int pulseaudio_output_drain(PulseaudioOutput* output);
const char* pulseaudio_output_get_error(PulseaudioOutput* output);
const char* pulseaudio_output_get_last_error(void);
unsigned int pulseaudio_output_get_underflow_count(PulseaudioOutput* output);
void pulseaudio_output_close(PulseaudioOutput* output);

typedef void (*PulseaudioDeviceCallback)(const char* name, const char* description,
                                       unsigned int index, void* user_data);
/* Snapshot playback sinks; callback strings live only for the callback.
 * Called on the listing thread, outside the PulseAudio mainloop lock. */
int pulseaudio_output_list_devices(const char* server, PulseaudioDeviceCallback callback,
                                  void* user_data);

#endif
