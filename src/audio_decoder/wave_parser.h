#ifndef WAVE_PARSER_H
#define WAVE_PARSER_H

#include <stdint.h>
#include <stddef.h>

/* Opaque handle to a WAVE file */
typedef struct WaveFile WaveFile;

/* Format information extracted from WAVE file */
typedef struct {
    uint16_t audio_format;          /* Audio format (1 = PCM) */
    uint16_t num_channels;          /* Number of channels (1 = mono, 2 = stereo) */
    uint32_t sample_rate;           /* Sample rate in Hz (e.g., 44100) */
    uint32_t byte_rate;             /* Bytes per second */
    uint16_t block_align;           /* Bytes per sample frame */
    uint16_t bits_per_sample;       /* Bits per sample (8, 16, 24, 32) */
    uint16_t valid_bits_per_sample; /* Actual audio bits */
    uint32_t data_size;             /* Size of PCM data in bytes */
    uint32_t num_samples;           /* Total number of sample frames */
    uint32_t channel_mask;          /* Speaker position mask */
} WaveFormat;

/**
 * Opens a WAVE file for reading.
 * 
 * @param filepath Path to the WAVE file
 * @return Pointer to WaveFile handle, or NULL on error
 */
WaveFile* wave_open(const char* filepath);

/**
 * Retrieves format information from the WAVE file.
 * 
 * @param wave Pointer to WaveFile handle
 * @param format Pointer to WaveFormat structure to fill
 * @return 0 on success, -1 on error
 */
int wave_get_format(WaveFile* wave, WaveFormat* format);

/**
 * Reads PCM samples from the WAVE file.
 * 
 * @param wave Pointer to WaveFile handle
 * @param buffer Buffer to store samples
 * @param num_samples Number of sample frames to read
 * @return Number of sample frames actually read, 0 at EOF, -1 on error
 */
size_t wave_read_samples(WaveFile* wave, void* buffer, size_t num_samples);

/**
 * Seeks to a specific sample position in the file.
 * 
 * @param wave Pointer to WaveFile handle
 * @param sample_position Sample frame position to seek to
 * @return 0 on success, -1 on error
 */
int wave_seek(WaveFile* wave, uint32_t sample_position);

/**
 * Returns current sample position.
 * 
 * @param wave Pointer to WaveFile handle
 * @return Current sample frame position
 */
uint32_t wave_tell(WaveFile* wave);

/**
 * Closes the WAVE file and frees resources.
 * 
 * @param wave Pointer to WaveFile handle
 */
void wave_close(WaveFile* wave);

/**
 * Returns the last error message.
 * 
 * @param wave Pointer to WaveFile handle
 * @return Error message string, or NULL if no error
 */
const char* wave_get_error(WaveFile* wave);

#endif /* WAVE_PARSER_H */