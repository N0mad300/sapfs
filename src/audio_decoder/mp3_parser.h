#ifndef MP3_PARSER_H
#define MP3_PARSER_H

#include <stdint.h>
#include <stddef.h>

typedef struct Mp3File Mp3File;

typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;
    uint16_t block_align;
    uint64_t total_samples;
} Mp3Format;

Mp3File* mp3_open(const char* filepath);
void mp3_close(Mp3File* mp3);
int mp3_get_format(Mp3File* mp3, Mp3Format* format);
size_t mp3_read_samples(Mp3File* mp3, void* buffer, size_t num_frames);
int mp3_seek(Mp3File* mp3, uint64_t sample_position);
uint64_t mp3_tell(Mp3File* mp3);
const char* mp3_get_error(Mp3File* mp3);

#endif /* MP3_PARSER_H */