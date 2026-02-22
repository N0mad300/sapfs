#include "wave_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* RIFF chunk header structure */
typedef struct {
    char chunk_id[4];       /* "RIFF" */
    uint32_t chunk_size;    /* File size - 8 bytes */
    char format[4];         /* "WAVE" */
} RiffHeader;

/* Format chunk structure */
typedef struct {
    char subchunk_id[4];    /* "fmt " */
    uint32_t subchunk_size;
    uint16_t audio_format;  /* Audio format (1 = PCM, 0xFFFE = EXTENSIBLE) */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} FormatChunk;

/* Extended format chunk (WAVE_FORMAT_EXTENSIBLE) */
typedef struct {
    uint16_t cb_size;
    uint16_t valid_bits;
    uint32_t channel_mask;
    uint8_t sub_format[16];
} FormatChunkExtension;

/* Generic chunk header */
typedef struct {
    char chunk_id[4];
    uint32_t chunk_size;
} ChunkHeader;

/* Internal WaveFile structure */
struct WaveFile {
    FILE* fp;
    WaveFormat format;
    long data_start_pos;
    uint32_t current_sample;
    char error_msg[256];
    int is_valid;
    int must_expand_24_to_32; 
    uint16_t file_block_align;
};

/* Helper function to read a chunk header (ID + size) */
static int read_chunk_header(FILE* fp, ChunkHeader* header) {
    return fread(header, sizeof(ChunkHeader), 1, fp) == 1 ? 0 : -1;
}

/* Helper function to skip a chunk */
static int skip_chunk(FILE* fp, uint32_t chunk_size) {
    uint32_t skip_size = (chunk_size + 1) & ~1;
    return fseek(fp, (long)skip_size, SEEK_CUR);
}

/* Find and parse the fmt chunk */
static int parse_fmt_chunk(WaveFile* wave, uint32_t chunk_size) {
    FormatChunk fmt;
    
    /* Read fmt chunk header */
    if (fread(&fmt.audio_format,    sizeof(uint16_t), 1, wave->fp) != 1 ||
        fread(&fmt.num_channels,    sizeof(uint16_t), 1, wave->fp) != 1 ||
        fread(&fmt.sample_rate,     sizeof(uint32_t), 1, wave->fp) != 1 ||
        fread(&fmt.byte_rate,       sizeof(uint32_t), 1, wave->fp) != 1 ||
        fread(&fmt.block_align,     sizeof(uint16_t), 1, wave->fp) != 1 ||
        fread(&fmt.bits_per_sample, sizeof(uint16_t), 1, wave->fp) != 1) {
        printf("Failed to read format chunk data");
        return -1;
    }

    wave->format.channel_mask = 0;
    wave->format.valid_bits_per_sample = fmt.bits_per_sample;
    
    if (fmt.audio_format == 0xFFFE) {
        /* WAVE_FORMAT_EXTENSIBLE */
        if (chunk_size < 40) {
            printf("WAVE_FORMAT_EXTENSIBLE chunk too small\n");
            return -1;
        }
        
        FormatChunkExtension ext;
        if (fread(&ext.cb_size, sizeof(uint16_t), 1, wave->fp) != 1 ||
            fread(&ext.valid_bits, sizeof(uint16_t), 1, wave->fp) != 1 ||
            fread(&ext.channel_mask, sizeof(uint32_t), 1, wave->fp) != 1 ||
            fread(&ext.sub_format, 16, 1, wave->fp) != 1) {
            printf("Failed to read WAVE_FORMAT_EXTENSIBLE data\n");
            return -1;
        }

        wave->format.channel_mask = ext.channel_mask;
        wave->format.valid_bits_per_sample = (ext.valid_bits > 0) ? ext.valid_bits : fmt.bits_per_sample;
        
        uint16_t actual_format = ext.sub_format[0] | (ext.sub_format[1] << 8);
        if (actual_format != 1 && actual_format != 3) {
            printf("Unsupported EXTENSIBLE sub-format: %u (only PCM is supported)\n", actual_format);
            return -1;
        }
    }
    else if (fmt.audio_format != 1) {
        printf("Unsupported audio format: %u (only PCM is supported)\n", fmt.audio_format);
        return -1;
    }
    
    if (fmt.num_channels < 1 || fmt.num_channels > 8) {
        printf("Invalid number of channels: %u\n", fmt.num_channels);
        return -1;
    }
    
    if (fmt.sample_rate < 1000 || fmt.sample_rate > 192000) {
        printf("Invalid sample rate: %u\n", fmt.sample_rate);
        return -1;
    }
    
    if (fmt.bits_per_sample != 8 && fmt.bits_per_sample != 16 && 
        fmt.bits_per_sample != 24 && fmt.bits_per_sample != 32) {
        printf("Unsupported bits per sample: %u\n", fmt.bits_per_sample);
        return -1;
    }
    
    /* Validate derived values */
    uint16_t bytes_per_sample = fmt.bits_per_sample / 8;
    uint16_t expected_block_align_standard = fmt.num_channels * bytes_per_sample;
    uint16_t expected_block_align_extended = (fmt.bits_per_sample == 24) ? fmt.num_channels * 4 : 0;
    
    int block_align_valid = (fmt.block_align == expected_block_align_standard) || 
                            (expected_block_align_extended && fmt.block_align == expected_block_align_extended);
    if (!block_align_valid) {
        printf("Invalid block align: got %u, expected %u%s", fmt.block_align, expected_block_align_standard, (fmt.bits_per_sample == 24) ? " or 8 (24-in-32)" : "");
        return -1;
    }
    
    uint32_t expected_byte_rate = fmt.sample_rate * fmt.block_align;
    if (fmt.byte_rate != expected_byte_rate) {
        printf("Invalid byte rate (expected %u, got %u)", expected_byte_rate, fmt.byte_rate);
        return -1;
    }

    if (fmt.bits_per_sample == 24 && fmt.block_align == expected_block_align_standard) {
        wave->must_expand_24_to_32 = 1;
        wave->file_block_align = fmt.block_align;
        wave->format.bits_per_sample = 32;
        wave->format.block_align = fmt.num_channels * 4;
    } else {
        wave->must_expand_24_to_32 = 0;
        wave->file_block_align = fmt.block_align;
        wave->format.bits_per_sample = fmt.bits_per_sample;
        wave->format.block_align = fmt.block_align;
    }
    
    /* Store format information */
    wave->format.audio_format = 1;
    wave->format.num_channels = fmt.num_channels;
    wave->format.sample_rate = fmt.sample_rate;
    wave->format.byte_rate = fmt.byte_rate;
    
    return 0;
}

WaveFile* wave_open(const char* filepath) {
    WaveFile* wave = (WaveFile*)calloc(1, sizeof(WaveFile));
    if (!wave) return NULL;
    
    wave->fp = fopen(filepath, "rb");
    if (!wave->fp) {
        printf("Failed to open file: %s", filepath);
        free(wave);
        return NULL;
    }
    
    RiffHeader riff;
    if (fread(&riff, sizeof(RiffHeader), 1, wave->fp) != 1) {
        printf("Failed to read RIFF header");
        fclose(wave->fp);
        free(wave);
        return NULL;
    }
    
    if (memcmp(riff.chunk_id, "RIFF", 4) != 0) {
        printf("Not a RIFF file");
        fclose(wave->fp);
        free(wave);
        return NULL;
    }
    
    if (memcmp(riff.format, "WAVE", 4) != 0) {
        printf("Not a WAVE file");
        fclose(wave->fp);
        free(wave);
        return NULL;
    }

    int found_fmt = 0, found_data = 0;
    
    while (!found_fmt || !found_data) {
        ChunkHeader header;
        
        if (read_chunk_header(wave->fp, &header) != 0) {
            if (feof(wave->fp)) {
                printf("Reached end of file without finding required chunks");
            } else {
                printf("Failed to read chunk header");
            }
            fclose(wave->fp);
            free(wave);
            return NULL;
        }
        
        if (memcmp(header.chunk_id, "fmt ", 4) == 0) {
            if (header.chunk_size < 16) {
                printf("Format chunk too small");
                fclose(wave->fp);
                free(wave);
                return NULL;
            }
            
            if (parse_fmt_chunk(wave, header.chunk_size) != 0) {
                fclose(wave->fp);
                free(wave);
                return NULL;
            }

            long bytes_consumed = (header.chunk_size >= 40) ? 40 : 16;
            if ((long)header.chunk_size > bytes_consumed) {
                fseek(wave->fp, (long)(header.chunk_size - bytes_consumed), SEEK_CUR);
            }
            
            found_fmt = 1;
        }
        else if (memcmp(header.chunk_id, "data", 4) == 0) {
            wave->format.data_size = header.chunk_size;
            wave->data_start_pos = ftell(wave->fp);
            
            /* Calculate number of samples */
            if (wave->format.block_align > 0) {
                wave->format.num_samples = header.chunk_size / wave->file_block_align;
            }
            
            found_data = 1;
        }
        else {
            if (skip_chunk(wave->fp, header.chunk_size) != 0) {
                printf("Failed to skip chunk");
                fclose(wave->fp);
                free(wave);
                return NULL;
            }
        }
    }
    
    fseek(wave->fp, wave->data_start_pos, SEEK_SET);
    wave->current_sample = 0;
    wave->is_valid = 1;
    
    return wave;
}

int wave_get_format(WaveFile* wave, WaveFormat* format) {
    if (!wave || !format || !wave->is_valid) return -1;
    memcpy(format, &wave->format, sizeof(WaveFormat));
    return 0;
}

size_t wave_read_samples(WaveFile* wave, void* buffer, size_t num_samples) {
    if (!wave || !buffer || !wave->is_valid) return (size_t)-1;
    
    size_t samples_remaining = wave->format.num_samples - wave->current_sample;
    size_t samples_to_read = (num_samples > samples_remaining) ? samples_remaining : num_samples;
    
    if (samples_to_read == 0) return 0;
    
    if (wave->must_expand_24_to_32) {
        uint16_t file_block_align = wave->file_block_align;

        uint8_t temp_buf[4096];
        size_t max_samples_per_batch = sizeof(temp_buf) / file_block_align;
        size_t total_read = 0;
        int32_t* dst = (int32_t*)buffer;

        while (total_read < samples_to_read) {
            size_t to_read = samples_to_read - total_read;
            if (to_read > max_samples_per_batch) to_read = max_samples_per_batch;

            size_t read_count = fread(temp_buf, file_block_align, to_read, wave->fp);
            if (read_count == 0) break;

            uint8_t* src = temp_buf;
            size_t elements = read_count * wave->format.num_channels;

            for (size_t i = 0; i < elements; ++i) {
                uint32_t b0 = src[0];
                uint32_t b1 = src[1];
                uint32_t b2 = src[2];
                src += 3;

                *dst++ = (int32_t)((b2 << 24) | (b1 << 16) | (b0 << 8));
            }

            total_read += read_count;
        }

        wave->current_sample += total_read;
        return total_read;
    }

    size_t samples_read = fread(buffer, wave->format.block_align, samples_to_read, wave->fp);
    wave->current_sample += samples_read;
    return samples_read;
}

int wave_seek(WaveFile* wave, uint32_t sample_position) {
    if (!wave || !wave->is_valid) return -1;
    
    if (sample_position > wave->format.num_samples) {
        printf("Seek position out of bounds\n");
        return -1;
    }
    
    long offset = wave->data_start_pos + (long)(sample_position * wave->file_block_align);
    if (fseek(wave->fp, offset, SEEK_SET) != 0) {
        printf("Failed to seek\n");
        return -1;
    }
    
    wave->current_sample = sample_position;
    return 0;
}

uint32_t wave_tell(WaveFile* wave) {
    if (!wave || !wave->is_valid) return 0;
    return wave->current_sample;
}

void wave_close(WaveFile* wave) {
    if (wave) {
        if (wave->fp) fclose(wave->fp);
        free(wave);
    }
}

const char* wave_get_error(WaveFile* wave) {
    if (!wave) return "Invalid wave file handle";
    return wave->error_msg[0] ? wave->error_msg : NULL;
}