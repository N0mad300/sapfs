#ifndef FLAC_PARSER_H
#define FLAC_PARSER_H

#include <stdint.h>
#include <stddef.h>

typedef struct FlacFile FlacFile;

typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t valid_bits_per_sample;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint32_t data_size;
    uint32_t channel_mask;
    uint64_t total_samples;
} FlacFormat;

FlacFile* flac_open(const char* filepath);
int flac_get_format(FlacFile* flac, FlacFormat* format);
size_t flac_read_samples(FlacFile* flac, void* buffer, size_t num_samples);
int flac_seek(FlacFile* flac, uint64_t sample_position);
uint64_t flac_tell(FlacFile* flac);
void flac_close(FlacFile* flac);
const char* flac_get_error(FlacFile* flac);

#endif /* FLAC_PARSER_H */