#include "coreaudio_output.h"
#include "ring_buffer.h"

#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AudioOutput {
    AudioUnit   audio_unit;
    AudioFormat format;
    AudioState  state;

    RingBuffer* ring_buffer;
    size_t      bytes_per_frame;

    char error_msg[512];
};

static void set_error(AudioOutput* out, const char* msg, OSStatus st) {
    if (st != noErr)
        snprintf(out->error_msg, sizeof(out->error_msg), "%s (OSStatus %d)", msg, (int)st);
    else
        snprintf(out->error_msg, sizeof(out->error_msg), "%s", msg);
}

static OSStatus render_callback(
        void*                       inRefCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp*       inTimeStamp,
        UInt32                      inBusNumber,
        UInt32                      inNumberFrames,
        AudioBufferList*            ioData)
{
    (void)ioActionFlags;
    (void)inTimeStamp;
    (void)inBusNumber;

    AudioOutput* output = (AudioOutput*)inRefCon;

    for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
        UInt32  wanted    = ioData->mBuffers[b].mDataByteSize;
        uint8_t* dst      = (uint8_t*)ioData->mBuffers[b].mData;

        size_t got = ring_buffer_read(output->ring_buffer, dst, (size_t)wanted);

        if (got < wanted)
            memset(dst + got, 0, wanted - got);
    }

    return noErr;
}

AudioOutput* coreaudio_output_create(const AudioFormat* format) {
    if (!format) return NULL;

    AudioOutput* output = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!output) return NULL;

    memcpy(&output->format, format, sizeof(AudioFormat));
    output->state          = AUDIO_STATE_STOPPED;
    output->bytes_per_frame = (size_t)format->num_channels * sizeof(float);

    /* ---- Ring buffer: 2 seconds of float PCM ---- */
    size_t ring_bytes = (size_t)format->sample_rate
                      * format->num_channels
                      * sizeof(float)
                      * 2;
    output->ring_buffer = ring_buffer_create(ring_bytes);
    if (!output->ring_buffer) {
        set_error(output, "Failed to allocate ring buffer", noErr);
        free(output);
        return NULL;
    }

    AudioComponentDescription desc;
    memset(&desc, 0, sizeof(desc));
    desc.componentType         = kAudioUnitType_Output;
    desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;

    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp) {
        set_error(output, "AudioComponentFindNext: no default output found", noErr);
        goto fail;
    }

    OSStatus st = AudioComponentInstanceNew(comp, &output->audio_unit);
    if (st != noErr) {
        set_error(output, "AudioComponentInstanceNew failed", st);
        goto fail;
    }

    AudioStreamBasicDescription asbd;
    memset(&asbd, 0, sizeof(asbd));
    asbd.mSampleRate       = (Float64)format->sample_rate;
    asbd.mFormatID         = kAudioFormatLinearPCM;
    asbd.mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    asbd.mBitsPerChannel   = 32;
    asbd.mChannelsPerFrame = (UInt32)format->num_channels;
    asbd.mFramesPerPacket  = 1;
    asbd.mBytesPerFrame    = (UInt32)output->bytes_per_frame;
    asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

    st = AudioUnitSetProperty(
            output->audio_unit,
            kAudioUnitProperty_StreamFormat,
            kAudioUnitScope_Input,
            0,
            &asbd,
            sizeof(asbd));
    if (st != noErr) {
        set_error(output, "Failed to set stream format", st);
        goto fail_dispose;
    }

    AURenderCallbackStruct cb;
    cb.inputProc       = render_callback;
    cb.inputProcRefCon = output;

    st = AudioUnitSetProperty(
            output->audio_unit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input,
            0,
            &cb,
            sizeof(cb));
    if (st != noErr) {
        set_error(output, "Failed to set render callback", st);
        goto fail_dispose;
    }

    st = AudioUnitInitialize(output->audio_unit);
    if (st != noErr) {
        set_error(output, "AudioUnitInitialize failed", st);
        goto fail_dispose;
    }

    return output;

fail_dispose:
    AudioComponentInstanceDispose(output->audio_unit);
    output->audio_unit = NULL;
fail:
    ring_buffer_destroy(output->ring_buffer);
    free(output);
    return NULL;
}


int audio_output_start(AudioOutput* output) {
    if (!output) return -1;
    if (output->state == AUDIO_STATE_PLAYING) return 0;

    OSStatus st = AudioOutputUnitStart(output->audio_unit);
    if (st != noErr) {
        set_error(output, "AudioOutputUnitStart failed", st);
        return -1;
    }

    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_stop(AudioOutput* output) {
    if (!output) return -1;
    if (output->state == AUDIO_STATE_STOPPED) return 0;

    ring_buffer_cancel(output->ring_buffer);

    OSStatus st = AudioOutputUnitStop(output->audio_unit);
    if (st != noErr) {
        set_error(output, "AudioOutputUnitStop failed", st);
    }

    AudioUnitReset(output->audio_unit, kAudioUnitScope_Global, 0);

    output->state = AUDIO_STATE_STOPPED;
    return 0;
}

int audio_output_pause(AudioOutput* output) {
    if (!output) return -1;
    if (output->state != AUDIO_STATE_PLAYING) return -1;

    OSStatus st = AudioOutputUnitStop(output->audio_unit);
    if (st != noErr) {
        set_error(output, "Pause: AudioOutputUnitStop failed", st);
        return -1;
    }

    output->state = AUDIO_STATE_PAUSED;
    return 0;
}

int audio_output_resume(AudioOutput* output) {
    if (!output) return -1;
    if (output->state != AUDIO_STATE_PAUSED) return -1;

    OSStatus st = AudioOutputUnitStart(output->audio_unit);
    if (st != noErr) {
        set_error(output, "Resume: AudioOutputUnitStart failed", st);
        return -1;
    }

    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_write(AudioOutput* output, const void* data, size_t num_samples) {
    if (!output || !data) return -1;

    size_t bytes   = num_samples * output->bytes_per_frame;
    size_t written = ring_buffer_write(output->ring_buffer, data, bytes);

    return (int)(written / output->bytes_per_frame);
}

AudioState audio_output_get_state(AudioOutput* output) {
    if (!output) return AUDIO_STATE_STOPPED;
    return output->state;
}

int audio_output_get_available_frames(AudioOutput* output) {
    if (!output || !output->ring_buffer) return -1;

    size_t capacity   = ring_buffer_get_capacity(output->ring_buffer);
    size_t filled     = ring_buffer_available(output->ring_buffer);
    size_t bytes_free = capacity - filled;

    return (int)(bytes_free / output->bytes_per_frame);
}

const char* audio_output_get_error(AudioOutput* output) {
    if (!output) return "Invalid audio output handle";
    return output->error_msg[0] ? output->error_msg : NULL;
}

void audio_output_close(AudioOutput* output) {
    if (!output) return;

    if (output->state != AUDIO_STATE_STOPPED)
        audio_output_stop(output);

    if (output->audio_unit) {
        AudioUnitUninitialize(output->audio_unit);
        AudioComponentInstanceDispose(output->audio_unit);
        output->audio_unit = NULL;
    }

    if (output->ring_buffer) {
        ring_buffer_destroy(output->ring_buffer);
        output->ring_buffer = NULL;
    }

    free(output);
}
