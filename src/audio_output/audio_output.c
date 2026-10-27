#include "audio_output.h"

/* Platform-specific back-end headers */
#ifdef _WIN32
    #include "windows_output.h"
#elif defined(__APPLE__)
    #include "coreaudio_output.h"
#elif defined(__linux__)
    #include "linux_output.h"
#endif

/**
 * Platform-independent constructor.
 *
 * Validates the requested format then delegates to the appropriate
 * platform back-end:
 *   Windows  -> WASAPI / ASIO  (windows_output.c)
 *   macOS    -> CoreAudio AudioUnit  (coreaudio_output.c)
 *   Linux    -> ALSA / PipeWire / JACK / PulseAudio (linux_output.c)
 */

AudioOutput* audio_output_init(const AudioFormat* format, AudioOutputConfig config) {
    if (!format) return NULL;

    /* ---- Validate common parameters ---- */
    if (format->num_channels < 1 || format->num_channels > 8)
        return NULL;

    if (format->sample_rate < 8000 || format->sample_rate > 192000)
        return NULL;

    if (format->bits_per_sample != 8  && format->bits_per_sample != 16 &&
        format->bits_per_sample != 24 && format->bits_per_sample != 32)
        return NULL;

    /* ---- Dispatch to platform back-end ---- */
#ifdef _WIN32
    return windows_output_create(format, (const WindowsAudioConfig*)config);

#elif defined(__APPLE__)
    (void)config;
    return coreaudio_output_create(format, (const CoreAudioConfig*)config);

#elif defined(__linux__)
    return linux_output_create(format, (const LinuxAudioConfig*)config);

#else
    (void)config;
    return NULL;
#endif
}

#ifndef __linux__
int audio_output_drain(AudioOutput* output) {
    int queued = audio_output_get_queued_frames(output);
    return queued < 0 ? -1 : queued == 0;
}
#endif
