#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

/**
 * Utility functions for audio processing and debugging.
 */

/**
 * Convert 8-bit unsigned PCM to 16-bit signed PCM.
 * 8-bit samples are unsigned (0-255, center at 128).
 * 16-bit samples are signed (-32768 to 32767, center at 0).
 * 
 * @param input 8-bit input samples
 * @param output 16-bit output samples
 * @param num_samples Number of samples to convert
 */
void utils_convert_8bit_to_16bit(const uint8_t* input, int16_t* output, size_t num_samples);

/**
 * Calculate the RMS (Root Mean Square) volume of audio samples.
 * This gives a measure of the "loudness" of the audio.
 * 
 * @param samples 16-bit sample data
 * @param num_samples Number of samples
 * @param num_channels Number of channels
 * @return RMS value (0.0 to 1.0)
 */
double utils_calculate_rms_16bit(const int16_t* samples, size_t num_samples, int num_channels);

/**
 * Normalize 16-bit audio samples to a target RMS level.
 * This adjusts the volume to a consistent level.
 * 
 * @param samples Sample data (modified in place)
 * @param num_samples Number of samples
 * @param num_channels Number of channels
 * @param target_rms Target RMS level (0.0 to 1.0)
 */
void utils_normalize_16bit(int16_t* samples, size_t num_samples, int num_channels, double target_rms);

/**
 * Format time in seconds as MM:SS string.
 * 
 * @param seconds Time in seconds
 * @param buffer Output buffer (should be at least 16 bytes)
 * @param buffer_size Size of output buffer
 */
void utils_format_time(double seconds, char* buffer, size_t buffer_size);

/**
 * Format byte size as human-readable string (KB, MB, GB).
 * 
 * @param bytes Size in bytes
 * @param buffer Output buffer (should be at least 32 bytes)
 * @param buffer_size Size of output buffer
 */
void utils_format_size(uint64_t bytes, char* buffer, size_t buffer_size);

#endif /* UTILS_H */