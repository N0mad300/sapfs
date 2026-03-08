#include "flac_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <windows.h>
    #define LIB_HANDLE HMODULE
    #define LOAD_LIBRARY(name) LoadLibraryA(name)
    #define GET_PROC_ADDRESS GetProcAddress
    #define FREE_LIBRARY FreeLibrary
    #define LIB_NAME "libFLAC.dll"
#else
    #include <dlfcn.h>
    #define LIB_HANDLE void*
    #define LOAD_LIBRARY(name) dlopen(name, RTLD_LAZY)
    #define GET_PROC_ADDRESS dlsym
    #define FREE_LIBRARY dlclose
    #ifdef __APPLE__
        #define LIB_NAME "libFLAC.dylib"
    #else
        #define LIB_NAME "libFLAC.so.8"
    #endif
#endif

typedef enum {
    FLAC__STREAM_DECODER_INIT_STATUS_OK = 0
} FLAC__StreamDecoderInitStatus;

typedef enum {
    FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE = 0,
    FLAC__STREAM_DECODER_WRITE_STATUS_ABORT
} FLAC__StreamDecoderWriteStatus;

typedef enum {
    FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC = 0
} FLAC__StreamDecoderErrorStatus;

typedef enum {
    FLAC__METADATA_TYPE_STREAMINFO = 0
} FLAC__MetadataType;

typedef enum {
    FLAC__STREAM_DECODER_END_OF_STREAM = 4
} FLAC__StreamDecoderState;

typedef int FLAC__bool;
typedef int FLAC__int32;
typedef unsigned long long FLAC__uint64;

typedef struct FLAC__StreamDecoder FLAC__StreamDecoder;
typedef struct FLAC__Frame FLAC__Frame;
typedef struct FLAC__FrameHeader FLAC__FrameHeader;
typedef struct FLAC__StreamMetadata FLAC__StreamMetadata;
typedef struct FLAC__StreamMetadata_StreamInfo FLAC__StreamMetadata_StreamInfo;

struct FLAC__FrameHeader {
    unsigned blocksize;
    unsigned sample_rate;
    unsigned channels;
    unsigned bits_per_sample;
    FLAC__uint64 number;
};

struct FLAC__Frame {
    FLAC__FrameHeader header;
};

struct FLAC__StreamMetadata_StreamInfo {
    unsigned min_blocksize;
    unsigned max_blocksize;
    unsigned min_framesize;
    unsigned max_framesize;
    unsigned sample_rate;
    unsigned channels;
    unsigned bits_per_sample;
    FLAC__uint64 total_samples;
    unsigned char md5sum[16];
};

struct FLAC__StreamMetadata {
    FLAC__MetadataType type;
    unsigned is_last;
    unsigned length;
    union {
        FLAC__StreamMetadata_StreamInfo stream_info;
    } data;
};

typedef FLAC__StreamDecoder* (*FLAC__stream_decoder_new_func)(void);
typedef void (*FLAC__stream_decoder_delete_func)(FLAC__StreamDecoder*);
typedef FLAC__StreamDecoderInitStatus (*FLAC__stream_decoder_init_file_func)(
    FLAC__StreamDecoder*,
    const char*,
    FLAC__StreamDecoderWriteStatus (*)(const FLAC__StreamDecoder*, const FLAC__Frame*, const FLAC__int32* const[], void*),
    void (*)(const FLAC__StreamDecoder*, const FLAC__StreamMetadata*, void*),
    void (*)(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void*),
    void*
);
typedef FLAC__bool (*FLAC__stream_decoder_process_until_end_of_metadata_func)(FLAC__StreamDecoder*);
typedef FLAC__bool (*FLAC__stream_decoder_process_single_func)(FLAC__StreamDecoder*);
typedef FLAC__bool (*FLAC__stream_decoder_seek_absolute_func)(FLAC__StreamDecoder*, FLAC__uint64);
typedef FLAC__bool (*FLAC__stream_decoder_finish_func)(FLAC__StreamDecoder*);
typedef FLAC__StreamDecoderState (*FLAC__stream_decoder_get_state_func)(FLAC__StreamDecoder*);

static FLAC__stream_decoder_new_func flac_decoder_new = NULL;
static FLAC__stream_decoder_delete_func flac_decoder_delete = NULL;
static FLAC__stream_decoder_init_file_func flac_decoder_init_file = NULL;
static FLAC__stream_decoder_process_until_end_of_metadata_func flac_process_metadata = NULL;
static FLAC__stream_decoder_process_single_func flac_process_single = NULL;
static FLAC__stream_decoder_seek_absolute_func flac_seek_absolute = NULL;
static FLAC__stream_decoder_finish_func flac_decoder_finish = NULL;
static FLAC__stream_decoder_get_state_func flac_get_state = NULL;

static LIB_HANDLE flac_lib = NULL;
static int flac_lib_initialized = 0;
static char flac_lib_error[256] = {0};

static int load_flac_library(void) {
    if (flac_lib_initialized) {
        return flac_lib != NULL ? 0 : -1;
    }
    
    flac_lib_initialized = 1;
    
    flac_lib = LOAD_LIBRARY(LIB_NAME);
    if (!flac_lib) {
        snprintf(flac_lib_error, sizeof(flac_lib_error), "Failed to load %s. Please install libFLAC.", LIB_NAME);
        return -1;
    }
    
    flac_decoder_new = (FLAC__stream_decoder_new_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_new");
    flac_decoder_delete = (FLAC__stream_decoder_delete_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_delete");
    flac_decoder_init_file = (FLAC__stream_decoder_init_file_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_init_file");
    flac_process_metadata = (FLAC__stream_decoder_process_until_end_of_metadata_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_process_until_end_of_metadata");
    flac_process_single = (FLAC__stream_decoder_process_single_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_process_single");
    flac_seek_absolute = (FLAC__stream_decoder_seek_absolute_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_seek_absolute");
    flac_decoder_finish = (FLAC__stream_decoder_finish_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_finish");
    flac_get_state = (FLAC__stream_decoder_get_state_func)
        GET_PROC_ADDRESS(flac_lib, "FLAC__stream_decoder_get_state");
    
    if (!flac_decoder_new       || !flac_decoder_delete || !flac_decoder_init_file  ||
        !flac_process_metadata  || !flac_process_single || !flac_seek_absolute      ||
        !flac_decoder_finish    || !flac_get_state) {
        snprintf(flac_lib_error, sizeof(flac_lib_error), "Failed to load FLAC functions from %s", LIB_NAME);
        FREE_LIBRARY(flac_lib);
        flac_lib = NULL;
        return -1;
    }
    
    return 0;
}

struct FlacFile {
    FLAC__StreamDecoder* decoder;
    FlacFormat format;
    
    int32_t* decode_buffer;
    size_t decode_buffer_size;
    size_t decode_buffer_used;
    size_t decode_buffer_pos;
    
    uint64_t current_sample;
    uint64_t target_sample;
    int seeking;

    unsigned int original_bits_per_sample;
    
    char error_msg[512];
    int is_valid;
    int eof_reached;
};

static FLAC__StreamDecoderWriteStatus write_callback(
    const FLAC__StreamDecoder* decoder,
    const FLAC__Frame* frame,
    const FLAC__int32* const buffer[],
    void* client_data)
{
    FlacFile* flac = (FlacFile*)client_data;
    (void)decoder;
    
    size_t samples = frame->header.blocksize;
    size_t channels = frame->header.channels;
    
    if (flac->seeking) {
        if (flac->current_sample + samples > flac->target_sample) {
            flac->seeking = 0;
        } else {
            flac->current_sample += samples;
            return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
        }
    }
    
    if (samples > flac->decode_buffer_size) {
        size_t new_size = samples * 2;
        int32_t* new_buffer = (int32_t*)realloc(
            flac->decode_buffer,
            new_size * channels * sizeof(int32_t)
        );
        if (!new_buffer) {
            snprintf(flac->error_msg, sizeof(flac->error_msg), "Failed to allocate decode buffer");
            return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
        }
        flac->decode_buffer = new_buffer;
        flac->decode_buffer_size = new_size;
    }
    
    for (size_t i = 0; i < samples; i++) {
        for (size_t ch = 0; ch < channels; ch++) {
            flac->decode_buffer[i * channels + ch] = buffer[ch][i];
        }
    }
    
    flac->decode_buffer_used = samples;
    flac->decode_buffer_pos = 0;
    flac->current_sample += samples;
    
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void metadata_callback(
    const FLAC__StreamDecoder* decoder,
    const FLAC__StreamMetadata* metadata,
    void* client_data)
{
    FlacFile* flac = (FlacFile*)client_data;
    (void)decoder;
    
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        flac->format.sample_rate = metadata->data.stream_info.sample_rate;
        flac->format.num_channels = metadata->data.stream_info.channels;

        unsigned bps = metadata->data.stream_info.bits_per_sample;

        flac->original_bits_per_sample = bps;
        flac->format.valid_bits_per_sample = bps;

        if (bps > 16) {
            flac->format.bits_per_sample = bps;
            flac->format.block_align = flac->format.num_channels * 4;
        } else {
            flac->format.bits_per_sample = 16;
            flac->format.block_align = flac->format.num_channels * 2;
        }

        flac->format.channel_mask = 0;
        flac->format.total_samples = metadata->data.stream_info.total_samples;
        flac->format.data_size = (uint32_t)(flac->format.total_samples * flac->format.block_align);
    }
}

static void error_callback(
    const FLAC__StreamDecoder* decoder,
    FLAC__StreamDecoderErrorStatus status,
    void* client_data)
{
    FlacFile* flac = (FlacFile*)client_data;
    (void)decoder;
    
    snprintf(flac->error_msg, sizeof(flac->error_msg),
            "FLAC decoder error: %d", status);
}

FlacFile* flac_open(const char* filepath) {
    FlacFile* flac = NULL;
    FLAC__StreamDecoderInitStatus init_status;
    
    if (!filepath) {
        return NULL;
    }
    
    if (load_flac_library() != 0) {
        fprintf(stderr, "Error: %s\n", flac_lib_error);
        return NULL;
    }
    
    flac = (FlacFile*)calloc(1, sizeof(FlacFile));
    if (!flac) {
        return NULL;
    }
    
    flac->decoder = flac_decoder_new();
    if (!flac->decoder) {
        snprintf(flac->error_msg, sizeof(flac->error_msg),
                "Failed to create FLAC decoder");
        free(flac);
        return NULL;
    }
    
    init_status = flac_decoder_init_file(
        flac->decoder,
        filepath,
        write_callback,
        metadata_callback,
        error_callback,
        flac
    );
    
    if (init_status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        snprintf(flac->error_msg, sizeof(flac->error_msg), "Failed to initialize FLAC decoder: %d", init_status);
        flac_decoder_delete(flac->decoder);
        free(flac);
        return NULL;
    }
    
    if (!flac_process_metadata(flac->decoder)) {
        snprintf(flac->error_msg, sizeof(flac->error_msg), "Failed to process FLAC metadata");
        flac_decoder_delete(flac->decoder);
        free(flac);
        return NULL;
    }
    
    if (flac->format.num_channels == 0 || flac->format.sample_rate == 0) {
        snprintf(flac->error_msg, sizeof(flac->error_msg), "Invalid FLAC format");
        flac_decoder_delete(flac->decoder);
        free(flac);
        return NULL;
    }
    
    flac->decode_buffer_size = 4096;
    flac->decode_buffer = (int32_t*)malloc(
        flac->decode_buffer_size * flac->format.num_channels * sizeof(int32_t)
    );
    if (!flac->decode_buffer) {
        snprintf(flac->error_msg, sizeof(flac->error_msg), "Failed to allocate decode buffer");
        flac_decoder_delete(flac->decoder);
        free(flac);
        return NULL;
    }
    
    flac->current_sample = 0;
    flac->is_valid = 1;
    flac->eof_reached = 0;
    
    return flac;
}

int flac_get_format(FlacFile* flac, FlacFormat* format) {
    if (!flac || !format || !flac->is_valid) return -1;
    
    memcpy(format, &flac->format, sizeof(FlacFormat));
    return 0;
}

size_t flac_read_samples(FlacFile* flac, void* buffer, size_t num_samples) {
    if (!flac || !buffer || !flac->is_valid)
        return (size_t)-1;

    if (flac->eof_reached)
        return 0;
    
    int is_high_res = (flac->format.bits_per_sample > 16);
    int32_t* out_32 = (int32_t*)buffer;
    int16_t* out_16 = (int16_t*)buffer;

    size_t samples_read = 0;
    size_t channels = flac->format.num_channels;
    unsigned int bits = flac->original_bits_per_sample;
    
    while (samples_read < num_samples) {
        if (flac->decode_buffer_pos < flac->decode_buffer_used) {
            size_t available = flac->decode_buffer_used - flac->decode_buffer_pos;
            size_t to_copy = num_samples - samples_read;
            if (to_copy > available) {
                to_copy = available;
            }
            
            for (size_t i = 0; i < to_copy; i++) {
                for (size_t ch = 0; ch < channels; ch++) {
                    int32_t sample = flac->decode_buffer[
                        (flac->decode_buffer_pos + i) * channels + ch
                    ];
                    
                    if (is_high_res) {
                        out_32[(samples_read + i) * channels + ch] = sample;
                    } else {
                        out_16[(samples_read + i) * channels + ch] = (int16_t)sample;
                    }
                }
            }
            
            samples_read += to_copy;
            flac->decode_buffer_pos += to_copy;
        } else {
            flac->decode_buffer_used = 0;
            flac->decode_buffer_pos = 0;
            
            if (!flac_process_single(flac->decoder)) {
                FLAC__StreamDecoderState state = flac_get_state(flac->decoder);
                
                if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                    flac->eof_reached = 1;
                    break;
                } else {
                    snprintf(flac->error_msg, sizeof(flac->error_msg),
                            "FLAC decode error: state %d", state);
                    return -1;
                }
            }
            
            if (flac->decode_buffer_used == 0) {
                flac->eof_reached = 1;
                break;
            }
        }
    }
    
    return samples_read;
}

int flac_seek(FlacFile* flac, uint64_t sample_position) {
    if (!flac || !flac->is_valid) return -1;
    
    if (sample_position > flac->format.total_samples) {
        snprintf(flac->error_msg, sizeof(flac->error_msg), "Seek position out of bounds");
        return -1;
    }
    
    if (!flac_seek_absolute(flac->decoder, sample_position)) {
        snprintf(flac->error_msg, sizeof(flac->error_msg), "FLAC seek failed");
        return -1;
    }
    
    flac->current_sample = sample_position;
    flac->decode_buffer_used = 0;
    flac->decode_buffer_pos = 0;
    flac->eof_reached = 0;
    
    return 0;
}

uint64_t flac_tell(FlacFile* flac) {
    if (!flac || !flac->is_valid) {
        return 0;
    }
    return flac->current_sample;
}

void flac_close(FlacFile* flac) {
    if (flac) {
        if (flac->decoder) {
            flac_decoder_finish(flac->decoder);
            flac_decoder_delete(flac->decoder);
        }
        if (flac->decode_buffer) {
            free(flac->decode_buffer);
        }
        free(flac);
    }
}

const char* flac_get_error(FlacFile* flac) {
    if (!flac) {
        return "Invalid FLAC file handle";
    }
    return flac->error_msg[0] ? flac->error_msg : NULL;
}