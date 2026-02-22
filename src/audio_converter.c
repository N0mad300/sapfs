#include "audio_converter.h"
#include <stdlib.h>
#include <math.h>

/* Helper for TPDF Dithering */
static inline float rand_float(void) {
    return (float)rand() / (float)RAND_MAX;
}

/* ---------------- IMPORTERS (Source -> Float) ---------------- */

void pcm8_to_float(const uint8_t* src, float* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        dst[i] = (src[i] - 128) / 128.0f;
    }
}

void pcm16_to_float(const int16_t* src, float* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        dst[i] = src[i] / 32768.0f;
    }
}

void pcm24_packed_to_float(const uint8_t* src, float* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {

        int32_t val = (src[3*i+0]) | (src[3*i+1] << 8) | (src[3*i+2] << 16);
        
        if (val & 0x800000) {
            val |= 0xFF000000;
        }
        
        dst[i] = val / 8388608.0f;
    }
}

void pcm24_padded_to_float(const int32_t* src, float* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        dst[i] = (float)src[i] / 8388608.0f;
        if (dst[i] > 1.0f) dst[i] = 1.0f;
        if (dst[i] < -1.0f) dst[i] = -1.0f;
    }
}

void pcm32_to_float(const int32_t* src, float* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        dst[i] = src[i] / 2147483648.0f;
    }
}

void pcm32_float_copy(const float* src, float* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        dst[i] = src[i];
    }
}

/* ---------------- EXPORTERS (Float -> Target) ---------------- */

void float_to_pcm16(const float* src, int16_t* dst, size_t num_samples, int dither) {
    for (size_t i = 0; i < num_samples; i++) {
        float sample = src[i] * 32768.0f;
        
        if (dither) {
            /* Triangular Probability Density Function (TPDF) Dither */
            sample += (rand_float() - rand_float());
        }

        if (sample >= 32767.0f) dst[i] = 32767;
        else if (sample <= -32768.0f) dst[i] = -32768;
        else dst[i] = (int16_t)sample;
    }
}

void float_to_pcm24(const float* src, uint8_t* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        float sample = src[i] * 8388608.0f;
        int32_t val;

        if (sample >= 8388607.0f) val = 8388607;
        else if (sample <= -8388608.0f) val = -8388608;
        else val = (int32_t)sample;

        dst[3*i+0] = (uint8_t)(val & 0xFF);
        dst[3*i+1] = (uint8_t)((val >> 8) & 0xFF);
        dst[3*i+2] = (uint8_t)((val >> 16) & 0xFF);
    }
}

void float_to_pcm32(const float* src, int32_t* dst, size_t num_samples) {
    for (size_t i = 0; i < num_samples; i++) {
        float sample = src[i] * 2147483648.0f;
        
        if (sample >= 2147483647.0f) dst[i] = 2147483647;
        else if (sample <= -2147483648.0f) dst[i] = (int32_t)(-2147483648LL);
        else dst[i] = (int32_t)sample;
    }
}