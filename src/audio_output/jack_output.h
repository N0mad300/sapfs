#ifndef JACK_OUTPUT_H
#define JACK_OUTPUT_H

#include "audio_output.h"

typedef enum {
    JACK_CONNECT_AUTO = 0,   /* First N matching input ports; physical by default. */
    JACK_CONNECT_MANUAL = 1  /* Keep queued samples until every channel is routed. */
} JackConnectionMode;

typedef struct {
    const char* server_name;       /* NULL: JACK default/environment. Never starts a server. */
    const char* client_name;       /* NULL: sapfs. JACK may make the name unique. */
    JackConnectionMode connection_mode;
    const char* port_pattern;      /* Optional JACK regex for destination audio input ports. */
    const char* const* ports;      /* Optional explicit destinations in source channel order. */
    size_t num_ports;              /* Must equal num_channels when ports is provided. */
    unsigned int buffer_duration_ms; /* Queue: 0 = 100; 10..5000; at least two initial periods. */
    unsigned int timeout_ms;       /* Control/stall/reconnection waits: 0 = 5000; max 60000. */
} JackConfig;

typedef struct JackOutput JackOutput;

/* JACK is shared, planar float32 at the server rate. No resampling or exclusive
 * hardware access is performed. Integer PCM above 24 valid bits loses precision.
 * Control/write calls use a single owner thread; close requires no concurrent
 * API calls. Callbacks consume a preallocated lock-free SPSC queue. */
JackOutput* jack_output_create(const AudioFormat* format, const JackConfig* config);
int jack_output_start(JackOutput* output);
int jack_output_stop(JackOutput* output);
int jack_output_pause(JackOutput* output);
int jack_output_resume(JackOutput* output);
/* Accepted frames; 0 = backpressure; -1 = failure. Conversion happens here. */
int jack_output_write(JackOutput* output, const void* data, size_t num_frames);
AudioState jack_output_get_state(JackOutput* output);
int jack_output_get_available_frames(JackOutput* output);
int jack_output_get_queued_frames(JackOutput* output);
int jack_output_drain(JackOutput* output); /* 1 done, 0 pending, -1 error. */
const char* jack_output_get_error(JackOutput* output);
const char* jack_output_get_last_error(void);
const char* jack_output_get_client_name(JackOutput* output);
const char* jack_output_get_port_name(JackOutput* output, unsigned int channel);
unsigned int jack_output_get_xrun_count(JackOutput* output);
void jack_output_close(JackOutput* output);

typedef void (*JackPortCallback)(const char* name, int is_physical, void* user_data);
/* Snapshot destination audio input ports, without activating or starting JACK. */
int jack_output_list_ports(const char* server_name, const char* pattern,
                          JackPortCallback callback, void* user_data);

#endif
