#ifndef AUDIO_CONVERTER_H
#define AUDIO_CONVERTER_H

#include <stdint.h>
#include <stddef.h>

/* --- IMPORTERS (Raw -> Float) --- */

/* 8-bit Unsigned (0 to 255) -> Float (-1.0 to 1.0) */
void pcm8_to_float(const uint8_t* src, float* dst, size_t num_samples);

/* 16-bit Signed -> Float */
void pcm16_to_float(const int16_t* src, float* dst, size_t num_samples);

/* 24-bit Packed (3 bytes) -> Float */
void pcm24_packed_to_float(const uint8_t* src, float* dst, size_t num_samples);

/* 24-bit Padded (4 bytes) -> Float */
void pcm24_padded_to_float(const int32_t* src, float* dst, size_t num_samples);

/* 32-bit Signed -> Float */
void pcm32_to_float(const int32_t* src, float* dst, size_t num_samples);

/* 32-bit Float -> Float */
void pcm32_float_copy(const float* src, float* dst, size_t num_samples);

/* --- EXPORTERS (Float -> Raw) --- */

/* Float -> 16-bit Signed (with optional TPDF dither) */
void float_to_pcm16(const float* src, int16_t* dst, size_t num_samples, int dither);

/* Float -> 24-bit Packed (3 bytes) */
void float_to_pcm24(const float* src, uint8_t* dst, size_t num_samples);

/* Float -> 32-bit Signed */
void float_to_pcm32(const float* src, int32_t* dst, size_t num_samples);

#endif