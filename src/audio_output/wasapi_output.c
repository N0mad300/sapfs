#include "wasapi_output.h"

/* Required for MSVC compatibility */
#define COBJMACROS
#define CINTERFACE

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>

const CLSID CLSID_MMDeviceEnumerator = 
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};

const IID IID_IMMDeviceEnumerator = 
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};

const IID IID_IAudioClient = 
    {0x1CB9AD4C, 0xDBFA, 0x4c32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};

const IID IID_IAudioRenderClient = 
    {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};

/* WASAPI-specific audio output structure */
struct AudioOutput {
    /* COM interfaces */
    IMMDeviceEnumerator* enumerator;
    IMMDevice* device;
    IAudioClient* audio_client;
    IAudioRenderClient* render_client;
    
    /* Audio format */
    AudioFormat format;
    WAVEFORMATEX wave_format;
    
    /* State */
    AudioState state;
    UINT32 buffer_frame_count;
    WasapiMode mode;

    /* Event for exclusive mode synchronization */
    HANDLE buffer_ready_event;
    
    /* Error handling */
    char error_msg[512];
};

/* Helper function to set error message */
static void set_error(AudioOutput* output, const char* msg, HRESULT hr) {
    if (hr != S_OK) {
        snprintf(output->error_msg, sizeof(output->error_msg), 
                 "%s (HRESULT: 0x%08lX)", msg, hr);
    } else {
        snprintf(output->error_msg, sizeof(output->error_msg), "%s", msg);
    }
}

/* Helper function to convert AudioFormat to WAVEFORMATEX */
static void audio_format_to_waveformatex(const AudioFormat* format, WAVEFORMATEX* wfx) {
    wfx->wFormatTag = WAVE_FORMAT_PCM;
    wfx->nChannels = format->num_channels;
    wfx->nSamplesPerSec = format->sample_rate;
    wfx->wBitsPerSample = format->bits_per_sample;
    wfx->nBlockAlign = format->block_align;
    wfx->nAvgBytesPerSec = format->sample_rate * format->block_align;
    wfx->cbSize = 0;
}

AudioOutput* wasapi_output_create(const AudioFormat* format, const WasapiConfig* config) {
    AudioOutput* output = NULL;
    HRESULT hr;
    REFERENCE_TIME buffer_duration;
    
    /* Validate format */
    if (!format) {
        return NULL;
    }
    
    if (format->bits_per_sample != 8 && format->bits_per_sample != 16 && 
        format->bits_per_sample != 24 && format->bits_per_sample != 32) {
        return NULL;
    }
    
    /* Allocate output structure */
    output = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!output) {
        return NULL;
    }
    
    /* Copy format and mode */
    memcpy(&output->format, format, sizeof(AudioFormat));
    audio_format_to_waveformatex(format, &output->wave_format);
    output->state = AUDIO_STATE_STOPPED;
    output->mode = config ? config->mode : WASAPI_MODE_SHARED;
    
    /* Initialize COM */
    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        set_error(output, "Failed to initialize COM", hr);
        free(output);
        return NULL;
    }
    
    /* Create device enumerator */
    hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator,
        (void**)&output->enumerator
    );
    
    if (FAILED(hr)) {
        set_error(output, "Failed to create device enumerator", hr);
        CoUninitialize();
        free(output);
        return NULL;
    }
    
    /* Get default audio endpoint */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        output->enumerator,
        eRender,
        eConsole,
        &output->device
    );
    
    if (FAILED(hr)) {
        set_error(output, "Failed to get default audio endpoint", hr);
        IMMDeviceEnumerator_Release(output->enumerator);
        CoUninitialize();
        free(output);
        return NULL;
    }
    
    /* Activate audio client */
    hr = IMMDevice_Activate(
        output->device,
        &IID_IAudioClient,
        CLSCTX_ALL,
        NULL,
        (void**)&output->audio_client
    );
    
    if (FAILED(hr)) {
        set_error(output, "Failed to activate audio client", hr);
        IMMDevice_Release(output->device);
        IMMDeviceEnumerator_Release(output->enumerator);
        CoUninitialize();
        free(output);
        return NULL;
    }
    
    /* Configure based on mode */
    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        /* Exclusive mode - bit-perfect playback */
        printf("Initializing WASAPI in EXCLUSIVE mode (bit-perfect)...\n");
        
        /* Check if format is supported in exclusive mode */
        WAVEFORMATEX* closest_match = NULL;
        hr = IAudioClient_IsFormatSupported(
            output->audio_client,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            &output->wave_format,
            &closest_match
        );
        
        if (hr == S_FALSE) {
            /* Format not supported exactly, but a close match exists */
            printf("Warning: Exact format not supported in exclusive mode.\n");
            printf("         Device suggested: %u Hz, %u ch, %u bits\n",
                   closest_match->nSamplesPerSec,
                   closest_match->nChannels,
                   closest_match->wBitsPerSample);
            
            if (closest_match) {
                CoTaskMemFree(closest_match);
            }
            
            set_error(output, "Format not supported in exclusive mode", S_OK);
            IAudioClient_Release(output->audio_client);
            IMMDevice_Release(output->device);
            IMMDeviceEnumerator_Release(output->enumerator);
            CoUninitialize();
            free(output);
            return NULL;
        } else if (FAILED(hr)) {
            set_error(output, "Failed to check format support", hr);
            IAudioClient_Release(output->audio_client);
            IMMDevice_Release(output->device);
            IMMDeviceEnumerator_Release(output->enumerator);
            CoUninitialize();
            free(output);
            return NULL;
        }

        /* Create event for buffer synchronization */
        output->buffer_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!output->buffer_ready_event) {
            set_error(output, "Failed to create buffer ready event", S_OK);
            IAudioClient_Release(output->audio_client);
            IMMDevice_Release(output->device);
            IMMDeviceEnumerator_Release(output->enumerator);
            CoUninitialize();
            free(output);
            return NULL;
        }
        
        /* Get device period for exclusive mode */
        REFERENCE_TIME default_period, min_period;
        hr = IAudioClient_GetDevicePeriod(output->audio_client, &default_period, &min_period);
        if (FAILED(hr)) {
            set_error(output, "Failed to get device period", hr);
            CloseHandle(output->buffer_ready_event);
            IAudioClient_Release(output->audio_client);
            IMMDevice_Release(output->device);
            IMMDeviceEnumerator_Release(output->enumerator);
            CoUninitialize();
            free(output);
            return NULL;
        }
        
        /* Use default period for exclusive mode (not minimum, for stability) */
        buffer_duration = default_period;
        
        printf("Exclusive mode buffer: %.2f ms (device period)\n", 
               buffer_duration / 10000.0);
        
        /* Initialize in exclusive mode */
        hr = IAudioClient_Initialize(
            output->audio_client,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,  /* Use event-driven mode */
            buffer_duration,
            buffer_duration,
            &output->wave_format,
            NULL
        );
        
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                /* Need to align buffer size */
                UINT32 aligned_frames;
                hr = IAudioClient_GetBufferSize(output->audio_client, &aligned_frames);
                if (SUCCEEDED(hr)) {
                    IAudioClient_Release(output->audio_client);
                    
                    /* Re-activate audio client */
                    hr = IMMDevice_Activate(
                        output->device,
                        &IID_IAudioClient,
                        CLSCTX_ALL,
                        NULL,
                        (void**)&output->audio_client
                    );
                    
                    if (FAILED(hr)) {
                        set_error(output, "Failed to re-activate audio client", hr);
                        CloseHandle(output->buffer_ready_event);
                        IMMDevice_Release(output->device);
                        IMMDeviceEnumerator_Release(output->enumerator);
                        CoUninitialize();
                        free(output);
                        return NULL;
                    }
                    
                    /* Calculate aligned buffer duration */
                    buffer_duration = (REFERENCE_TIME)(
                        10000000.0 * aligned_frames / output->wave_format.nSamplesPerSec + 0.5
                    );
                    
                    /* Try again with aligned buffer */
                    hr = IAudioClient_Initialize(
                        output->audio_client,
                        AUDCLNT_SHAREMODE_EXCLUSIVE,
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                        buffer_duration,
                        buffer_duration,
                        &output->wave_format,
                        NULL
                    );
                }
            }
            
            if (FAILED(hr)) {
                set_error(output, "Failed to initialize exclusive mode", hr);
                CloseHandle(output->buffer_ready_event);
                IAudioClient_Release(output->audio_client);
                IMMDevice_Release(output->device);
                IMMDeviceEnumerator_Release(output->enumerator);
                CoUninitialize();
                free(output);
                return NULL;
            }
        }

        /* Set the event handle */
        hr = IAudioClient_SetEventHandle(output->audio_client, output->buffer_ready_event);
        if (FAILED(hr)) {
            set_error(output, "Failed to set event handle", hr);
            CloseHandle(output->buffer_ready_event);
            IAudioClient_Release(output->audio_client);
            IMMDevice_Release(output->device);
            IMMDeviceEnumerator_Release(output->enumerator);
            CoUninitialize();
            free(output);
            return NULL;
        }
        
        printf("Exclusive mode initialized successfully!\n");
        
    } else {
        /* Shared mode - standard operation */
        printf("Initializing WASAPI in SHARED mode...\n");

        output->buffer_ready_event = NULL;
        
        /* Use configured buffer duration or default to 1 second */
        unsigned int buffer_ms = (config && config->buffer_duration_ms > 0) 
                                 ? config->buffer_duration_ms 
                                 : 1000;
        buffer_duration = (REFERENCE_TIME)(buffer_ms * 10000);
        
        printf("Shared mode buffer: %u ms\n", buffer_ms);
        
        /* Initialize audio client in shared mode */
        hr = IAudioClient_Initialize(
            output->audio_client,
            AUDCLNT_SHAREMODE_SHARED,
            0,
            buffer_duration,
            0,
            &output->wave_format,
            NULL
        );
        
        if (FAILED(hr)) {
            set_error(output, "Failed to initialize shared mode", hr);
            IAudioClient_Release(output->audio_client);
            IMMDevice_Release(output->device);
            IMMDeviceEnumerator_Release(output->enumerator);
            CoUninitialize();
            free(output);
            return NULL;
        }
    }
    
    /* Get buffer size */
    hr = IAudioClient_GetBufferSize(output->audio_client, &output->buffer_frame_count);
    if (FAILED(hr)) {
        set_error(output, "Failed to get buffer size", hr);
        if (output->buffer_ready_event) CloseHandle(output->buffer_ready_event);
        IAudioClient_Release(output->audio_client);
        IMMDevice_Release(output->device);
        IMMDeviceEnumerator_Release(output->enumerator);
        CoUninitialize();
        free(output);
        return NULL;
    }

    printf("Buffer size: %u frames (%.2f ms)\n", 
           output->buffer_frame_count,
           (double)output->buffer_frame_count * 1000.0 / output->format.sample_rate);
    
    /* Get render client */
    hr = IAudioClient_GetService(
        output->audio_client,
        &IID_IAudioRenderClient,
        (void**)&output->render_client
    );
    
    if (FAILED(hr)) {
        set_error(output, "Failed to get render client", hr);
        if (output->buffer_ready_event) CloseHandle(output->buffer_ready_event);
        IAudioClient_Release(output->audio_client);
        IMMDevice_Release(output->device);
        IMMDeviceEnumerator_Release(output->enumerator);
        CoUninitialize();
        free(output);
        return NULL;
    }
    
    return output;
}

int audio_output_start(AudioOutput* output) {
    HRESULT hr;
    
    if (!output) {
        return -1;
    }
    
    if (output->state == AUDIO_STATE_PLAYING) {
        return 0; /* Already playing */
    }

    /* For exclusive mode, pre-fill the buffer with silence before starting */
    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        BYTE* buffer;
        hr = IAudioRenderClient_GetBuffer(
            output->render_client,
            output->buffer_frame_count,
            &buffer
        );
        
        if (SUCCEEDED(hr)) {
            /* Fill with silence */
            memset(buffer, 0, output->buffer_frame_count * output->format.block_align);
            
            hr = IAudioRenderClient_ReleaseBuffer(
                output->render_client,
                output->buffer_frame_count,
                0
            );
            
            if (FAILED(hr)) {
                set_error(output, "Failed to release initial buffer", hr);
                return -1;
            }
        } else {
            set_error(output, "Failed to get initial buffer", hr);
            return -1;
        }
    }
    
    hr = IAudioClient_Start(output->audio_client);
    if (FAILED(hr)) {
        set_error(output, "Failed to start audio client", hr);
        return -1;
    }
    
    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_stop(AudioOutput* output) {
    HRESULT hr;
    
    if (!output) {
        return -1;
    }
    
    if (output->state == AUDIO_STATE_STOPPED) {
        return 0; /* Already stopped */
    }
    
    hr = IAudioClient_Stop(output->audio_client);
    if (FAILED(hr)) {
        set_error(output, "Failed to stop audio client", hr);
        return -1;
    }
    
    /* Reset the audio client */
    hr = IAudioClient_Reset(output->audio_client);
    if (FAILED(hr)) {
        set_error(output, "Failed to reset audio client", hr);
        return -1;
    }
    
    output->state = AUDIO_STATE_STOPPED;
    return 0;
}

int audio_output_pause(AudioOutput* output) {
    HRESULT hr;
    
    if (!output) {
        return -1;
    }
    
    if (output->state != AUDIO_STATE_PLAYING) {
        return -1; /* Can only pause when playing */
    }
    
    hr = IAudioClient_Stop(output->audio_client);
    if (FAILED(hr)) {
        set_error(output, "Failed to pause audio client", hr);
        return -1;
    }
    
    output->state = AUDIO_STATE_PAUSED;
    return 0;
}

int audio_output_resume(AudioOutput* output) {
    HRESULT hr;
    
    if (!output) {
        return -1;
    }
    
    if (output->state != AUDIO_STATE_PAUSED) {
        return -1; /* Can only resume when paused */
    }
    
    hr = IAudioClient_Start(output->audio_client);
    if (FAILED(hr)) {
        set_error(output, "Failed to resume audio client", hr);
        return -1;
    }
    
    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_write(AudioOutput* output, const void* data, size_t num_samples) {
    HRESULT hr;
    BYTE* buffer;
    
    if (!output || !data) {
        return -1;
    }

    /* In exclusive mode, we need to use event-driven approach */
    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        /* Wait for buffer ready event with timeout */
        DWORD wait_result = WaitForSingleObject(output->buffer_ready_event, 100);
        
        if (wait_result == WAIT_TIMEOUT) {
            /* Timeout - this is unusual but not fatal */
            return 0;
        } else if (wait_result != WAIT_OBJECT_0) {
            set_error(output, "Failed to wait for buffer ready event", S_OK);
            return -1;
        }
        
        /* Buffer is ready - get it */
        hr = IAudioRenderClient_GetBuffer(
            output->render_client,
            output->buffer_frame_count,
            &buffer
        );
        
        if (FAILED(hr)) {
            set_error(output, "Failed to get buffer (exclusive)", hr);
            return -1;
        }
        
        /* Copy data to buffer */
        UINT32 frames_to_copy = (UINT32)num_samples;
        if (frames_to_copy > output->buffer_frame_count) {
            frames_to_copy = output->buffer_frame_count;
        }
        
        memcpy(buffer, data, frames_to_copy * output->format.block_align);
        
        /* If we have less data than buffer size, fill with silence */
        if (frames_to_copy < output->buffer_frame_count) {
            memset(buffer + (frames_to_copy * output->format.block_align), 
                   0, 
                   (output->buffer_frame_count - frames_to_copy) * output->format.block_align);
        }
        
        /* Release buffer */
        hr = IAudioRenderClient_ReleaseBuffer(
            output->render_client,
            output->buffer_frame_count,
            0
        );
        
        if (FAILED(hr)) {
            set_error(output, "Failed to release buffer (exclusive)", hr);
            return -1;
        }
        
        return (int)frames_to_copy;
    }
    else 
    {
        UINT32 padding;

        /* Get current padding (frames already in buffer) */
        hr = IAudioClient_GetCurrentPadding(output->audio_client, &padding);
        if (FAILED(hr)) {
            set_error(output, "Failed to get current padding", hr);
            return -1;
        }
        
        /* Calculate available frames */
        UINT32 available_frames = output->buffer_frame_count - padding;
        
        if (available_frames == 0) {
            return 0;
        }
        
        /* Don't write more than available */
        UINT32 frames_to_write = (UINT32)num_samples;
        if (frames_to_write > available_frames) {
            frames_to_write = available_frames;
        }
        
        /* Get buffer from WASAPI */
        hr = IAudioRenderClient_GetBuffer(
            output->render_client,
            frames_to_write,
            &buffer
        );
        
        if (FAILED(hr)) {
            set_error(output, "Failed to get buffer", hr);
            return -1;
        }
        
        /* Copy data to buffer */
        memcpy(buffer, data, frames_to_write * output->format.block_align);
        
        /* Release buffer */
        hr = IAudioRenderClient_ReleaseBuffer(
            output->render_client,
            frames_to_write,
            0
        );
        
        if (FAILED(hr)) {
            set_error(output, "Failed to release buffer", hr);
            return -1;
        }
        
        return (int)frames_to_write;
    }
}

AudioState audio_output_get_state(AudioOutput* output) {
    if (!output) {
        return AUDIO_STATE_STOPPED;
    }
    return output->state;
}

int audio_output_get_available_frames(AudioOutput* output) {
    HRESULT hr;
    UINT32 padding;
    
    if (!output) {
        return -1;
    }

    /* In exclusive mode, always return buffer size */
    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        return (int)output->buffer_frame_count;
    }
    
    hr = IAudioClient_GetCurrentPadding(output->audio_client, &padding);
    if (FAILED(hr)) {
        set_error(output, "Failed to get current padding", hr);
        return -1;
    }
    
    return (int)(output->buffer_frame_count - padding);
}

const char* audio_output_get_error(AudioOutput* output) {
    if (!output) {
        return "Invalid audio output handle";
    }
    return output->error_msg[0] ? output->error_msg : NULL;
}

void audio_output_close(AudioOutput* output) {
    if (!output) {
        return;
    }
    
    /* Stop playback if active */
    if (output->state != AUDIO_STATE_STOPPED) {
        audio_output_stop(output);
    }
    
    /* Release COM interfaces */
    if (output->render_client) {
        IAudioRenderClient_Release(output->render_client);
    }
    
    if (output->audio_client) {
        IAudioClient_Release(output->audio_client);
    }
    
    if (output->device) {
        IMMDevice_Release(output->device);
    }
    
    if (output->enumerator) {
        IMMDeviceEnumerator_Release(output->enumerator);
    }

    if (output->buffer_ready_event) {
        CloseHandle(output->buffer_ready_event);
    }
    
    /* Uninitialize COM */
    CoUninitialize();
    
    /* Free structure */
    free(output);
}