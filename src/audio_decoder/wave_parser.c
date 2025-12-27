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
    uint32_t subchunk_size; /* Size of format data */
    uint16_t audio_format;  /* Audio format (1 = PCM, 0xFFFE = EXTENSIBLE) */
    uint16_t num_channels;  /* Number of channels */
    uint32_t sample_rate;   /* Sample rate */
    uint32_t byte_rate;     /* Byte rate */
    uint16_t block_align;   /* Block alignment */
    uint16_t bits_per_sample; /* Bits per sample */
} FormatChunk;

/* Extended format chunk (WAVE_FORMAT_EXTENSIBLE) */
typedef struct {
    uint16_t cb_size;           /* Extension size (22 for WAVEFORMATEXTENSIBLE) */
    uint16_t valid_bits;        /* Valid bits per sample */
    uint32_t channel_mask;      /* Speaker position mask */
    uint8_t sub_format[16];     /* GUID for actual format (first 2 bytes = format code) */
} FormatChunkExtension;

/* Generic chunk header (used for skipping unknown chunks) */
typedef struct {
    char chunk_id[4];
    uint32_t chunk_size;
} ChunkHeader;

/* Internal WaveFile structure */
struct WaveFile {
    FILE* fp;                   /* File pointer */
    WaveFormat format;          /* Format information */
    long data_start_pos;        /* Position where PCM data starts */
    uint32_t current_sample;    /* Current sample position */
    char error_msg[256];        /* Last error message */
    int is_valid;               /* Whether file is valid */
};

/* Helper function to read a chunk header (ID + size) */
static int read_chunk_header(FILE* fp, ChunkHeader* header) {
    if (fread(header, sizeof(ChunkHeader), 1, fp) != 1) {
        return -1;
    }
    return 0;
}

/* Helper function to skip a chunk */
static int skip_chunk(FILE* fp, uint32_t chunk_size) {
    /* Chunks are word-aligned, so if size is odd, skip one extra byte */
    uint32_t skip_size = (chunk_size + 1) & ~1;
    return fseek(fp, skip_size, SEEK_CUR);
}

/* Find and parse the fmt chunk */
static int parse_fmt_chunk(WaveFile* wave, uint32_t chunk_size) {
    FormatChunk fmt;
    
    /* Read fmt chunk header */
    if (fread(&fmt.audio_format, sizeof(uint16_t), 1, wave->fp) != 1 ||
        fread(&fmt.num_channels, sizeof(uint16_t), 1, wave->fp) != 1 ||
        fread(&fmt.sample_rate, sizeof(uint32_t), 1, wave->fp) != 1 ||
        fread(&fmt.byte_rate, sizeof(uint32_t), 1, wave->fp) != 1 ||
        fread(&fmt.block_align, sizeof(uint16_t), 1, wave->fp) != 1 ||
        fread(&fmt.bits_per_sample, sizeof(uint16_t), 1, wave->fp) != 1) {
        printf("Failed to read format chunk data");
        return -1;
    }
    
    /* Handle WAVE_FORMAT_EXTENSIBLE (0xFFFE) */
    if (fmt.audio_format == 0xFFFE) {
        /* This is an extensible format - need to read extension */
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
        
        /* Extract the actual format from SubFormat GUID (first 2 bytes) */
        uint16_t actual_format = ext.sub_format[0] | (ext.sub_format[1] << 8);
        
        /* Check if it's PCM (format code 1) */
        if (actual_format != 1) {
            printf("Unsupported EXTENSIBLE sub-format: %u (only PCM is supported)\n", actual_format);
            return -1;
        }
        
        /* It's PCM in extensible wrapper - treat as standard PCM */
        if (ext.valid_bits > 0 && ext.valid_bits <= fmt.bits_per_sample) {
        }
    }
    /* Standard PCM format */
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
    
    /* For 24-bit, also accept 32-bit container (4 bytes per sample) */
    uint16_t expected_block_align_extended = 0;
    if (fmt.bits_per_sample == 24) {
        expected_block_align_extended = fmt.num_channels * 4;
    }
    
    /* Check if block_align matches either standard or extended format */
    int block_align_valid = (fmt.block_align == expected_block_align_standard);
    if (fmt.bits_per_sample == 24) {
        block_align_valid = block_align_valid || (fmt.block_align == expected_block_align_extended);
    }
    
    if (!block_align_valid) {
        printf("Invalid block align: got %u, expected %u%s", fmt.block_align, expected_block_align_standard, (fmt.bits_per_sample == 24) ? " or 8 (24-in-32)" : "");
        return -1;
    }
    
    /* Validate byte_rate based on actual block_align */
    uint32_t expected_byte_rate = fmt.sample_rate * fmt.block_align;
    
    if (fmt.byte_rate != expected_byte_rate) {
        printf("Invalid byte rate (expected %u, got %u)", expected_byte_rate, fmt.byte_rate);
        return -1;
    }
    
    /* Store format information */
    wave->format.audio_format = 1; /* Treat extensible PCM as standard PCM */
    wave->format.num_channels = fmt.num_channels;
    wave->format.sample_rate = fmt.sample_rate;
    wave->format.byte_rate = fmt.byte_rate;
    wave->format.block_align = fmt.block_align; /* Use actual, not calculated */
    wave->format.bits_per_sample = fmt.bits_per_sample;
    
    return 0;
}

WaveFile* wave_open(const char* filepath) {
    WaveFile* wave = NULL;
    RiffHeader riff;
    int found_fmt = 0;
    int found_data = 0;
    
    /* Allocate WaveFile structure */
    wave = (WaveFile*)calloc(1, sizeof(WaveFile));
    if (!wave) {
        return NULL;
    }
    
    /* Open file */
    wave->fp = fopen(filepath, "rb");
    if (!wave->fp) {
        printf("Failed to open file: %s", filepath);
        free(wave);
        return NULL;
    }
    
    /* Read RIFF header */
    if (fread(&riff, sizeof(RiffHeader), 1, wave->fp) != 1) {
        printf("Failed to read RIFF header");
        fclose(wave->fp);
        free(wave);
        return NULL;
    }
    
    /* Validate RIFF header */
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
    
    /* Parse chunks until we find fmt and data */
    while (!found_fmt || !found_data) {
        ChunkHeader header;
        
        /* Read chunk header */
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
        
        /* Handle fmt chunk */
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
            
            /* Skip any extra format bytes we didn't read */
            long current_pos = ftell(wave->fp);
            long expected_pos = current_pos - 16;

            if (header.chunk_size > 16) {
                /* For extensible format, we read 40 bytes total (16 + 24) */
                /* For standard, we only read 16 bytes */
                long bytes_read = 16;
                if (header.chunk_size >= 40) {
                    bytes_read = 40;
                }
                
                if (header.chunk_size > bytes_read) {
                    skip_chunk(wave->fp, header.chunk_size - bytes_read);
                }
            }
            
            found_fmt = 1;
        }
        /* Handle data chunk */
        else if (memcmp(header.chunk_id, "data", 4) == 0) {
            wave->format.data_size = header.chunk_size;
            wave->data_start_pos = ftell(wave->fp);
            
            /* Calculate number of samples */
            if (wave->format.block_align > 0) {
                wave->format.num_samples = header.chunk_size / wave->format.block_align;
            }
            
            found_data = 1;
        }
        /* Skip unknown chunks */
        else {
            if (skip_chunk(wave->fp, header.chunk_size) != 0) {
                printf("Failed to skip chunk");
                fclose(wave->fp);
                free(wave);
                return NULL;
            }
        }
    }
    
    /* Seek to start of data */
    fseek(wave->fp, wave->data_start_pos, SEEK_SET);
    wave->current_sample = 0;
    wave->is_valid = 1;
    
    return wave;
}

int wave_get_format(WaveFile* wave, WaveFormat* format) {
    if (!wave || !format) {
        return -1;
    }
    
    if (!wave->is_valid) {
        return -1;
    }
    
    memcpy(format, &wave->format, sizeof(WaveFormat));
    return 0;
}

size_t wave_read_samples(WaveFile* wave, void* buffer, size_t num_samples) {
    if (!wave || !buffer) {
        return -1;
    }
    
    if (!wave->is_valid) {
        return -1;
    }
    
    /* Calculate how many samples we can actually read */
    size_t samples_remaining = wave->format.num_samples - wave->current_sample;
    size_t samples_to_read = num_samples;
    
    if (samples_to_read > samples_remaining) {
        samples_to_read = samples_remaining;
    }
    
    if (samples_to_read == 0) {
        return 0; /* End of file */
    }
    
    /* Read the samples */
    size_t samples_read = fread(buffer, wave->format.block_align, 
                                samples_to_read, wave->fp);
    
    wave->current_sample += samples_read;
    
    return samples_read;
}

int wave_seek(WaveFile* wave, uint32_t sample_position) {
    if (!wave) {
        return -1;
    }
    
    if (!wave->is_valid) {
        return -1;
    }
    
    /* Check bounds */
    if (sample_position > wave->format.num_samples) {
        printf("Seek position out of bounds\n");
        return -1;
    }
    
    /* Calculate byte offset */
    long offset = wave->data_start_pos + 
                  (long)(sample_position * wave->format.block_align);
    
    /* Seek to position */
    if (fseek(wave->fp, offset, SEEK_SET) != 0) {
        printf("Failed to seek\n");
        return -1;
    }
    
    wave->current_sample = sample_position;
    return 0;
}

uint32_t wave_tell(WaveFile* wave) {
    if (!wave || !wave->is_valid) {
        return 0;
    }
    return wave->current_sample;
}

void wave_close(WaveFile* wave) {
    if (wave) {
        if (wave->fp) {
            fclose(wave->fp);
        }
        free(wave);
    }
}

const char* wave_get_error(WaveFile* wave) {
    if (!wave) {
        return "Invalid wave file handle";
    }
    return wave->error_msg[0] ? wave->error_msg : NULL;
}