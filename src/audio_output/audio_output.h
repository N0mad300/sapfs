#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

#include <stdint.h>
#include <stddef.h>

/**
 * Audio output abstraction layer
 * This provides a platform-independent interface for audio playback.
 * Platform-specific implementations (WASAPI, ALSA, CoreAudio) implement these functions.
 */

/* Opaque handle to audio output device */
typedef struct AudioOutput AudioOutput;

/* Audio format specification */
typedef struct {
    uint32_t sample_rate;       /* Sample rate in Hz (e.g., 44100) */
    uint16_t num_channels;      /* Number of channels (1=mono, 2=stereo) */
    uint16_t bits_per_sample;   /* Bits per sample (8, 16, 24, 32) */
    uint16_t block_align;       /* Bytes per sample frame */
} AudioFormat;

/* Playback state */
typedef enum {
    AUDIO_STATE_STOPPED = 0,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED
} AudioState;

/**
 * Initialize audio output with the specified format.
 * 
 * @param format Audio format specification
 * @return Pointer to AudioOutput handle, or NULL on failure
 */
AudioOutput* audio_output_init(const AudioFormat* format);

/**
 * Start audio playback.
 * 
 * @param output Pointer to AudioOutput handle
 * @return 0 on success, -1 on error
 */
int audio_output_start(AudioOutput* output);

/**
 * Stop audio playback.
 * 
 * @param output Pointer to AudioOutput handle
 * @return 0 on success, -1 on error
 */
int audio_output_stop(AudioOutput* output);

/**
 * Pause audio playback.
 * 
 * @param output Pointer to AudioOutput handle
 * @return 0 on success, -1 on error
 */
int audio_output_pause(AudioOutput* output);

/**
 * Resume audio playback from paused state.
 * 
 * @param output Pointer to AudioOutput handle
 * @return 0 on success, -1 on error
 */
int audio_output_resume(AudioOutput* output);

/**
 * Write audio samples to the output buffer.
 * This function blocks if the buffer is full.
 * 
 * @param output Pointer to AudioOutput handle
 * @param data Pointer to sample data
 * @param num_samples Number of sample frames to write
 * @return Number of frames actually written, or -1 on error
 */
int audio_output_write(AudioOutput* output, const void* data, size_t num_samples);

/**
 * Get the current playback state.
 * 
 * @param output Pointer to AudioOutput handle
 * @return Current AudioState
 */
AudioState audio_output_get_state(AudioOutput* output);

/**
 * Get the number of sample frames that can be written without blocking.
 * 
 * @param output Pointer to AudioOutput handle
 * @return Number of available frames, or -1 on error
 */
int audio_output_get_available_frames(AudioOutput* output);

/**
 * Get the last error message.
 * 
 * @param output Pointer to AudioOutput handle
 * @return Error message string, or NULL if no error
 */
const char* audio_output_get_error(AudioOutput* output);

/**
 * Clean up and close audio output.
 * 
 * @param output Pointer to AudioOutput handle
 */
void audio_output_close(AudioOutput* output);

#endif /* AUDIO_OUTPUT_H */