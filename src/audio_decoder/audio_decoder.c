#include "audio_decoder.h"
#include "wave_parser.h"
#include "flac_parser.h"
#include <string.h>
#include <stdlib.h>

/* Cross-platform case-insensitive string compare */
#ifdef _WIN32
    #include <string.h>
    #define strcasecmp _stricmp
#else
    #include <strings.h>  /* For strcasecmp on Unix */
#endif

/* Decoder type enumeration */
typedef enum {
    DECODER_TYPE_WAVE,
    DECODER_TYPE_MP3,
    DECODER_TYPE_FLAC,
    DECODER_TYPE_UNKNOWN
} DecoderType;

/* Generic decoder structure */
struct AudioDecoder {
    DecoderType type;
    void* decoder_instance;     /* Pointer to specific decoder (WaveFile*, FlacFile*, etc.) */
    AudioDecoderFormat format;
    char error_msg[256];
};

/* Helper: Detect decoder type from file extension */
static DecoderType detect_decoder_type(const char* filepath) {
    const char* ext = strrchr(filepath, '.');
    if (!ext) {
        return DECODER_TYPE_UNKNOWN;
    }
    
    ext++; /* Skip the dot */
    
    if (strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "wave") == 0) {
        return DECODER_TYPE_WAVE;
    } else if (strcasecmp(ext, "mp3") == 0) {
        return DECODER_TYPE_MP3;
    } else if (strcasecmp(ext, "flac") == 0) {
        return DECODER_TYPE_FLAC;
    }
    
    return DECODER_TYPE_UNKNOWN;
}

AudioDecoder* audio_decoder_open(const char* filepath) {
    AudioDecoder* decoder = NULL;
    DecoderType type;
    
    if (!filepath) {
        return NULL;
    }
    
    /* Detect decoder type */
    type = detect_decoder_type(filepath);
    if (type == DECODER_TYPE_UNKNOWN) {
        return NULL;
    }
    
    /* Allocate decoder structure */
    decoder = (AudioDecoder*)calloc(1, sizeof(AudioDecoder));
    if (!decoder) {
        return NULL;
    }
    
    decoder->type = type;
    
    /* Open codec-specific decoder */
    switch (type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = wave_open(filepath);
            if (!wave) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to open WAVE file");
                free(decoder);
                return NULL;
            }
            
            /* Get WAVE format and convert to generic format */
            WaveFormat wave_format;
            if (wave_get_format(wave, &wave_format) != 0) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to get WAVE format: %s", wave_get_error(wave));
                wave_close(wave);
                free(decoder);
                return NULL;
            }
            
            /* Fill generic format */
            decoder->format.sample_rate = wave_format.sample_rate;
            decoder->format.num_channels = wave_format.num_channels;
            decoder->format.bits_per_sample = wave_format.bits_per_sample;
            decoder->format.block_align = wave_format.block_align;
            decoder->format.total_samples = wave_format.num_samples;
            strncpy(decoder->format.codec_name, "PCM", sizeof(decoder->format.codec_name) - 1);
            
            decoder->decoder_instance = wave;
            break;
        }

        case DECODER_TYPE_FLAC: {
            FlacFile* flac = flac_open(filepath);
            if (!flac) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to open FLAC file");
                free(decoder);
                return NULL;
            }
            
            /* Get FLAC format and convert to generic format */
            FlacFormat flac_format;
            if (flac_get_format(flac, &flac_format) != 0) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to get FLAC format: %s", flac_get_error(flac));
                flac_close(flac);
                free(decoder);
                return NULL;
            }
            
            /* Fill generic format */
            decoder->format.sample_rate = flac_format.sample_rate;
            decoder->format.num_channels = flac_format.num_channels;
            decoder->format.bits_per_sample = flac_format.bits_per_sample;
            decoder->format.block_align = flac_format.block_align;
            decoder->format.total_samples = (uint32_t)flac_format.total_samples;
            strncpy(decoder->format.codec_name, "FLAC", sizeof(decoder->format.codec_name) - 1);
            
            decoder->decoder_instance = flac;
            break;
        }
        
        case DECODER_TYPE_MP3:
            /* Future: MP3 decoder implementation */
            snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                     "MP3 decoder not yet implemented");
            free(decoder);
            return NULL;
            
        default:
            free(decoder);
            return NULL;
    }
    
    return decoder;
}

int audio_decoder_get_format(AudioDecoder* decoder, AudioDecoderFormat* format) {
    if (!decoder || !format) {
        return -1;
    }
    
    memcpy(format, &decoder->format, sizeof(AudioDecoderFormat));
    return 0;
}

size_t audio_decoder_read_samples(AudioDecoder* decoder, void* buffer, size_t num_samples) {
    if (!decoder || !buffer) {
        return -1;
    }
    
    switch (decoder->type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = (WaveFile*)decoder->decoder_instance;
            return wave_read_samples(wave, buffer, num_samples);
        }

        case DECODER_TYPE_FLAC: {
            FlacFile* flac = (FlacFile*)decoder->decoder_instance;
            return flac_read_samples(flac, buffer, num_samples);
        }
        
        case DECODER_TYPE_MP3:
            /* Future: MP3 read implementation */
            return -1;
            
        default:
            return -1;
    }
}

int audio_decoder_seek(AudioDecoder* decoder, uint32_t sample_position) {
    if (!decoder) {
        return -1;
    }
    
    switch (decoder->type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = (WaveFile*)decoder->decoder_instance;
            return wave_seek(wave, sample_position);
        }

        case DECODER_TYPE_FLAC: {
            FlacFile* flac = (FlacFile*)decoder->decoder_instance;
            return flac_seek(flac, sample_position);
        }
        
        case DECODER_TYPE_MP3:
            /* Future: MP3 seek implementation */
            return -1;
            
        default:
            return -1;
    }
}

uint32_t audio_decoder_tell(AudioDecoder* decoder) {
    if (!decoder) {
        return 0;
    }
    
    switch (decoder->type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = (WaveFile*)decoder->decoder_instance;
            return wave_tell(wave);
        }

        case DECODER_TYPE_FLAC: {
            FlacFile* flac = (FlacFile*)decoder->decoder_instance;
            return (uint32_t)flac_tell(flac);
        }
        
        case DECODER_TYPE_MP3:
            /* Future: MP3 tell implementation */
            return 0;
            
        default:
            return 0;
    }
}

const char* audio_decoder_get_error(AudioDecoder* decoder) {
    if (!decoder) {
        return "Invalid decoder handle";
    }
    
    /* If decoder-specific error exists, return it */
    if (decoder->error_msg[0]) {
        return decoder->error_msg;
    }
    
    /* Otherwise, check codec-specific error */
    switch (decoder->type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = (WaveFile*)decoder->decoder_instance;
            return wave_get_error(wave);
        }

        case DECODER_TYPE_FLAC: {
            FlacFile* flac = (FlacFile*)decoder->decoder_instance;
            return flac_get_error(flac);
        }
        
        default:
            return NULL;
    }
}

void audio_decoder_close(AudioDecoder* decoder) {
    if (!decoder) {
        return;
    }
    
    /* Close codec-specific decoder */
    switch (decoder->type) {
        case DECODER_TYPE_WAVE:
            if (decoder->decoder_instance) {
                wave_close((WaveFile*)decoder->decoder_instance);
            }
            break;

        case DECODER_TYPE_FLAC:
            if (decoder->decoder_instance) {
                flac_close((FlacFile*)decoder->decoder_instance);
            }
            break;
            
        case DECODER_TYPE_MP3:
            /* Future: MP3 cleanup */
            break;
            
        default:
            break;
    }
    
    free(decoder);
}