#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <stdint.h>
#include <stddef.h>

/**
 * Audio decoder abstraction layer
 * This provides a codec-independent interface for decoding audio files.
 * Codec-specific implementations (WAVE, MP3, FLAC, etc.) implement these functions.
 */

/* Opaque handle to audio decoder */
typedef struct AudioDecoder AudioDecoder;

/* Audio format specification */
typedef struct {
    uint32_t sample_rate;       /* Sample rate in Hz (e.g., 44100) */
    uint16_t num_channels;      /* Number of channels (1=mono, 2=stereo) */
    uint16_t bits_per_sample;   /* Bits per sample (8, 16, 24, 32) */
    uint16_t block_align;       /* Bytes per sample frame */
    uint32_t total_samples;     /* Total number of sample frames */
    char codec_name[32];        /* Codec name (e.g., "PCM", "MP3") */
} AudioDecoderFormat;

/**
 * Open an audio file for decoding.
 * The decoder type is automatically detected from file extension/content.
 * 
 * @param filepath Path to the audio file
 * @return Pointer to AudioDecoder handle, or NULL on failure
 */
AudioDecoder* audio_decoder_open(const char* filepath);

/**
 * Get format information from the decoder.
 * 
 * @param decoder Pointer to AudioDecoder handle
 * @param format Pointer to AudioDecoderFormat structure to fill
 * @return 0 on success, -1 on error
 */
int audio_decoder_get_format(AudioDecoder* decoder, AudioDecoderFormat* format);

/**
 * Read decoded PCM samples from the audio file.
 * All decoders output PCM samples regardless of source format.
 * 
 * @param decoder Pointer to AudioDecoder handle
 * @param buffer Buffer to store samples
 * @param num_samples Number of sample frames to read
 * @return Number of sample frames actually read, 0 at EOF, -1 on error
 */
size_t audio_decoder_read_samples(AudioDecoder* decoder, void* buffer, size_t num_samples);

/**
 * Seek to a specific sample position in the file.
 * 
 * @param decoder Pointer to AudioDecoder handle
 * @param sample_position Sample frame position to seek to
 * @return 0 on success, -1 on error
 */
int audio_decoder_seek(AudioDecoder* decoder, uint32_t sample_position);

/**
 * Get current sample position.
 * 
 * @param decoder Pointer to AudioDecoder handle
 * @return Current sample frame position
 */
uint32_t audio_decoder_tell(AudioDecoder* decoder);

/**
 * Get the last error message.
 * 
 * @param decoder Pointer to AudioDecoder handle
 * @return Error message string, or NULL if no error
 */
const char* audio_decoder_get_error(AudioDecoder* decoder);

/**
 * Close the decoder and free resources.
 * 
 * @param decoder Pointer to AudioDecoder handle
 */
void audio_decoder_close(AudioDecoder* decoder);

#endif /* AUDIO_DECODER_H */