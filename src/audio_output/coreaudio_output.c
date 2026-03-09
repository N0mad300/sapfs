#include "coreaudio_output.h"
#include "ring_buffer.h"
#include "../audio_converter.h"

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct AudioOutput {
    CoreAudioMode   mode;
    AudioFormat     format;
    AudioState      state;
    
    /* ---- Shared-mode ---- */
    AudioUnit     audio_unit;

    /* ---- Hog-mode ---- */
    AudioDeviceID         device_id;
    AudioDeviceIOProcID   io_proc_id;
    pid_t                 hog_pid;
    int                   mixing_disabled;
    AudioStreamBasicDescription hw_asbd;
    uint8_t*              conv_buf;
    size_t                conv_buf_bytes;

    RingBuffer*     ring_buffer;
    size_t          bytes_per_frame;

    char error_msg[512];
};

static void set_error(AudioOutput* out, const char* msg, OSStatus st) {
    if (st != noErr)
        snprintf(out->error_msg, sizeof(out->error_msg), "%s (OSStatus %d)", msg, (int)st);
    else
        snprintf(out->error_msg, sizeof(out->error_msg), "%s", msg);
}

static OSStatus shared_render_cb(
        void*                       inRefCon,
        AudioUnitRenderActionFlags* ioActionFlags,
        const AudioTimeStamp*       inTimeStamp,
        UInt32                      inBusNumber,
        UInt32                      inNumberFrames,
        AudioBufferList*            ioData)
{
    (void)ioActionFlags; (void)inTimeStamp; (void)inBusNumber;
    (void)inNumberFrames;

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

static AudioOutput* create_shared(AudioOutput* output, const AudioFormat* format) {
    output->bytes_per_frame = (size_t)format->num_channels * sizeof(float);

    /* ---- Ring buffer: 2 seconds of float PCM ---- */
    size_t ring_bytes = (size_t)format->sample_rate
                      * format->num_channels
                      * sizeof(float)
                      * 2;
    output->ring_buffer = ring_buffer_create(ring_bytes);
    if (!output->ring_buffer) {
        set_error(output, "Failed to allocate ring buffer", noErr);
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
        return NULL;
    }

    OSStatus st = AudioComponentInstanceNew(comp, &output->audio_unit);
    if (st != noErr) {
        set_error(output, "AudioComponentInstanceNew failed", st);
        return NULL;
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

    st = AudioUnitSetProperty(output->audio_unit,
                              kAudioUnitProperty_StreamFormat,
                              kAudioUnitScope_Input, 0,
                              &asbd, sizeof(asbd));
    if (st != noErr) { set_error(output, "Failed to set stream format", st); return NULL; }

    /* Register the render callback */
    AURenderCallbackStruct cb = { shared_render_cb, output };
    st = AudioUnitSetProperty(output->audio_unit,
                              kAudioUnitProperty_SetRenderCallback,
                              kAudioUnitScope_Input, 0,
                              &cb, sizeof(cb));
    if (st != noErr) { set_error(output, "Failed to set render callback", st); return NULL; }

    st = AudioUnitInitialize(output->audio_unit);
    if (st != noErr) { set_error(output, "AudioUnitInitialize failed", st); return NULL; }

    return output;
}

static int get_device_formats(AudioDeviceID device_id,
                               AudioStreamBasicDescription** formats,
                               UInt32* count)
{
    UInt32 size = 0;
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    OSStatus st = AudioObjectGetPropertyDataSize(device_id, &pa, 0, NULL, &size);
    if (st != noErr || size == 0) return -1;

    UInt32 stream_count = size / sizeof(AudioStreamID);
    AudioStreamID* streams = (AudioStreamID*)malloc(size);
    if (!streams) return -1;

    st = AudioObjectGetPropertyData(device_id, &pa, 0, NULL, &size, streams);
    if (st != noErr) { free(streams); return -1; }

    AudioStreamID stream = streams[0];
    free(streams);

    AudioObjectPropertyAddress fpa = {
        kAudioStreamPropertyAvailablePhysicalFormats,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    st = AudioObjectGetPropertyDataSize(stream, &fpa, 0, NULL, &size);
    if (st != noErr || size == 0) return -1;

    UInt32 n = size / sizeof(AudioStreamRangedDescription);
    AudioStreamRangedDescription* ranged =
        (AudioStreamRangedDescription*)malloc(size);
    if (!ranged) return -1;

    st = AudioObjectGetPropertyData(stream, &fpa, 0, NULL, &size, ranged);
    if (st != noErr) { free(ranged); return -1; }

    AudioStreamBasicDescription* out =
        (AudioStreamBasicDescription*)malloc(n * sizeof(AudioStreamBasicDescription));
    if (!out) { free(ranged); return -1; }

    for (UInt32 i = 0; i < n; i++)
        out[i] = ranged[i].mFormat;

    free(ranged);
    *formats = out;
    *count   = n;
    return 0;
}

static int score_format(const AudioStreamBasicDescription* cand,
                         const AudioFormat* want)
{
    if (cand->mFormatID != kAudioFormatLinearPCM) return -1;
    
    int is_float = (cand->mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    if (is_float && cand->mBitsPerChannel != 32) return -1;
    if (!is_float && cand->mBitsPerChannel != 16 && cand->mBitsPerChannel != 32) return -1;

    if ((UInt32)cand->mSampleRate != want->sample_rate) return -1;
    int score = 1000;

    if (is_float) score += 200;
    if (cand->mChannelsPerFrame == (UInt32)want->num_channels) score += 100;
    if (cand->mBitsPerChannel >= want->valid_bits_per_sample) score += 10;
    score += (int)cand->mBitsPerChannel;

    return score;
}

static int negotiate_hog_format(AudioDeviceID device_id,
                                 const AudioFormat* want,
                                 AudioStreamBasicDescription* chosen)
{
    AudioStreamBasicDescription* formats = NULL;
    UInt32 count = 0;
    if (get_device_formats(device_id, &formats, &count) != 0 || count == 0) {
        free(formats);
        return -1;
    }

    int   best_score = -1;
    UInt32 best_idx  = 0;

    for (UInt32 i = 0; i < count; i++) {
        int s = score_format(&formats[i], want);
        if (s > best_score) { best_score = s; best_idx = i; }
    }

    if (best_score < 0) { free(formats); return -1; }

    *chosen = formats[best_idx];
    free(formats);
    return 0;
}

static OSStatus hog_io_proc(
        AudioObjectID           inDevice,
        const AudioTimeStamp*   inNow,
        const AudioBufferList*  inInputData,
        const AudioTimeStamp*   inInputTime,
        AudioBufferList*        outOutputData,
        const AudioTimeStamp*   inOutputTime,
        void*                   inClientData)
{
    (void)inDevice; (void)inNow; (void)inInputData;
    (void)inInputTime; (void)inOutputTime;

    AudioOutput* output = (AudioOutput*)inClientData;

    if (outOutputData->mNumberBuffers == 0) return noErr;

    UInt32 frames_needed = outOutputData->mBuffers[0].mDataByteSize
                         / output->hw_asbd.mBytesPerFrame;

    UInt32 channels      = output->hw_asbd.mChannelsPerFrame;
    UInt32 hw_bps        = output->hw_asbd.mBitsPerChannel;
    
    int    hw_is_float     = (output->hw_asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    int    non_interleaved = (output->hw_asbd.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;

    size_t float_bytes   = (size_t)frames_needed * channels * sizeof(float);

    if (float_bytes > output->conv_buf_bytes) {
        for (UInt32 b = 0; b < outOutputData->mNumberBuffers; b++)
            memset(outOutputData->mBuffers[b].mData, 0,
                   outOutputData->mBuffers[b].mDataByteSize);
        return noErr;
    }

    size_t got = ring_buffer_read(output->ring_buffer, output->conv_buf, float_bytes);

    if (got < float_bytes)
        memset(output->conv_buf + got, 0, float_bytes - got);

    float* src = (float*)output->conv_buf;

    if (hw_is_float) {
        /* === Float32 path — ring buffer currency matches wire format === */
        if (!non_interleaved) {
            /* Interleaved float32: direct copy, no conversion whatsoever */
            memcpy(outOutputData->mBuffers[0].mData, src, float_bytes);
        } else {
            /* Non-interleaved float32: de-interleave by channel */
            for (UInt32 ch = 0; ch < outOutputData->mNumberBuffers; ch++) {
                float* dst = (float*)outOutputData->mBuffers[ch].mData;
                for (UInt32 f = 0; f < frames_needed; f++)
                    dst[f] = src[f * channels + ch];
            }
        }
    } else if (!non_interleaved) {
        /* === Interleaved integer output === */
        uint8_t* dst = (uint8_t*)outOutputData->mBuffers[0].mData;
        if (hw_bps == 16) {
            float_to_pcm16(src, (int16_t*)dst,
                           (size_t)frames_needed * channels, /*dither=*/1);
        } else {  /* 32-bit integer */
            float_to_pcm32(src, (int32_t*)dst,
                           (size_t)frames_needed * channels);
        }
    } else {
        /* === Non-interleaved integer output — one buffer per channel === */
        for (UInt32 ch = 0; ch < outOutputData->mNumberBuffers; ch++) {
            if (hw_bps == 16) {
                int16_t* d16 = (int16_t*)outOutputData->mBuffers[ch].mData;
                for (UInt32 f = 0; f < frames_needed; f++) {
                    float sv = src[f * channels + ch] * 32768.0f;
                    /* TPDF dither */
                    sv += ((float)rand() / (float)RAND_MAX)
                        - ((float)rand() / (float)RAND_MAX);
                    if      (sv >=  32767.0f) d16[f] =  32767;
                    else if (sv <= -32768.0f) d16[f] = -32768;
                    else                      d16[f] = (int16_t)sv;
                }
            } else {  /* 32-bit integer */
                int32_t* d32 = (int32_t*)outOutputData->mBuffers[ch].mData;
                for (UInt32 f = 0; f < frames_needed; f++) {
                    float sv = src[f * channels + ch] * 2147483648.0f;
                    if      (sv >=  2147483647.0f) d32[f] =  2147483647;
                    else if (sv <= -2147483648.0f) d32[f] = (int32_t)(-2147483648LL);
                    else                           d32[f] = (int32_t)sv;
                }
            }
        }
    }

    return noErr;
}

static OSStatus set_hog_mode(AudioDeviceID device_id, pid_t* out_pid) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyHogMode,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    pid_t pid = getpid();
    UInt32 size = sizeof(pid);
    OSStatus st = AudioObjectSetPropertyData(device_id, &pa, 0, NULL,
                                             size, &pid);
    if (st == noErr) *out_pid = pid;
    return st;
}

static void release_hog_mode(AudioDeviceID device_id) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyHogMode,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    pid_t pid = -1;
    UInt32 size = sizeof(pid);
    AudioObjectSetPropertyData(device_id, &pa, 0, NULL, size, &pid);
}

static OSStatus set_mixing(AudioDeviceID device_id, int enable) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertySupportsMixing,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    
    if (!AudioObjectHasProperty(device_id, &pa)) return noErr;

    UInt32 val  = (UInt32)enable;
    UInt32 size = sizeof(val);
    return AudioObjectSetPropertyData(device_id, &pa, 0, NULL, size, &val);
}

static OSStatus set_physical_format(AudioDeviceID device_id,
                                    const AudioStreamBasicDescription* asbd)
{
    UInt32 size = 0;
    AudioObjectPropertyAddress spa = {
        kAudioDevicePropertyStreams,
        kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    OSStatus st = AudioObjectGetPropertyDataSize(device_id, &spa, 0, NULL, &size);
    if (st != noErr || size == 0) return st;

    AudioStreamID* streams = (AudioStreamID*)malloc(size);
    if (!streams) return kAudio_MemFullError;

    st = AudioObjectGetPropertyData(device_id, &spa, 0, NULL, &size, streams);
    AudioStreamID stream = streams[0];
    free(streams);
    if (st != noErr) return st;

    AudioObjectPropertyAddress fpa = {
        kAudioStreamPropertyPhysicalFormat,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    return AudioObjectSetPropertyData(stream, &fpa, 0, NULL,
                                      sizeof(*asbd), asbd);
}

static OSStatus set_buffer_frame_size(AudioDeviceID device_id, UInt32 frames) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(frames);
    return AudioObjectSetPropertyData(device_id, &pa, 0, NULL, size, &frames);
}

static UInt32 get_min_buffer_frame_size(AudioDeviceID device_id) {
    AudioObjectPropertyAddress pa = {
        kAudioDevicePropertyBufferFrameSizeRange,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioValueRange range = { 0, 0 };
    UInt32 size = sizeof(range);
    AudioObjectGetPropertyData(device_id, &pa, 0, NULL, &size, &range);
    return (range.mMinimum > 0) ? (UInt32)range.mMinimum : 256;
}

static AudioOutput* create_hog(AudioOutput* output, const AudioFormat* format) {
    output->hog_pid       = -1;
    output->bytes_per_frame = (size_t)format->num_channels * sizeof(float);

    AudioObjectPropertyAddress pa = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(AudioDeviceID);
    OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject,
                                             &pa, 0, NULL, &size,
                                             &output->device_id);
    if (st != noErr || output->device_id == kAudioDeviceUnknown) {
        set_error(output, "Failed to get default output device", st);
        return NULL;
    }

    st = set_hog_mode(output->device_id, &output->hog_pid);
    if (st != noErr) {
        set_error(output, "Failed to acquire hog mode — another app may hold it", st);
        return NULL;
    }

    st = set_mixing(output->device_id, 0);
    if (st == noErr) output->mixing_disabled = 1;
    
    if (negotiate_hog_format(output->device_id, format, &output->hw_asbd) != 0) {
        set_error(output,
            "No supported bit-perfect format found for this sample rate. "
            "Check Audio MIDI Setup.app for available device formats.", noErr);
        return NULL;
    }

    st = set_physical_format(output->device_id, &output->hw_asbd);
    if (st != noErr) {
        set_error(output, "Failed to set physical format", st);
        return NULL;
    }

    UInt32 min_frames = get_min_buffer_frame_size(output->device_id);
    set_buffer_frame_size(output->device_id, min_frames);

    UInt32 max_frames = min_frames * 4;
    output->conv_buf_bytes = (size_t)max_frames
                           * output->hw_asbd.mChannelsPerFrame
                           * sizeof(float);
    output->conv_buf = (uint8_t*)malloc(output->conv_buf_bytes);
    if (!output->conv_buf) {
        set_error(output, "Failed to allocate conversion buffer", noErr);
        return NULL;
    }

    size_t ring_bytes = (size_t)format->sample_rate
                      * format->num_channels * sizeof(float) * 2;
    output->ring_buffer = ring_buffer_create(ring_bytes);
    if (!output->ring_buffer) {
        set_error(output, "Failed to allocate ring buffer", noErr);
        return NULL;
    }
    
    st = AudioDeviceCreateIOProcID(output->device_id, hog_io_proc,
                                   output, &output->io_proc_id);
    if (st != noErr) {
        set_error(output, "AudioDeviceCreateIOProcID failed", st);
        return NULL;
    }

    return output;
}

AudioOutput* coreaudio_output_create(const AudioFormat* format,
                                     const CoreAudioConfig* config)
{
    if (!format) return NULL;

    AudioOutput* output = (AudioOutput*)calloc(1, sizeof(AudioOutput));
    if (!output) return NULL;

    memcpy(&output->format, format, sizeof(AudioFormat));
    output->state     = AUDIO_STATE_STOPPED;
    output->device_id = kAudioDeviceUnknown;
    output->hog_pid   = -1;

    CoreAudioMode mode = (config && config->mode == COREAUDIO_MODE_HOG)
                       ? COREAUDIO_MODE_HOG : COREAUDIO_MODE_SHARED;
    output->mode = mode;

    AudioOutput* result = (mode == COREAUDIO_MODE_HOG)
                        ? create_hog(output, format)
                        : create_shared(output, format);

    if (!result) {
        audio_output_close(output);
        return NULL;
    }

    return output;
}

int audio_output_start(AudioOutput* output) {
    if (!output) return -1;
    if (output->state == AUDIO_STATE_PLAYING) return 0;

    OSStatus st;
    if (output->mode == COREAUDIO_MODE_HOG) {
        st = AudioDeviceStart(output->device_id, output->io_proc_id);
        if (st != noErr) { set_error(output, "AudioDeviceStart failed", st); return -1; }
    } else {
        st = AudioOutputUnitStart(output->audio_unit);
        if (st != noErr) { set_error(output, "AudioOutputUnitStart failed", st); return -1; }
    }

    output->state = AUDIO_STATE_PLAYING;
    return 0;
}

int audio_output_stop(AudioOutput* output) {
    if (!output) return -1;
    if (output->state == AUDIO_STATE_STOPPED) return 0;

    if (output->ring_buffer) ring_buffer_cancel(output->ring_buffer);

    if (output->mode == COREAUDIO_MODE_HOG) {
        if (output->io_proc_id)
            AudioDeviceStop(output->device_id, output->io_proc_id);
    } else {
        if (output->audio_unit) {
            AudioOutputUnitStop(output->audio_unit);
            AudioUnitReset(output->audio_unit, kAudioUnitScope_Global, 0);
        }
    }

    output->state = AUDIO_STATE_STOPPED;
    return 0;
}

int audio_output_pause(AudioOutput* output) {
    if (!output) return -1;
    if (output->state != AUDIO_STATE_PLAYING) return -1;

    OSStatus st;
    if (output->mode == COREAUDIO_MODE_HOG) {
        st = AudioDeviceStop(output->device_id, output->io_proc_id);
        if (st != noErr) { set_error(output, "Pause: AudioDeviceStop failed", st); return -1; }
    } else {
        st = AudioOutputUnitStop(output->audio_unit);
        if (st != noErr) { set_error(output, "Pause: AudioOutputUnitStop failed", st); return -1; }
    }

    output->state = AUDIO_STATE_PAUSED;
    return 0;
}

int audio_output_resume(AudioOutput* output) {
    if (!output) return -1;
    if (output->state != AUDIO_STATE_PAUSED) return -1;

    OSStatus st;
    if (output->mode == COREAUDIO_MODE_HOG) {
        st = AudioDeviceStart(output->device_id, output->io_proc_id);
        if (st != noErr) { set_error(output, "Resume: AudioDeviceStart failed", st); return -1; }
    } else {
        st = AudioOutputUnitStart(output->audio_unit);
        if (st != noErr) { set_error(output, "Resume: AudioOutputUnitStart failed", st); return -1; }
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

    if (output->mode == COREAUDIO_MODE_HOG) {
        if (output->io_proc_id) {
            AudioDeviceDestroyIOProcID(output->device_id, output->io_proc_id);
            output->io_proc_id = NULL;
        }

        if (output->mixing_disabled)
            set_mixing(output->device_id, 1);

        if (output->hog_pid != -1)
            release_hog_mode(output->device_id);

        if (output->conv_buf) { free(output->conv_buf); output->conv_buf = NULL; }

    } else {
        if (output->audio_unit) {
            AudioUnitUninitialize(output->audio_unit);
            AudioComponentInstanceDispose(output->audio_unit);
            output->audio_unit = NULL;
        }
    }

    if (output->ring_buffer) {
        ring_buffer_destroy(output->ring_buffer);
        output->ring_buffer = NULL;
    }

    free(output);
}
