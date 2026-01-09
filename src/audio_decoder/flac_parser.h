#ifndef FLAC_PARSER_H
#define FLAC_PARSER_H

#include <stdint.h>
#include <stddef.h>

/**
 * FLAC decoder module
 * This provides a simple interface to decode FLAC files using libFLAC.
 * Integrates seamlessly with the audio_decoder abstraction layer.
 */

/* Opaque handle to a FLAC file */
typedef struct FlacFile FlacFile;

/* Format information extracted from FLAC file */
typedef struct {
    uint32_t sample_rate;       /* Sample rate in Hz */
    uint16_t num_channels;      /* Number of channels */
    uint16_t bits_per_sample;   /* Bits per sample */
    uint16_t block_align;       /* Bytes per sample frame */
    uint32_t data_size;         /* Size of decoded PCM data in bytes (estimate) */
    uint64_t total_samples;     /* Total number of sample frames */
} FlacFormat;

/**
 * Opens a FLAC file for reading.
 * 
 * @param filepath Path to the FLAC file
 * @return Pointer to FlacFile handle, or NULL on error
 */
FlacFile* flac_open(const char* filepath);

/**
 * Retrieves format information from the FLAC file.
 * 
 * @param flac Pointer to FlacFile handle
 * @param format Pointer to FlacFormat structure to fill
 * @return 0 on success, -1 on error
 */
int flac_get_format(FlacFile* flac, FlacFormat* format);

/**
 * Reads decoded PCM samples from the FLAC file.
 * Output is always 16-bit signed PCM, regardless of source bit depth.
 * 
 * @param flac Pointer to FlacFile handle
 * @param buffer Buffer to store samples (interleaved, 16-bit signed)
 * @param num_samples Number of sample frames to read
 * @return Number of sample frames actually read, 0 at EOF, -1 on error
 */
size_t flac_read_samples(FlacFile* flac, void* buffer, size_t num_samples);

/**
 * Seeks to a specific sample position in the file.
 * 
 * @param flac Pointer to FlacFile handle
 * @param sample_position Sample frame position to seek to
 * @return 0 on success, -1 on error
 */
int flac_seek(FlacFile* flac, uint64_t sample_position);

/**
 * Returns current sample position.
 * 
 * @param flac Pointer to FlacFile handle
 * @return Current sample frame position
 */
uint64_t flac_tell(FlacFile* flac);

/**
 * Closes the FLAC file and frees resources.
 * 
 * @param flac Pointer to FlacFile handle
 */
void flac_close(FlacFile* flac);

/**
 * Returns the last error message.
 * 
 * @param flac Pointer to FlacFile handle
 * @return Error message string, or NULL if no error
 */
const char* flac_get_error(FlacFile* flac);

#endif /* FLAC_PARSER_H */