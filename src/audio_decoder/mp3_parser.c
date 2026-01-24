#include "mp3_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

/* --- Dynamic Loading Boilerplate --- */
#ifdef _WIN32
    #include <windows.h>
    #define LIB_HANDLE HMODULE
    #define LOAD_LIBRARY(name) LoadLibraryA(name)
    #define GET_PROC_ADDRESS GetProcAddress
    #define FREE_LIBRARY FreeLibrary
    #define LIB_NAME "libmpg123-0.dll"
#else
    #include <dlfcn.h>
    #define LIB_HANDLE void*
    #define LOAD_LIBRARY(name) dlopen(name, RTLD_LAZY)
    #define GET_PROC_ADDRESS dlsym
    #define FREE_LIBRARY dlclose
    #ifdef __APPLE__
        #define LIB_NAME "libmpg123.dylib"
    #else
        #define LIB_NAME "libmpg123.so.0"
    #endif
#endif

/* --- MPG123 Types and Enums --- */
typedef struct mpg123_handle_struct mpg123_handle;

enum mpg123_errors {
    MPG123_OK = 0,
    MPG123_DONE = -12,
    MPG123_NEW_FORMAT = -11,
    MPG123_NEED_MORE = -10,
    MPG123_ERR = -1,
};

enum mpg123_enc_enum {
    MPG123_ENC_SIGNED_16 = 0x0D0
};

/* --- Function Pointers --- */
typedef int (*func_init)(void);
typedef void (*func_exit)(void);
typedef mpg123_handle* (*func_new)(const char*, int*);
typedef void (*func_delete)(mpg123_handle*);
typedef int (*func_open)(mpg123_handle*, const char*);
typedef int (*func_close)(mpg123_handle*);
typedef int (*func_read)(mpg123_handle*, unsigned char*, size_t, size_t*);
typedef int64_t (*func_seek)(mpg123_handle*, int64_t, int);
typedef int64_t (*func_tell)(mpg123_handle*);
typedef int64_t (*func_length)(mpg123_handle*);
typedef int (*func_getformat)(mpg123_handle*, long*, int*, int*);
typedef int (*func_format_none)(mpg123_handle*);
typedef int (*func_format)(mpg123_handle*, long, int, int);
typedef const char* (*func_strerror)(mpg123_handle*);

static func_init mpg123_init_ptr = NULL;
static func_exit mpg123_exit_ptr = NULL;
static func_new mpg123_new_ptr = NULL;
static func_delete mpg123_delete_ptr = NULL;
static func_open mpg123_open_ptr = NULL;
static func_close mpg123_close_ptr = NULL;
static func_read mpg123_read_ptr = NULL;
static func_seek mpg123_seek_ptr = NULL;
static func_tell mpg123_tell_ptr = NULL;
static func_length mpg123_length_ptr = NULL;
static func_getformat mpg123_getformat_ptr = NULL;
static func_format_none mpg123_format_none_ptr = NULL;
static func_format mpg123_format_ptr = NULL;
static func_strerror mpg123_strerror_ptr = NULL;

static LIB_HANDLE lib_handle = NULL;
static int lib_initialized = 0;
static char lib_error[256] = {0};

/* --- Implementation --- */

static int load_mpg123_library(void) {
    if (lib_initialized) return lib_handle ? 0 : -1;
    lib_initialized = 1;

    lib_handle = LOAD_LIBRARY(LIB_NAME);
    if (!lib_handle) {
        snprintf(lib_error, sizeof(lib_error), "Failed to load %s", LIB_NAME);
        return -1;
    }

    /* Load symbols */
    mpg123_init_ptr = (func_init)GET_PROC_ADDRESS(lib_handle, "mpg123_init");
    mpg123_exit_ptr = (func_exit)GET_PROC_ADDRESS(lib_handle, "mpg123_exit");
    mpg123_new_ptr = (func_new)GET_PROC_ADDRESS(lib_handle, "mpg123_new");
    mpg123_delete_ptr = (func_delete)GET_PROC_ADDRESS(lib_handle, "mpg123_delete");
    mpg123_open_ptr = (func_open)GET_PROC_ADDRESS(lib_handle, "mpg123_open");
    mpg123_close_ptr = (func_close)GET_PROC_ADDRESS(lib_handle, "mpg123_close");
    mpg123_read_ptr = (func_read)GET_PROC_ADDRESS(lib_handle, "mpg123_read");
    mpg123_seek_ptr = (func_seek)GET_PROC_ADDRESS(lib_handle, "mpg123_seek");
    mpg123_tell_ptr = (func_tell)GET_PROC_ADDRESS(lib_handle, "mpg123_tell");
    mpg123_length_ptr = (func_length)GET_PROC_ADDRESS(lib_handle, "mpg123_length");
    mpg123_getformat_ptr = (func_getformat)GET_PROC_ADDRESS(lib_handle, "mpg123_getformat");
    mpg123_format_none_ptr = (func_format_none)GET_PROC_ADDRESS(lib_handle, "mpg123_format_none");
    mpg123_format_ptr = (func_format)GET_PROC_ADDRESS(lib_handle, "mpg123_format");
    mpg123_strerror_ptr = (func_strerror)GET_PROC_ADDRESS(lib_handle, "mpg123_strerror");

    if (!mpg123_init_ptr || !mpg123_new_ptr || !mpg123_open_ptr || !mpg123_read_ptr) {
        snprintf(lib_error, sizeof(lib_error), "Failed to load symbols from %s", LIB_NAME);
        FREE_LIBRARY(lib_handle);
        lib_handle = NULL;
        return -1;
    }

    mpg123_init_ptr();
    return 0;
}

struct Mp3File {
    mpg123_handle* mh;
    Mp3Format format;
    char error_msg[256];
    int is_valid;
    int eof_reached;
};

Mp3File* mp3_open(const char* filepath) {
    if (load_mpg123_library() != 0) return NULL;

    Mp3File* mp3 = (Mp3File*)calloc(1, sizeof(Mp3File));
    if (!mp3) return NULL;

    int err = 0;
    mp3->mh = mpg123_new_ptr(NULL, &err);
    if (!mp3->mh) {
        snprintf(mp3->error_msg, sizeof(mp3->error_msg), "Failed to create mpg123 handle: %d", err);
        free(mp3);
        return NULL;
    }

    /* Open the file */
    if (mpg123_open_ptr(mp3->mh, filepath) != MPG123_OK) {
        snprintf(mp3->error_msg, sizeof(mp3->error_msg), "Failed to open MP3: %s", mpg123_strerror_ptr(mp3->mh));
        mpg123_delete_ptr(mp3->mh);
        free(mp3);
        return NULL;
    }

    /* Force format to 16-bit Signed Interleaved. */
    long rate = 0;
    int channels = 0, enc = 0;
    
    mpg123_getformat_ptr(mp3->mh, &rate, &channels, &enc);
    
    /* Disable all formats, then enable only what we want */
    mpg123_format_none_ptr(mp3->mh);
    mpg123_format_ptr(mp3->mh, rate, channels, MPG123_ENC_SIGNED_16);

    /* Populate format struct */
    mp3->format.sample_rate = (uint32_t)rate;
    mp3->format.num_channels = (uint16_t)channels;
    mp3->format.bits_per_sample = 16;
    mp3->format.block_align = channels * 2;
    mp3->format.total_samples = (uint64_t)mpg123_length_ptr(mp3->mh);

    mp3->is_valid = 1;
    return mp3;
}

int mp3_get_format(Mp3File* mp3, Mp3Format* format) {
    if (!mp3 || !mp3->is_valid || !format) return -1;
    memcpy(format, &mp3->format, sizeof(Mp3Format));
    return 0;
}

size_t mp3_read_samples(Mp3File* mp3, void* buffer, size_t num_frames) {
    if (!mp3 || !mp3->is_valid) return -1;
    if (mp3->eof_reached) return 0;

    size_t bytes_needed = num_frames * mp3->format.block_align;
    size_t bytes_read = 0;
    
    int err = mpg123_read_ptr(mp3->mh, (unsigned char*)buffer, bytes_needed, &bytes_read);

    if (err == MPG123_DONE || (err == MPG123_OK && bytes_read == 0)) {
        mp3->eof_reached = 1;
        return bytes_read / mp3->format.block_align;
    }

    if (err != MPG123_OK && err != MPG123_DONE && err != MPG123_NEW_FORMAT) {
        snprintf(mp3->error_msg, sizeof(mp3->error_msg), "MP3 read error: %s", mpg123_strerror_ptr(mp3->mh));
        return -1;
    }

    return bytes_read / mp3->format.block_align;
}

int mp3_seek(Mp3File* mp3, uint64_t sample_position) {
    if (!mp3 || !mp3->is_valid) return -1;

    int64_t offset = (int64_t)sample_position;
    
    if (mpg123_seek_ptr(mp3->mh, offset, SEEK_SET) < 0) {
        snprintf(mp3->error_msg, sizeof(mp3->error_msg), "MP3 seek error: %s", mpg123_strerror_ptr(mp3->mh));
        return -1;
    }
    
    mp3->eof_reached = 0;
    return 0;
}

uint64_t mp3_tell(Mp3File* mp3) {
    if (!mp3 || !mp3->is_valid) return 0;
    return (uint64_t)mpg123_tell_ptr(mp3->mh);
}

const char* mp3_get_error(Mp3File* mp3) {
    if (!mp3) return "Invalid MP3 handle";
    if (mp3->is_valid && mp3->mh) {
         return mp3->error_msg[0] ? mp3->error_msg : NULL; 
    }
    return mp3->error_msg[0] ? mp3->error_msg : NULL;
}

void mp3_close(Mp3File* mp3) {
    if (mp3) {
        if (mp3->mh) {
            mpg123_close_ptr(mp3->mh);
            mpg123_delete_ptr(mp3->mh);
        }
        free(mp3);
    }
}