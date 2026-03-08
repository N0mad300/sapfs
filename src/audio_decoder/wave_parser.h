#ifndef WAVE_PARSER_H
#define WAVE_PARSER_H

#include <stdint.h>
#include <stddef.h>

typedef struct WaveFile WaveFile;

typedef struct {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    uint16_t valid_bits_per_sample;
    uint32_t data_size;
    uint32_t num_samples;
    uint32_t channel_mask;
    int      is_float;
} WaveFormat;

WaveFile* wave_open(const char* filepath);
int wave_get_format(WaveFile* wave, WaveFormat* format);
size_t wave_read_samples(WaveFile* wave, void* buffer, size_t num_samples);
int wave_seek(WaveFile* wave, uint32_t sample_position);
uint32_t wave_tell(WaveFile* wave);
void wave_close(WaveFile* wave);
const char* wave_get_error(WaveFile* wave);

#endif /* WAVE_PARSER_H */