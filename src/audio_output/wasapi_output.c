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

AudioOutput* wasapi_output_create(const AudioFormat* format) {
    AudioOutput* output = NULL;
    HRESULT hr;
    
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
    
    /* Copy format */
    memcpy(&output->format, format, sizeof(AudioFormat));
    audio_format_to_waveformatex(format, &output->wave_format);
    output->state = AUDIO_STATE_STOPPED;
    
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
    
    /* Initialize audio client in shared mode */
    hr = IAudioClient_Initialize(
        output->audio_client,
        AUDCLNT_SHAREMODE_SHARED,
        0,
        10000000,  /* 1 second buffer */
        0,
        &output->wave_format,
        NULL
    );
    
    if (FAILED(hr)) {
        set_error(output, "Failed to initialize audio client", hr);
        IAudioClient_Release(output->audio_client);
        IMMDevice_Release(output->device);
        IMMDeviceEnumerator_Release(output->enumerator);
        CoUninitialize();
        free(output);
        return NULL;
    }
    
    /* Get buffer size */
    hr = IAudioClient_GetBufferSize(output->audio_client, &output->buffer_frame_count);
    if (FAILED(hr)) {
        set_error(output, "Failed to get buffer size", hr);
        IAudioClient_Release(output->audio_client);
        IMMDevice_Release(output->device);
        IMMDeviceEnumerator_Release(output->enumerator);
        CoUninitialize();
        free(output);
        return NULL;
    }
    
    /* Get render client */
    hr = IAudioClient_GetService(
        output->audio_client,
        &IID_IAudioRenderClient,
        (void**)&output->render_client
    );
    
    if (FAILED(hr)) {
        set_error(output, "Failed to get render client", hr);
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
    UINT32 padding;
    UINT32 available_frames;
    BYTE* buffer;
    UINT32 frames_to_write;
    
    if (!output || !data) {
        return -1;
    }
    
    /* Get current padding (frames already in buffer) */
    hr = IAudioClient_GetCurrentPadding(output->audio_client, &padding);
    if (FAILED(hr)) {
        set_error(output, "Failed to get current padding", hr);
        return -1;
    }
    
    /* Calculate available frames */
    available_frames = output->buffer_frame_count - padding;
    
    if (available_frames == 0) {
        /* Buffer is full, wait a bit */
        Sleep(10);
        return 0;
    }
    
    /* Don't write more than available */
    frames_to_write = (UINT32)num_samples;
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
    
    /* Uninitialize COM */
    CoUninitialize();
    
    /* Free structure */
    free(output);
}