#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>

typedef struct AudioOutput AudioOutput;

typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint16_t valid_bits_per_sample;
    uint16_t block_align;
    uint32_t channel_mask;
    int      is_float;
} AudioFormat;

/* Playback state */
typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} AudioState;

/* Platform-specific configuration (forward declaration) */
typedef void* AudioOutputConfig;

AudioOutput* audio_output_init(const AudioFormat* format, AudioOutputConfig config);
int audio_output_start(AudioOutput* output);
int audio_output_stop(AudioOutput* output);
int audio_output_pause(AudioOutput* output);
int audio_output_resume(AudioOutput* output);
int audio_output_write(AudioOutput* output, const void* data, size_t num_samples);
AudioState audio_output_get_state(AudioOutput* output);
int audio_output_get_available_frames(AudioOutput* output);
const char* audio_output_get_error(AudioOutput* output);
void audio_output_close(AudioOutput* output);

#endif /* AUDIO_OUTPUT_H */
