#include "linux_output.h"

#ifdef __linux__

#include "alsa_output.h"
#ifdef SAPFS_HAVE_PIPEWIRE
#include "pipewire_output.h"
#endif
#ifdef SAPFS_HAVE_JACK
#include "jack_output.h"
#endif
#ifdef SAPFS_HAVE_PULSEAUDIO
#include "pulseaudio_output.h"
#endif
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    void* (*create)(const AudioFormat*, const void*);
    int (*start)(void*);
    int (*stop)(void*);
    int (*pause)(void*);
    int (*resume)(void*);
    int (*write)(void*, const void*, size_t);
    AudioState (*get_state)(void*);
    int (*get_available_frames)(void*);
    int (*get_queued_frames)(void*);
    int (*drain)(void*);
    const char* (*get_error)(void*);
    const char* (*get_last_error)(void);
    void (*close)(void*);
} LinuxAudioOps;

/* Typed adapters avoid calling functions through incompatible pointer types. */
#define BACKEND_ADAPTERS(prefix, Type, Config) \
    static void* prefix##_create_adapter(const AudioFormat* f, const void* c) { return prefix##_create(f, (const Config*)c); } \
    static int prefix##_start_adapter(void* p) { return prefix##_start((Type*)p); } \
    static int prefix##_stop_adapter(void* p) { return prefix##_stop((Type*)p); } \
    static int prefix##_pause_adapter(void* p) { return prefix##_pause((Type*)p); } \
    static int prefix##_resume_adapter(void* p) { return prefix##_resume((Type*)p); } \
    static int prefix##_write_adapter(void* p, const void* d, size_t n) { return prefix##_write((Type*)p, d, n); } \
    static AudioState prefix##_state_adapter(void* p) { return prefix##_get_state((Type*)p); } \
    static int prefix##_available_adapter(void* p) { return prefix##_get_available_frames((Type*)p); } \
    static int prefix##_queued_adapter(void* p) { return prefix##_get_queued_frames((Type*)p); } \
    static const char* prefix##_error_adapter(void* p) { return prefix##_get_error((Type*)p); } \
    static void prefix##_close_adapter(void* p) { prefix##_close((Type*)p); }

#define BACKEND_OPS(prefix, drain_fn) { \
    prefix##_create_adapter, prefix##_start_adapter, prefix##_stop_adapter, \
    prefix##_pause_adapter, prefix##_resume_adapter, prefix##_write_adapter, \
    prefix##_state_adapter, prefix##_available_adapter, prefix##_queued_adapter, \
    drain_fn, prefix##_error_adapter, prefix##_get_last_error, prefix##_close_adapter }

BACKEND_ADAPTERS(alsa_output, AlsaOutput, AlsaConfig)
static int alsa_drain_adapter(void* p) {
    int queued = alsa_output_get_queued_frames(p);
    return queued < 0 ? -1 : queued == 0;
}
static const LinuxAudioOps alsa_ops = BACKEND_OPS(alsa_output, alsa_drain_adapter);

#ifdef SAPFS_HAVE_PIPEWIRE
BACKEND_ADAPTERS(pipewire_output, PipewireOutput, PipewireConfig)
static int pipewire_drain_adapter(void* p) { return pipewire_output_drain(p); }
static const LinuxAudioOps pipewire_ops = BACKEND_OPS(pipewire_output, pipewire_drain_adapter);
#endif

#ifdef SAPFS_HAVE_JACK
BACKEND_ADAPTERS(jack_output, JackOutput, JackConfig)
static int jack_drain_adapter(void* p) { return jack_output_drain(p); }
static const LinuxAudioOps jack_ops = BACKEND_OPS(jack_output, jack_drain_adapter);
#endif

#ifdef SAPFS_HAVE_PULSEAUDIO
BACKEND_ADAPTERS(pulseaudio_output, PulseaudioOutput, PulseaudioConfig)
static int pulseaudio_drain_adapter(void* p) { return pulseaudio_output_drain(p); }
static const LinuxAudioOps pulseaudio_ops = BACKEND_OPS(pulseaudio_output, pulseaudio_drain_adapter);
#endif

struct AudioOutput {
    const LinuxAudioOps* ops;
    void* impl;
};

static _Thread_local char last_error[512];

AudioOutput* linux_output_create(const AudioFormat* format, const LinuxAudioConfig* config) {
    AudioOutput* output;
    last_error[0] = '\0';
    const LinuxAudioOps* ops;
    switch (config ? config->backend : LINUX_AUDIO_BACKEND_ALSA) {
    case LINUX_AUDIO_BACKEND_ALSA:
        ops = &alsa_ops;
        break;
    case LINUX_AUDIO_BACKEND_PIPEWIRE:
#ifdef SAPFS_HAVE_PIPEWIRE
        ops = &pipewire_ops;
        break;
#else
        snprintf(last_error, sizeof(last_error), "PipeWire support was not built; install libpipewire-0.3 development files and rebuild with WITH_PIPEWIRE=1");
        return NULL;
#endif
    case LINUX_AUDIO_BACKEND_JACK:
#ifdef SAPFS_HAVE_JACK
        ops = &jack_ops;
        break;
#else
        snprintf(last_error, sizeof(last_error), "JACK support was not built; install JACK development files and rebuild with WITH_JACK=1");
        return NULL;
#endif
    case LINUX_AUDIO_BACKEND_PULSEAUDIO:
#ifdef SAPFS_HAVE_PULSEAUDIO
        ops = &pulseaudio_ops;
        break;
#else
        snprintf(last_error, sizeof(last_error), "PulseAudio support was not built; install libpulse development files and rebuild with WITH_PULSEAUDIO=1");
        return NULL;
#endif
    default:
        snprintf(last_error, sizeof(last_error), "Unsupported Linux audio backend");
        return NULL;
    }
    output = calloc(1, sizeof(*output));
    if (!output) {
        snprintf(last_error, sizeof(last_error), "Failed to allocate Linux audio output");
        return NULL;
    }
    output->ops = ops;
    output->impl = ops->create(format, config ? config->backend_config : NULL);
    if (!output->impl) {
        snprintf(last_error, sizeof(last_error), "%s", ops->get_last_error());
        free(output);
        return NULL;
    }
    return output;
}

int audio_output_start(AudioOutput* output) {
    return output ? output->ops->start(output->impl) : -1;
}

int audio_output_stop(AudioOutput* output) {
    return output ? output->ops->stop(output->impl) : -1;
}

int audio_output_pause(AudioOutput* output) {
    return output ? output->ops->pause(output->impl) : -1;
}

int audio_output_resume(AudioOutput* output) {
    return output ? output->ops->resume(output->impl) : -1;
}

int audio_output_write(AudioOutput* output, const void* data, size_t num_frames) {
    return output ? output->ops->write(output->impl, data, num_frames) : -1;
}

AudioState audio_output_get_state(AudioOutput* output) {
    return output ? output->ops->get_state(output->impl) : AUDIO_STATE_STOPPED;
}

int audio_output_get_available_frames(AudioOutput* output) {
    return output ? output->ops->get_available_frames(output->impl) : -1;
}

int audio_output_get_queued_frames(AudioOutput* output) {
    return output ? output->ops->get_queued_frames(output->impl) : -1;
}

int audio_output_drain(AudioOutput* output) {
    return output ? output->ops->drain(output->impl) : -1;
}

const char* audio_output_get_error(AudioOutput* output) {
    if (output) return output->ops->get_error(output->impl);
    return last_error[0] ? last_error : "Invalid audio output handle or format";
}

void audio_output_close(AudioOutput* output) {
    if (!output) return;
    output->ops->close(output->impl);
    free(output);
}

#endif
