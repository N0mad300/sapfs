#ifndef WINDOWS_OUTPUT_H
#define WINDOWS_OUTPUT_H

#include "audio_output.h"

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WINDOWS_AUDIO_BACKEND_WASAPI = 0,
    WINDOWS_AUDIO_BACKEND_ASIO   = 1
} WindowsAudioBackend;

typedef struct {
    WindowsAudioBackend backend;
    const void* backend_config;
} WindowsAudioConfig;

AudioOutput* windows_output_create(const AudioFormat* format, const WindowsAudioConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* WINDOWS_OUTPUT_H */
