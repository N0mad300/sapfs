#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <stdint.h>
#include <stddef.h>

/*
 * By default decoders normalize to interleaved 32-bit float output.
 * audio_decoder_open_native() preserves integer precision for direct output.
 * bits_per_sample and block_align in AudioDecoderFormat describe the output
 * frames, not the on-disk representation.
 */

typedef struct AudioDecoder AudioDecoder;

typedef struct {
    uint32_t sample_rate;
    uint16_t num_channels;
    uint16_t bits_per_sample;           /* Sample width; normally 32 (float) */
    uint16_t valid_bits_per_sample;     /* Source bit depth */
    uint16_t block_align;
    uint32_t total_samples;
    uint32_t channel_mask;
    int      is_float;
    char     codec_name[32];
} AudioDecoderFormat;

AudioDecoder* audio_decoder_open(const char* filepath);
/* Interleaved PCM without a float round trip. Query its format before reading. */
AudioDecoder* audio_decoder_open_native(const char* filepath);
int audio_decoder_get_format(AudioDecoder* decoder, AudioDecoderFormat* format);
size_t audio_decoder_read_samples(AudioDecoder* decoder, void* buffer, size_t num_samples);
int audio_decoder_seek(AudioDecoder* decoder, uint32_t sample_position);
uint32_t audio_decoder_tell(AudioDecoder* decoder);
const char* audio_decoder_get_error(AudioDecoder* decoder);
void audio_decoder_close(AudioDecoder* decoder);

#endif /* AUDIO_DECODER_H */
