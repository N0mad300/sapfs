#include "audio_output.h"

/* Platform-specific includes */
#ifdef _WIN32
    #include "wasapi_output.h"
#endif

/**
 * This file implements the platform-independent audio output interface.
 * It delegates to platform-specific implementations based on the OS.
 */

AudioOutput* audio_output_init(const AudioFormat* format) {
    if (!format) {
        return NULL;
    }
    
    /* Validate format parameters */
    if (format->num_channels < 1 || format->num_channels > 8) {
        return NULL;
    }
    
    if (format->sample_rate < 8000 || format->sample_rate > 192000) {
        return NULL;
    }
    
    if (format->bits_per_sample != 8 && format->bits_per_sample != 16 &&
        format->bits_per_sample != 24 && format->bits_per_sample != 32) {
        return NULL;
    }
    
    /* Delegate to platform-specific implementation */
#ifdef _WIN32
    return wasapi_output_create(format);
#elif defined(__linux__)
    /* Future: return alsa_output_create(format); */
    return NULL;
#elif defined(__APPLE__)
    /* Future: return coreaudio_output_create(format); */
    return NULL;
#else
    return NULL;
#endif
}