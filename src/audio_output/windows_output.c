#include "windows_output.h"

#ifdef _WIN32

#include "wasapi_output.h"
#include "asio_output.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct AudioOutput {
    WindowsAudioBackend backend;
    void* impl;
    uint32_t sample_rate;
    char error_msg[512];
};

static char g_last_error[512];

static void set_last_error(const char* msg) {
    if (!msg || !msg[0]) msg = "Failed to initialize Windows audio output";
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

AudioOutput* windows_output_create(const AudioFormat* format, const WindowsAudioConfig* config) {
    if (!format) return NULL;

    AudioOutput* output = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!output) {
        set_last_error("Failed to allocate Windows audio output");
        return NULL;
    }

    output->backend = config ? config->backend : WINDOWS_AUDIO_BACKEND_WASAPI;
    output->sample_rate = format->sample_rate;

    if (output->backend == WINDOWS_AUDIO_BACKEND_ASIO) {
        output->impl = asio_output_create(format, (const AsioConfig*)config->backend_config);
        if (!output->impl) {
            set_last_error(asio_output_get_last_error());
            free(output);
            return NULL;
        }
    } else {
        const WasapiConfig* wasapi_config = config ? (const WasapiConfig*)config->backend_config : NULL;
        output->impl = wasapi_output_create(format, wasapi_config);
        if (!output->impl) {
            set_last_error("Failed to initialize WASAPI output");
            free(output);
            return NULL;
        }
    }

    g_last_error[0] = '\0';
    return output;
}

int audio_output_start(AudioOutput* output) {
    if (!output) return -1;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_start((AsioOutput*)output->impl)
         : wasapi_output_start((WasapiOutput*)output->impl);
}

int audio_output_stop(AudioOutput* output) {
    if (!output) return -1;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_stop((AsioOutput*)output->impl)
         : wasapi_output_stop((WasapiOutput*)output->impl);
}

int audio_output_pause(AudioOutput* output) {
    if (!output) return -1;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_pause((AsioOutput*)output->impl)
         : wasapi_output_pause((WasapiOutput*)output->impl);
}

int audio_output_resume(AudioOutput* output) {
    if (!output) return -1;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_resume((AsioOutput*)output->impl)
         : wasapi_output_resume((WasapiOutput*)output->impl);
}

int audio_output_write(AudioOutput* output, const void* data, size_t num_samples) {
    if (!output) return -1;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_write((AsioOutput*)output->impl, data, num_samples)
         : wasapi_output_write((WasapiOutput*)output->impl, data, num_samples);
}

AudioState audio_output_get_state(AudioOutput* output) {
    if (!output) return AUDIO_STATE_STOPPED;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_get_state((AsioOutput*)output->impl)
         : wasapi_output_get_state((WasapiOutput*)output->impl);
}

int audio_output_get_available_frames(AudioOutput* output) {
    if (!output) return -1;
    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_get_available_frames((AsioOutput*)output->impl)
         : wasapi_output_get_available_frames((WasapiOutput*)output->impl);
}

const char* audio_output_get_error(AudioOutput* output) {
    if (!output) return g_last_error[0] ? g_last_error : "Invalid audio output handle";

    return (output->backend == WINDOWS_AUDIO_BACKEND_ASIO)
         ? asio_output_get_error((AsioOutput*)output->impl)
         : wasapi_output_get_error((WasapiOutput*)output->impl);
}

int audio_output_get_queued_frames(AudioOutput* output) {
    int available = audio_output_get_available_frames(output);
    return available < 0 ? -1 : (int)(output->sample_rate * 2) - available;
}

void audio_output_close(AudioOutput* output) {
    if (!output) return;

    if (output->backend == WINDOWS_AUDIO_BACKEND_ASIO) {
        asio_output_close((AsioOutput*)output->impl);
    } else {
        wasapi_output_close((WasapiOutput*)output->impl);
    }

    free(output);
}

#endif /* _WIN32 */
