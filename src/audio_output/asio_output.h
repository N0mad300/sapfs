#ifndef ASIO_OUTPUT_H
#define ASIO_OUTPUT_H

#include "audio_output.h"

#ifdef _WIN32

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AsioOutput AsioOutput;

typedef struct {
    const char* driver_name;          /* NULL selects the first registered ASIO driver */
    long output_channel_offset;       /* First ASIO output channel to use, zero-based */
    long requested_buffer_size;       /* 0 selects the driver's preferred size */
} AsioConfig;

typedef void (*AsioDriverListCallback)(const char* driver_name, void* user_data);

AsioOutput* asio_output_create(const AudioFormat* format, const AsioConfig* config);
int asio_output_start(AsioOutput* output);
int asio_output_stop(AsioOutput* output);
int asio_output_pause(AsioOutput* output);
int asio_output_resume(AsioOutput* output);
int asio_output_write(AsioOutput* output, const void* data, size_t num_samples);
AudioState asio_output_get_state(AsioOutput* output);
int asio_output_get_available_frames(AsioOutput* output);
const char* asio_output_get_error(AsioOutput* output);
const char* asio_output_get_last_error(void);
void asio_output_close(AsioOutput* output);
int asio_output_list_drivers(AsioDriverListCallback callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* _WIN32 */

#endif /* ASIO_OUTPUT_H */
