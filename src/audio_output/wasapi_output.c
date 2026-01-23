#include "wasapi_output.h"
#include "ring_buffer.h"

/* Required for MSVC compatibility */
#define COBJMACROS
#define CINTERFACE

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <process.h>
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

    /* MMCSS Handle */
    HANDLE mmcss_handle;

    /* Ring Buffer */
    RingBuffer* ring_buffer;
    HANDLE consumer_thread;
    HANDLE stop_event;
    
    /* Error handling */
    char error_msg[512];
};

/* Helper function to set error message */
static void set_error(AudioOutput* output, const char* msg, HRESULT hr) {
    if (hr != S_OK) {
        snprintf(output->error_msg, sizeof(output->error_msg), "%s (HRESULT: 0x%08lX)", msg, hr);
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

static unsigned int __stdcall wasapi_consumer_thread(void* arg) {
    AudioOutput* output = (AudioOutput*)arg;
    HRESULT hr;
    BYTE* buffer;
    HANDLE wait_handles[2] = { output->stop_event, output->buffer_ready_event };
    
    /* 1. Register MMCSS (High Priority) */
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &task_index);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
    }

    while (1) {
        /* Wait for either Stop Signal OR WASAPI Ready Event */
        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        
        if (wait_result == WAIT_OBJECT_0) {
            /* Stop event signaled */
            break; 
        }
        
        if (wait_result != WAIT_OBJECT_0 + 1) {
            /* Timeout or error - unexpected */
            continue;
        }

        /* WASAPI needs data! */
        UINT32 frames_available_in_device = 0;
        
        if (output->mode == WASAPI_MODE_EXCLUSIVE) {
            frames_available_in_device = output->buffer_frame_count;
        } else {
            /* Shared mode padding check */
            UINT32 padding;
            hr = IAudioClient_GetCurrentPadding(output->audio_client, &padding);
            if (SUCCEEDED(hr)) {
                frames_available_in_device = output->buffer_frame_count - padding;
            }
        }
        
        if (frames_available_in_device == 0) continue;

        /* Get buffer from WASAPI */
        hr = IAudioRenderClient_GetBuffer(output->render_client, frames_available_in_device, &buffer);
        
        if (SUCCEEDED(hr)) {
            /* Pull from Ring Buffer */
            size_t bytes_needed = frames_available_in_device * output->format.block_align;
            
            /* NON-BLOCKING READ: Get what we have */
            size_t bytes_read = ring_buffer_read(output->ring_buffer, buffer, bytes_needed);
            
            /* Fill remainder with silence (Underrun protection) */
            if (bytes_read < bytes_needed) {
                memset(buffer + bytes_read, 0, bytes_needed - bytes_read);
            }
            
            /* Release buffer to hardware */
            IAudioRenderClient_ReleaseBuffer(output->render_client, frames_available_in_device, 0);
        }
    }

    /* Cleanup MMCSS */
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return 0;
}

AudioOutput* wasapi_output_create(const AudioFormat* format, const WasapiConfig* config) {
    AudioOutput* output = NULL;
    HRESULT hr;
    REFERENCE_TIME buffer_duration;
    
    /* Validate format */
    if (!format) return NULL;
    
    /* Allocate output structure */
    output = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!output) return NULL;
    
    /* Copy format and mode */
    memcpy(&output->format, format, sizeof(AudioFormat));
    audio_format_to_waveformatex(format, &output->wave_format);
    output->state = AUDIO_STATE_STOPPED;
    output->mode = config ? config->mode : WASAPI_MODE_SHARED;

    /* Initialize Ring Buffer (2 Seconds Capacity) */
    size_t ring_size = format->sample_rate * format->block_align * 2; 
    output->ring_buffer = ring_buffer_create(ring_size);
    if (!output->ring_buffer) {
        free(output);
        return NULL;
    }
    
    /* Initialize COM */
    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        set_error(output, "Failed to initialize COM", hr);
        ring_buffer_destroy(output->ring_buffer);
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
    if (FAILED(hr)) goto error_cleanup;
    
    /* Get default audio endpoint */
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        output->enumerator,
        eRender,
        eConsole,
        &output->device
    );
    if (FAILED(hr)) goto error_cleanup;
    
    /* Activate audio client */
    hr = IMMDevice_Activate(
        output->device,
        &IID_IAudioClient,
        CLSCTX_ALL,
        NULL,
        (void**)&output->audio_client
    );
    if (FAILED(hr)) goto error_cleanup;

    /* Create event for buffer synchronization (Used for BOTH modes now) */
    output->buffer_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!output->buffer_ready_event) goto error_cleanup;
    
    /* Initialize based on mode */
    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        
        /* Get device period for exclusive mode */
        REFERENCE_TIME min_p, def_p;
        hr = IAudioClient_GetDevicePeriod(output->audio_client, &def_p, &min_p);
        if (FAILED(hr)) goto error_cleanup;

        REFERENCE_TIME period = def_p;
        
        /* Initialize in exclusive mode */
        hr = IAudioClient_Initialize(
            output->audio_client,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            period,
            period,
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
                    
                    if (SUCCEEDED(hr)) {
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
            }
        }
        
    } else {
        unsigned int buffer_ms = (config && config->buffer_duration_ms > 0) 
                                 ? config->buffer_duration_ms 
                                 : 100;
        buffer_duration = (REFERENCE_TIME)(buffer_ms * 10000);
        
        /* Initialize audio client in shared mode */
        hr = IAudioClient_Initialize(
            output->audio_client,
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK, /* Enable Event Driven Mode */
            buffer_duration,
            0,
            &output->wave_format,
            NULL
        );
    }

    if (FAILED(hr)) goto error_cleanup;

    /* Set the event handle for both modes */
    hr = IAudioClient_SetEventHandle(output->audio_client, output->buffer_ready_event);
    if (FAILED(hr)) goto error_cleanup;
    
    /* Get buffer size */
    hr = IAudioClient_GetBufferSize(output->audio_client, &output->buffer_frame_count);
    if (FAILED(hr)) goto error_cleanup;
    
    /* Get render client */
    hr = IAudioClient_GetService(
        output->audio_client,
        &IID_IAudioRenderClient,
        (void**)&output->render_client
    );
    if (FAILED(hr)) goto error_cleanup;
    
    return output;

error_cleanup:
    audio_output_close(output);
    return NULL;
}

int audio_output_start(AudioOutput* output) {
    if (!output || output->state == AUDIO_STATE_PLAYING) return 0;

    output->stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        BYTE* buffer;
        HRESULT hr = IAudioRenderClient_GetBuffer(
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

    output->consumer_thread = (HANDLE)_beginthreadex(NULL, 0, wasapi_consumer_thread, output, 0, NULL);
    if (!output->consumer_thread) return -1;
    
    HRESULT hr = IAudioClient_Start(output->audio_client);
    if (FAILED(hr)) return -1;
    
    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_stop(AudioOutput* output) {
    if (!output || output->state == AUDIO_STATE_STOPPED) return 0;
    
    IAudioClient_Stop(output->audio_client);
    
    if (output->stop_event && output->consumer_thread) {
        SetEvent(output->stop_event);
        WaitForSingleObject(output->consumer_thread, INFINITE);
        CloseHandle(output->consumer_thread);
        CloseHandle(output->stop_event);
        output->consumer_thread = NULL;
        output->stop_event = NULL;
    }
    
    IAudioClient_Reset(output->audio_client);
    
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
    if (!output || !data) return -1;

    size_t bytes_to_write = num_samples * output->format.block_align;

    size_t written = ring_buffer_write(output->ring_buffer, data, bytes_to_write);
    
    return (int)(written / output->format.block_align);
}

AudioState audio_output_get_state(AudioOutput* output) {
    if (!output) {
        return AUDIO_STATE_STOPPED;
    }
    return output->state;
}

int audio_output_get_available_frames(AudioOutput* output) {
    if (!output || !output->ring_buffer) return -1;
    
    /* Use the getter function instead of direct access */
    size_t capacity = ring_buffer_get_capacity(output->ring_buffer);
    size_t filled = ring_buffer_available(output->ring_buffer);
    
    size_t bytes_free = capacity - filled;
    
    return (int)(bytes_free / output->format.block_align);
}

const char* audio_output_get_error(AudioOutput* output) {
    if (!output) {
        return "Invalid audio output handle";
    }
    return output->error_msg[0] ? output->error_msg : NULL;
}

void audio_output_close(AudioOutput* output) {
    if (!output) return;
    audio_output_stop(output);
    
    if (output->render_client) IAudioRenderClient_Release(output->render_client);
    if (output->audio_client) IAudioClient_Release(output->audio_client);
    if (output->device) IMMDevice_Release(output->device);
    if (output->enumerator) IMMDeviceEnumerator_Release(output->enumerator);
    if (output->buffer_ready_event) CloseHandle(output->buffer_ready_event);
    
    if (output->ring_buffer) ring_buffer_destroy(output->ring_buffer);
    
    CoUninitialize();
    free(output);
}