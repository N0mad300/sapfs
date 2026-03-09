#include "wasapi_output.h"
#include "../audio_converter.h"
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

static const GUID KSDATAFORMAT_SUBTYPE_PCM = 
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static const GUID KSDATAFORMAT_SUBTYPE_IEEE_FLOAT =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

#define SPEAKER_FRONT_LEFT              0x1
#define SPEAKER_FRONT_RIGHT             0x2
#define SPEAKER_FRONT_CENTER            0x4
#define SPEAKER_LOW_FREQUENCY           0x8
#define SPEAKER_BACK_LEFT               0x10
#define SPEAKER_BACK_RIGHT              0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER    0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER   0x80
#define SPEAKER_BACK_CENTER             0x100
#define SPEAKER_SIDE_LEFT               0x200
#define SPEAKER_SIDE_RIGHT              0x400
#define SPEAKER_TOP_CENTER              0x800
#define SPEAKER_TOP_FRONT_LEFT          0x1000
#define SPEAKER_TOP_FRONT_CENTER        0x2000
#define SPEAKER_TOP_FRONT_RIGHT         0x4000
#define SPEAKER_TOP_BACK_LEFT           0x8000
#define SPEAKER_TOP_BACK_CENTER         0x10000
#define SPEAKER_TOP_BACK_RIGHT          0x20000

/* WASAPI-specific audio output structure */
struct AudioOutput {
    IMMDeviceEnumerator* enumerator;
    IMMDevice*           device;
    IAudioClient*        audio_client;
    IAudioRenderClient*  render_client;
    
    AudioFormat format;
    WAVEFORMATEXTENSIBLE wave_format;
    
    AudioState state;
    UINT32 buffer_frame_count;
    WasapiMode mode;

    int device_bits_per_sample;

    HANDLE buffer_ready_event;
    HANDLE stop_event;
    HANDLE consumer_thread;

    HANDLE mmcss_handle;

    RingBuffer* ring_buffer;

    uint8_t* conv_buffer;
    size_t   conv_buffer_bytes;
    
    char error_msg[512];
};

static void set_error(AudioOutput* output, const char* msg, HRESULT hr) {
    if (hr != S_OK) {
        snprintf(output->error_msg, sizeof(output->error_msg), "%s (HRESULT: 0x%08lX)", msg, hr);
    } else {
        snprintf(output->error_msg, sizeof(output->error_msg), "%s", msg);
    }
}

static DWORD get_channel_mask(int num_channels) {
    switch (num_channels) {
        case 1:
            return SPEAKER_FRONT_CENTER;
        case 2:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        case 3:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER;
        case 4:
            return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | 
                   SPEAKER_BACK_LEFT  | SPEAKER_BACK_RIGHT;
        case 5:
            return SPEAKER_FRONT_LEFT   | SPEAKER_FRONT_RIGHT | 
                   SPEAKER_FRONT_CENTER |
                   SPEAKER_BACK_LEFT    | SPEAKER_BACK_RIGHT;
        case 6:
            return SPEAKER_FRONT_LEFT   | SPEAKER_FRONT_RIGHT | 
                   SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                   SPEAKER_BACK_LEFT    | SPEAKER_BACK_RIGHT;
        case 7:
            return SPEAKER_FRONT_LEFT   | SPEAKER_FRONT_RIGHT | 
                   SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                   SPEAKER_BACK_LEFT    | SPEAKER_BACK_RIGHT |
                   SPEAKER_BACK_CENTER;
        case 8:
            return SPEAKER_FRONT_LEFT   | SPEAKER_FRONT_RIGHT | 
                   SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY |
                   SPEAKER_BACK_LEFT    | SPEAKER_BACK_RIGHT |
                   SPEAKER_SIDE_LEFT    | SPEAKER_SIDE_RIGHT;
        default:
            return 0;
    }
}

static void build_waveformat(const AudioFormat* format, int bits_per_sample, int is_float, WAVEFORMATEXTENSIBLE* wfex) {
    memset(wfex, 0, sizeof(*wfex));
    
    wfex->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wfex->Format.nChannels = format->num_channels;
    wfex->Format.nSamplesPerSec = format->sample_rate;
    wfex->Format.wBitsPerSample = (WORD)bits_per_sample;
    wfex->Format.nBlockAlign = (WORD)(format->num_channels * (bits_per_sample / 8));
    wfex->Format.nAvgBytesPerSec = format->sample_rate * wfex->Format.nBlockAlign;
    wfex->Format.cbSize = 22;
    
    wfex->Samples.wValidBitsPerSample = (WORD)bits_per_sample;
    wfex->dwChannelMask = (format->channel_mask != 0) ? format->channel_mask : get_channel_mask(format->num_channels);                
    wfex->SubFormat = is_float ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
}

static unsigned int __stdcall wasapi_consumer_thread(void* arg) {
    AudioOutput* output = (AudioOutput*)arg;
    HRESULT hr;
    BYTE* device_buffer;
    HANDLE wait_handles[2] = { output->stop_event, output->buffer_ready_event };

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    
    /* Register MMCSS (High Priority) */
    DWORD task_index = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristics(TEXT("Pro Audio"), &task_index);
    if (mmcss) AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);

    const size_t float_block_align = output->format.num_channels * sizeof(float);

    while (1) {
        DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, INFINITE);
        
        if (wait_result == WAIT_OBJECT_0) break;         /* stop event */
        if (wait_result != WAIT_OBJECT_0 + 1) continue;  /* unexpected */

        UINT32 frames_needed = 0;
        if (output->mode == WASAPI_MODE_EXCLUSIVE) {
            frames_needed = output->buffer_frame_count;
        } else {
            UINT32 padding;
            hr = IAudioClient_GetCurrentPadding(output->audio_client, &padding);
            if (SUCCEEDED(hr)) {
                frames_needed = output->buffer_frame_count - padding;
            }
        }
        
        if (frames_needed == 0) continue;

        size_t float_bytes_needed = frames_needed * float_block_align;
        if (float_bytes_needed > output->conv_buffer_bytes) {
            IAudioRenderClient_ReleaseBuffer(
                output->render_client, 
                frames_needed,
                AUDCLNT_BUFFERFLAGS_SILENT
            );
            continue;
        }

        size_t float_bytes_read = ring_buffer_read(output->ring_buffer, output->conv_buffer, float_bytes_needed);
        size_t frames_read = float_bytes_read / float_block_align;

        if (float_bytes_read < float_bytes_needed) {
            memset((uint8_t*)output->conv_buffer + float_bytes_read, 0, float_bytes_needed - float_bytes_read);
            frames_read = frames_needed;
        }

        size_t total_samples = frames_needed * output->format.num_channels;
        const float* src     = (const float*)output->conv_buffer;

        hr = IAudioRenderClient_GetBuffer(output->render_client, frames_needed, &device_buffer);
        
        if (FAILED(hr)) continue;

        switch (output->device_bits_per_sample) {
            case 32:
                if (output->mode == WASAPI_MODE_SHARED) {
                    memcpy(device_buffer, src, frames_needed * float_block_align);
                } else {
                    float_to_pcm32(src, (int32_t*)device_buffer, total_samples);
                }
                break;
            case 24:
                float_to_pcm24(src, device_buffer, total_samples);
                break;
            case 16:
                float_to_pcm16(src, (int16_t*)device_buffer, total_samples, 1);
                break;
            default:
                memset(device_buffer, 0, frames_needed * (output->device_bits_per_sample / 8) * output->format.num_channels);
                break;
        }

        IAudioRenderClient_ReleaseBuffer(output->render_client, frames_needed, 0);
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
    return 0;
}

static int negotiate_exclusive_format(IAudioClient* client, const AudioFormat* format, REFERENCE_TIME period, WAVEFORMATEXTENSIBLE* out_wfex) {
    static const int depths[] = { 32, 24, 16 };
    HRESULT hr;

    for (int i = 0; i < 3; ++i) {
        int bps = depths[i];
        build_waveformat(format, bps, 0, out_wfex);

        hr = IAudioClient_IsFormatSupported(
            client,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            (WAVEFORMATEX*)out_wfex,
            NULL
        );

        if (hr == S_OK) return bps;
    }

    return 0;
}

AudioOutput* wasapi_output_create(const AudioFormat* format, const WasapiConfig* config) {
    if (!format) return NULL;

    AudioOutput* output = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!output) return NULL;
    
    memcpy(&output->format, format, sizeof(AudioFormat));
    output->state = AUDIO_STATE_STOPPED;
    output->mode = config ? config->mode : WASAPI_MODE_SHARED;

    /* Initialize Ring Buffer (2 Seconds Capacity) */
    size_t ring_size = (size_t)format->sample_rate * format->num_channels * sizeof(float) * 2; 
    output->ring_buffer = ring_buffer_create(ring_size);
    if (!output->ring_buffer) { free(output); return NULL; }
    
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        set_error(output, "Failed to initialize COM", hr);
        ring_buffer_destroy(output->ring_buffer);
        free(output);
        return NULL;
    }
    
    hr = CoCreateInstance(
        &CLSID_MMDeviceEnumerator,
        NULL,
        CLSCTX_ALL,
        &IID_IMMDeviceEnumerator,
        (void**)&output->enumerator
    );
    if (FAILED(hr)) goto error_cleanup;
    
    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(
        output->enumerator,
        eRender,
        eConsole,
        &output->device
    );
    if (FAILED(hr)) goto error_cleanup;
    
    hr = IMMDevice_Activate(
        output->device,
        &IID_IAudioClient,
        CLSCTX_ALL,
        NULL,
        (void**)&output->audio_client
    );
    if (FAILED(hr)) goto error_cleanup;

    output->buffer_ready_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!output->buffer_ready_event) goto error_cleanup;
    
    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        
        /* Get device period for exclusive mode */
        REFERENCE_TIME min_p, def_p;
        hr = IAudioClient_GetDevicePeriod(output->audio_client, &def_p, &min_p);
        if (FAILED(hr)) goto error_cleanup;

        REFERENCE_TIME period = def_p;

        int bps = negotiate_exclusive_format(output->audio_client, format, period, &output->wave_format);
        if (bps == 0) {
            set_error(output, "No supported exclusive-mode PCM format found", S_OK);
            goto error_cleanup;
        }
        output->device_bits_per_sample = bps;
        
        /* Initialize in exclusive mode */
        hr = IAudioClient_Initialize(
            output->audio_client,
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            period,
            period,
            (WAVEFORMATEX*)&output->wave_format,
            NULL
        );
        
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
            UINT32 aligned_frames;
            hr = IAudioClient_GetBufferSize(output->audio_client, &aligned_frames);
            if (FAILED(hr)) goto error_cleanup;

            period = (REFERENCE_TIME)(10000000.0 * aligned_frames / output->wave_format.Format.nSamplesPerSec + 0.5);

            IAudioClient_Release(output->audio_client);
            hr = IMMDevice_Activate(
                output->device, 
                &IID_IAudioClient,
                CLSCTX_ALL, 
                NULL,
                (void**)&output->audio_client
            );
            if (FAILED(hr)) goto error_cleanup;

            hr = IAudioClient_Initialize(
                output->audio_client,
                AUDCLNT_SHAREMODE_EXCLUSIVE,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                period, 
                period,
                (WAVEFORMATEX*)&output->wave_format,
                NULL
            );
        }
        
    } else {
        output->device_bits_per_sample = 32;
        build_waveformat(format, 32, 1, &output->wave_format);
        
        unsigned int buffer_ms = (config && config->buffer_duration_ms > 0) ? config->buffer_duration_ms : 100;
        REFERENCE_TIME buffer_duration = (REFERENCE_TIME)(buffer_ms * 10000);
        
        /* Initialize audio client in shared mode */
        hr = IAudioClient_Initialize(
            output->audio_client,
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            buffer_duration,
            0,
            &output->wave_format,
            NULL
        );
    }

    if (FAILED(hr)) { set_error(output, "Failed to initialize audio client", hr); goto error_cleanup; }

    hr = IAudioClient_SetEventHandle(output->audio_client, output->buffer_ready_event);
    if (FAILED(hr)) goto error_cleanup;
    
    hr = IAudioClient_GetBufferSize(output->audio_client, &output->buffer_frame_count);
    if (FAILED(hr)) goto error_cleanup;

    output->conv_buffer_bytes = output->buffer_frame_count * output->format.num_channels * sizeof(float);
    output->conv_buffer = (uint8_t*)malloc(output->conv_buffer_bytes);
    if (!output->conv_buffer) goto error_cleanup;
    
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
    if (!output->stop_event) return -1;

    if (output->mode == WASAPI_MODE_EXCLUSIVE) {
        BYTE* buffer;
        HRESULT hr = IAudioRenderClient_GetBuffer(
            output->render_client,
            output->buffer_frame_count,
            &buffer
        );
        
        if (SUCCEEDED(hr)) {
            memset(buffer, 0, output->buffer_frame_count * output->wave_format.Format.nBlockAlign);     
            hr = IAudioRenderClient_ReleaseBuffer(
                output->render_client,
                output->buffer_frame_count,
                0
            );
        } else {
            set_error(output, "Failed to get initial buffer", hr);
            CloseHandle(output->stop_event);
            output->stop_event = NULL;
            return -1;
        }
    }

    output->consumer_thread = (HANDLE)_beginthreadex(NULL, 0, wasapi_consumer_thread, output, 0, NULL);
    if (!output->consumer_thread) return -1;
    
    HRESULT hr = IAudioClient_Start(output->audio_client);
    if (FAILED(hr)) { set_error(output, "Failed to start audio client", hr); return -1; }
    
    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_stop(AudioOutput* output) {
    if (!output || output->state == AUDIO_STATE_STOPPED) return 0;
    
    IAudioClient_Stop(output->audio_client);
    
    if (output->stop_event && output->consumer_thread) {
        ring_buffer_cancel(output->ring_buffer);
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
    if (!output || output->state != AUDIO_STATE_PLAYING) return -1;

    HRESULT hr = IAudioClient_Stop(output->audio_client);
    if (FAILED(hr)) { set_error(output, "Failed to pause audio client", hr); return -1; }

    output->state = AUDIO_STATE_PAUSED;
    return 0;
}

int audio_output_resume(AudioOutput* output) {
    if (!output || output->state != AUDIO_STATE_PAUSED) return -1;

    HRESULT hr = IAudioClient_Start(output->audio_client);
    if (FAILED(hr)) { set_error(output, "Failed to resume audio client", hr); return -1; }

    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_write(AudioOutput* output, const void* data, size_t num_samples) {
    if (!output || !data) return -1;

    size_t bytes_to_write = num_samples * output->format.num_channels * sizeof(float);
    size_t written = ring_buffer_write(output->ring_buffer, data, bytes_to_write);
    
    return (int)(written / (output->format.num_channels * sizeof(float)));
}

AudioState audio_output_get_state(AudioOutput* output) {
    if (!output) return AUDIO_STATE_STOPPED;
    return output->state;
}

int audio_output_get_available_frames(AudioOutput* output) {
    if (!output || !output->ring_buffer) return -1;
    
    size_t capacity = ring_buffer_get_capacity(output->ring_buffer);
    size_t filled = ring_buffer_available(output->ring_buffer);
    size_t bytes_free = capacity - filled;
    size_t float_frame_bytes = output->format.num_channels * sizeof(float);
    
    return (int)(bytes_free / float_frame_bytes);
}

const char* audio_output_get_error(AudioOutput* output) {
    if (!output) return "Invalid audio output handle";
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
    if (output->conv_buffer) free(output->conv_buffer);
    
    CoUninitialize();
    free(output);
}
