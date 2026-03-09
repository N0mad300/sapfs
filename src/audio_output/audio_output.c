#include "audio_output.h"

/* Platform-specific back-end headers */
#ifdef _WIN32
    #include "wasapi_output.h"
#elif defined(__APPLE__)
    #include "coreaudio_output.h"
#endif

/**
 * Platform-independent constructor.
 *
 * Validates the requested format then delegates to the appropriate
 * platform back-end:
 *   Windows  → WASAPI  (wasapi_output.c)
 *   macOS    → CoreAudio AudioUnit  (coreaudio_output.c)
 *   Linux    → stub (ALSA / PipeWire planned)
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
    return wasapi_output_create(format, (const WasapiConfig*)config);

#elif defined(__APPLE__)
    (void)config;
    return coreaudio_output_create(format, (const CoreAudioConfig*)config);

#elif defined(__linux__)
    (void)config;
    /* Future: return alsa_output_create(format); */
    return NULL;

#else
    (void)config;
    return NULL;
#endif
}
