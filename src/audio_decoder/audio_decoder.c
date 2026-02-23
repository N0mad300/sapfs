#include "audio_decoder.h"
#include "../audio_converter.h"
#include "wave_parser.h"
#include "flac_parser.h"
#include "mp3_parser.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

/* Cross-platform case-insensitive string compare */
#ifdef _WIN32
    #include <string.h>
    #define strcasecmp _stricmp
#else
    #include <strings.h>
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
    DecoderType         type;
    void*               decoder_instance;      /* Pointer to specific decoder (WaveFile*, FlacFile*, etc.) */
    AudioDecoderFormat  format;

    uint16_t            src_bits_per_sample;   /* On-disk bit depth */
    uint16_t            src_block_align;       /* On-disk bytes per frame */
    int                 src_is_float;

    uint8_t*            raw_buffer;
    size_t              raw_buffer_capacity;

    char                error_msg[256];
};

/* Detect decoder type from file extension */
static DecoderType detect_decoder_type(const char* filepath) {
    const char* ext = strrchr(filepath, '.');
    if (!ext) return DECODER_TYPE_UNKNOWN;

    ext++;

    if (strcasecmp(ext, "wav") == 0 || strcasecmp(ext, "wave") == 0) return DECODER_TYPE_WAVE;
    if (strcasecmp(ext, "mp3") == 0) return DECODER_TYPE_MP3;
    if (strcasecmp(ext, "flac") == 0) return DECODER_TYPE_FLAC;

    return DECODER_TYPE_UNKNOWN;
}

static int ensure_raw_buf(AudioDecoder* decoder, size_t frames) {
    if (frames <= decoder->raw_buffer_capacity) return 0;

    size_t bytes = frames * decoder->src_block_align;
    uint8_t* p = (uint8_t*)realloc(decoder->raw_buffer, bytes);
    if (!p) return -1;

    decoder->raw_buffer = p;
    decoder->raw_buffer_capacity = frames;
    return 0;
}

AudioDecoder* audio_decoder_open(const char* filepath) {
    if (!filepath) return NULL;

    DecoderType type = detect_decoder_type(filepath);
    if (type == DECODER_TYPE_UNKNOWN) return NULL;

    AudioDecoder* decoder = (AudioDecoder*)calloc(1, sizeof(AudioDecoder));
    if (!decoder) return NULL;
    
    decoder->type = type;
    
    switch (type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = wave_open(filepath);
            if (!wave) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to open WAVE file");
                free(decoder);
                return NULL;
            }
            
            WaveFormat wave_format;
            if (wave_get_format(wave, &wave_format) != 0) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to get WAVE format: %s", wave_get_error(wave));
                wave_close(wave);
                free(decoder);
                return NULL;
            }
            
            decoder->src_bits_per_sample = wave_format.bits_per_sample;
            decoder->src_block_align     = wave_format.block_align;
            decoder->src_is_float        = 0;

            decoder->format.sample_rate = wave_format.sample_rate;
            decoder->format.num_channels = wave_format.num_channels;
            decoder->format.bits_per_sample = 32;
            decoder->format.valid_bits_per_sample = wave_format.valid_bits_per_sample ? wave_format.valid_bits_per_sample : wave_format.bits_per_sample;
            decoder->format.block_align = wave_format.num_channels * sizeof(float);
            decoder->format.total_samples = wave_format.num_samples;
            decoder->format.channel_mask = wave_format.channel_mask;
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
            
            FlacFormat flac_format;
            if (flac_get_format(flac, &flac_format) != 0) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to get FLAC format: %s", flac_get_error(flac));
                flac_close(flac);
                free(decoder);
                return NULL;
            }
            
            decoder->src_bits_per_sample = flac_format.bits_per_sample;
            decoder->src_block_align     = flac_format.block_align;
            decoder->src_is_float        = 0;

            decoder->format.sample_rate = flac_format.sample_rate;
            decoder->format.num_channels = flac_format.num_channels;
            decoder->format.bits_per_sample = 32;
            decoder->format.valid_bits_per_sample = flac_format.valid_bits_per_sample;
            decoder->format.block_align = flac_format.num_channels * 4;
            decoder->format.total_samples = (uint32_t)flac_format.total_samples;
            decoder->format.channel_mask = flac_format.channel_mask;
            strncpy(decoder->format.codec_name, "FLAC", sizeof(decoder->format.codec_name) - 1);
            
            decoder->decoder_instance = flac;
            break;
        }
        
        case DECODER_TYPE_MP3: {
            Mp3File* mp3 = mp3_open(filepath);
            if (!mp3) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to open MP3 file (Missing libmpg123-0.dll?)");
                free(decoder);
                return NULL;
            }
            
            Mp3Format mp3_format;
            if (mp3_get_format(mp3, &mp3_format) != 0) {
                snprintf(decoder->error_msg, sizeof(decoder->error_msg),
                         "Failed to get MP3 format");
                mp3_close(mp3);
                free(decoder);
                return NULL;
            }

            /* mp3_parser always outputs 16-bit signed PCM */
            decoder->src_bits_per_sample = 16;
            decoder->src_block_align     = mp3_format.block_align;
            decoder->src_is_float        = 0;
            
            decoder->format.sample_rate = mp3_format.sample_rate;
            decoder->format.num_channels = mp3_format.num_channels;
            decoder->format.bits_per_sample = 32;
            decoder->format.valid_bits_per_sample = 16;
            decoder->format.block_align = mp3_format.num_channels * 4;
            decoder->format.total_samples = (uint32_t)mp3_format.total_samples;
            decoder->format.channel_mask = 0; 
            strncpy(decoder->format.codec_name, "MP3 (mpg123)", sizeof(decoder->format.codec_name) - 1);
            
            decoder->decoder_instance = mp3;
            break;
        }
            
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
    if (!decoder || !buffer) return (size_t)-1;

    if (decoder->src_is_float) {
        switch (decoder->type) {
            case DECODER_TYPE_WAVE: {
                WaveFile* wave = (WaveFile*)decoder->decoder_instance;
                size_t frames = wave_read_samples(wave, buffer, num_samples);
                return frames;
            }
            default:
                return (size_t)-1;
        }
    }

    if (ensure_raw_buf(decoder, num_samples) != 0) return (size_t)-1;

    size_t frames_read = 0;
    
    switch (decoder->type) {
        case DECODER_TYPE_WAVE: {
            WaveFile* wave = (WaveFile*)decoder->decoder_instance;
            frames_read = wave_read_samples(wave, decoder->raw_buffer, num_samples);
            break;
        }
        case DECODER_TYPE_FLAC: {
            FlacFile* flac = (FlacFile*)decoder->decoder_instance;
            frames_read = flac_read_samples(flac, decoder->raw_buffer, num_samples);
            break;
        }
        case DECODER_TYPE_MP3: {
            Mp3File* mp3 = (Mp3File*)decoder->decoder_instance;
            frames_read = mp3_read_samples(mp3, decoder->raw_buffer, num_samples);
            break;
        }
        default:
            return (size_t)-1;
    }

    if (frames_read == (size_t)-1) return (size_t)-1;
    if (frames_read == 0) return 0;

    size_t total_samples = frames_read * decoder->format.num_channels;
    float* dst = (float*)buffer;

    switch (decoder->src_bits_per_sample) {
        case 8:
            pcm8_to_float((const uint8_t*)decoder->raw_buffer, dst, total_samples);
            break;
        case 16:
            pcm16_to_float((const int16_t*)decoder->raw_buffer, dst, total_samples);
            break;
        case 24:
            if (decoder->src_block_align == decoder->format.num_channels * 3) {
                /* PACKED: 3 bytes per sample (Standard WAV) */
                pcm24_packed_to_float((const uint8_t*)decoder->raw_buffer, dst, total_samples);
            }
            else {
                /* PADDED: 4 bytes per sample (WAVEFORMATEXTENSIBLE 24-in-32) */
                pcm24_padded_to_float((const int32_t*)decoder->raw_buffer, dst, total_samples);
            }
            break;
        case 32:
            pcm32_to_float((const int32_t*)decoder->raw_buffer, dst, total_samples);
            break;
        default:
            return (size_t)-1;
    }

    return frames_read;
}

int audio_decoder_seek(AudioDecoder* decoder, uint32_t sample_position) {
    if (!decoder) return -1;
    switch (decoder->type) {
        case DECODER_TYPE_WAVE:
            return wave_seek((WaveFile*)decoder->decoder_instance, sample_position);
        case DECODER_TYPE_FLAC:
            return flac_seek((FlacFile*)decoder->decoder_instance, sample_position);
        case DECODER_TYPE_MP3:
            return mp3_seek((Mp3File*)decoder->decoder_instance, sample_position);
        default:
            return -1;
    }
}

uint32_t audio_decoder_tell(AudioDecoder* decoder) {
    if (!decoder) return 0;
    switch (decoder->type) {
        case DECODER_TYPE_WAVE:
            return wave_tell((WaveFile*)decoder->decoder_instance);
        case DECODER_TYPE_FLAC:
            return (uint32_t)flac_tell((FlacFile*)decoder->decoder_instance);
        case DECODER_TYPE_MP3:
            return (uint32_t)mp3_tell((Mp3File*)decoder->decoder_instance);
        default:
            return 0;
    }
}

const char* audio_decoder_get_error(AudioDecoder* decoder) {
    if (!decoder) return "Invalid decoder handle";
    if (decoder->error_msg[0]) return decoder->error_msg;
    switch (decoder->type) {
        case DECODER_TYPE_WAVE:
            return wave_get_error((WaveFile*)decoder->decoder_instance);
        case DECODER_TYPE_FLAC:
            return flac_get_error((FlacFile*)decoder->decoder_instance);
        case DECODER_TYPE_MP3:
            return mp3_get_error((Mp3File*)decoder->decoder_instance);
        default:
            return NULL;
    }
}

void audio_decoder_close(AudioDecoder* decoder) {
    if (!decoder) return;
    switch (decoder->type) {
        case DECODER_TYPE_WAVE:
            if (decoder->decoder_instance)
                wave_close((WaveFile*)decoder->decoder_instance);
            break;
        case DECODER_TYPE_FLAC:
            if (decoder->decoder_instance)
                flac_close((FlacFile*)decoder->decoder_instance);
            break;
        case DECODER_TYPE_MP3:
            if (decoder->decoder_instance)
                mp3_close((Mp3File*)decoder->decoder_instance);
            break;
        default:
            break;
    }
    if (decoder->raw_buffer) free(decoder->raw_buffer);
    free(decoder);
}