#include "audio_decoder/audio_decoder.h"
#include "audio_output/audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* Platform-specific includes */
#ifdef _WIN32
    #include "audio_output/wasapi_output.h"
    #include <windows.h>
    #define SLEEP_MS(ms) Sleep(ms)
#else
    #include <unistd.h>
    #define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* Global flag for graceful shutdown */
static volatile int g_running = 1;

void setCursorVisible(int visible)
{
#if defined(_WIN32)
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_CURSOR_INFO info;

    GetConsoleCursorInfo(hConsole, &info);
    info.bVisible = visible ? TRUE : FALSE;
    SetConsoleCursorInfo(hConsole, &info);
#else
    if (visible)
        printf("\033[?25h");  // show cursor
    else
        printf("\033[?25l");  // hide cursor

    fflush(stdout);
#endif
}

/* Signal handler for Ctrl+C */
void signal_handler(int signum) {
    (void)signum;
    printf("\n\nReceived interrupt signal. Stopping playback...\n");
    setCursorVisible(1);
    g_running = 0;
}

/* Print usage information */
void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS] <audio_file>\n", program_name);
    printf("\nSimple audio player that plays various audio formats.\n");
    printf("\nOptions:\n");
#ifdef _WIN32
    printf("  --exclusive      Use WASAPI exclusive mode for bit-perfect playback\n");
    printf("                   (lower latency, requires exact format match)\n");
    printf("  --buffer <ms>    Set buffer size in milliseconds (shared mode only)\n");
    printf("                   Default: 1000 ms\n");
#endif
    printf("  -h, --help       Show this help message\n");
    printf("\nControls:\n");
    printf("  Ctrl+C - Stop playback and exit\n");
    printf("\nSupported formats:\n");
    printf("  - WAVE (.wav) - PCM uncompressed audio\n");
    printf("  - FLAC (.flac) - Free Lossless Audio Codec\n");
    printf("  - MP3 (.mp3) - Coming soon\n");
    printf("\nAudio specifications:\n");
    printf("  - 8, 16, 24, or 32-bit samples\n");
    printf("  - Mono or multi-channel audio\n");
    printf("  - Sample rates from 8000 Hz to 192000 Hz\n");
    printf("\nExamples:\n");
    printf("  %s music.wav\n", program_name);
#ifdef _WIN32
    printf("  %s --exclusive music.flac\n", program_name);
    printf("  %s --buffer 500 music.wav\n", program_name);
#endif
}

/* Convert decoder format to output format */
void decoder_format_to_audio_format(const AudioDecoderFormat* dec_fmt, AudioFormat* audio_fmt) {
    audio_fmt->sample_rate = dec_fmt->sample_rate;
    audio_fmt->num_channels = dec_fmt->num_channels;
    audio_fmt->bits_per_sample = dec_fmt->bits_per_sample;
    audio_fmt->block_align = dec_fmt->block_align;
}

/* Display file information */
void display_file_info(const AudioDecoderFormat* format) {
    printf("\n========================================\n");
    printf("        Audio File Information\n");
    printf("========================================\n");
    printf("Codec:           %s\n", format->codec_name);
    printf("Sample Rate:     %u Hz\n", format->sample_rate);
    printf("Channels:        %u (%s)\n", 
           format->num_channels,
           format->num_channels == 1 ? "Mono" : 
           format->num_channels == 2 ? "Stereo" : "Multi-channel");
    printf("Bit Depth:       %u bits\n", format->bits_per_sample);
    printf("Block Align:     %u bytes\n", format->block_align);
    printf("Total Samples:   %u frames\n", format->total_samples);
    
    double duration = (double)format->total_samples / format->sample_rate;
    int minutes = (int)duration / 60;
    int seconds = (int)duration % 60;
    printf("Duration:        %d:%02d (%.2f seconds)\n", minutes, seconds, duration);
    
    double size_mb = (double)(format->total_samples * format->block_align) / (1024.0 * 1024.0);
    printf("Estimated Size:  %.2f MB\n", size_mb);
    printf("========================================\n\n");
}

/* Display playback progress */
void display_progress(uint32_t current_sample, uint32_t total_samples, uint32_t sample_rate) {
    double current_time = (double)current_sample / sample_rate;
    double total_time = (double)total_samples / sample_rate;
    double percentage = ((double)current_sample / total_samples) * 100.0;
    
    int current_min = (int)current_time / 60;
    int current_sec = (int)current_time % 60;
    int total_min = (int)total_time / 60;
    int total_sec = (int)total_time % 60;
    
    /* Print progress bar */
    printf("\r[");
    int bar_width = 40;
    int filled = (int)(percentage / 100.0 * bar_width);
    for (int i = 0; i < bar_width; i++) {
        if (i < filled) {
            printf("=");
        } else if (i == filled) {
            printf(">");
        } else {
            printf(" ");
        }
    }
    printf("] %3.0f%% [%d:%02d / %d:%02d]", 
           percentage, current_min, current_sec, total_min, total_sec);
    fflush(stdout);
}

/* Main playback function - now codec-agnostic! */
int play_audio_file(const char* filepath, int use_exclusive, unsigned int buffer_ms) {
    AudioDecoder* decoder = NULL;
    AudioOutput* audio = NULL;
    AudioDecoderFormat decoder_format;
    AudioFormat audio_format;
    int result = -1;
    
    /* Buffer for reading samples */
    uint8_t* buffer = NULL;
    size_t buffer_frames = 0;
    
    /* Open audio file with automatic format detection */
    printf("Opening file: %s\n", filepath);
    decoder = audio_decoder_open(filepath);
    if (!decoder) {
        fprintf(stderr, "Error: Failed to open audio file (unsupported format?)\n");
        return -1;
    }
    
    /* Get format information */
    if (audio_decoder_get_format(decoder, &decoder_format) != 0) {
        fprintf(stderr, "Error: Failed to get format: %s\n", 
                audio_decoder_get_error(decoder));
        goto cleanup;
    }
    
    /* Display file information */
    display_file_info(&decoder_format);
    
    /* Convert to audio output format */
    decoder_format_to_audio_format(&decoder_format, &audio_format);
    
    /* Initialize audio output with configuration */
    printf("Initializing audio output...\n");

#ifdef _WIN32
    WasapiConfig config;
    config.mode = use_exclusive ? WASAPI_MODE_EXCLUSIVE : WASAPI_MODE_SHARED;
    config.buffer_duration_ms = buffer_ms > 0 ? buffer_ms : 0;
    
    audio = audio_output_init(&audio_format, &config);
#else
    (void)use_exclusive;  /* Unused on non-Windows platforms */
    (void)buffer_ms;
    audio = audio_output_init(&audio_format, NULL);
#endif

    if (!audio) {
        fprintf(stderr, "Error: Failed to initialize audio output\n");
        const char* error = audio_output_get_error(audio);
        if (error) {
            fprintf(stderr, "Details: %s\n", error);
        }
        goto cleanup;
    }
    
    /* Get the actual buffer size from the audio output */
    int output_buffer_frames = audio_output_get_available_frames(audio);
    if (output_buffer_frames <= 0) {
        fprintf(stderr, "Error: Failed to get output buffer size\n");
        goto cleanup;
    }
    
    if (use_exclusive) {
        /* Use the device buffer size directly */
        buffer_frames = output_buffer_frames;
    } else {
        buffer_frames = 4096;
    }
    
    printf("Read buffer: %zu frames (%.2f ms)\n", 
           buffer_frames,
           (double)buffer_frames * 1000.0 / decoder_format.sample_rate);
    
    /* Allocate buffer based on calculated size */
    size_t buffer_size = buffer_frames * decoder_format.block_align;
    buffer = (uint8_t*)malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate buffer\n");
        goto cleanup;
    }

#ifdef _WIN32
    if (!VirtualLock(buffer, buffer_size)) {
        printf("Warning: Failed to enable VirtualLock on buffer.\n");
    }

    if (use_exclusive) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }
#endif
    
    /* Start playback */
    printf("Starting playback...\n\n");
    if (audio_output_start(audio) != 0) {
        fprintf(stderr, "Error: Failed to start audio: %s\n", 
                audio_output_get_error(audio));
        goto cleanup;
    }
    
    /* Playback loop - completely codec-agnostic */
    size_t samples_read;
    clock_t last_update = 0;
    uint32_t total_written = 0;
    int consecutive_errors = 0;
    
    while (g_running) {
        /* Read decoded samples */
        samples_read = audio_decoder_read_samples(decoder, buffer, buffer_frames);
        
        if (samples_read == 0) {
            /* End of file */
            break;
        }
        
        if (samples_read == (size_t)-1) {
            fprintf(stderr, "\nError: Failed to read samples\n");
            break;
        }
        
        /* Write samples to audio output */
        size_t samples_written = 0;
        while (samples_written < samples_read && g_running) {
            int frames_to_write = (int)(samples_read - samples_written);
            uint8_t* write_ptr = buffer + (samples_written * decoder_format.block_align);
            
            int written = audio_output_write(audio, write_ptr, frames_to_write);
            
            if (written < 0) {
                fprintf(stderr, "\nError: Failed to write audio: %s\n",
                        audio_output_get_error(audio));
                consecutive_errors++;
                if (consecutive_errors > 10) {
                    fprintf(stderr, "Too many consecutive errors, stopping playback\n");
                    goto cleanup;
                }
                SLEEP_MS(10);
                continue;
            }
            
            consecutive_errors = 0;
            samples_written += written;
            total_written += written;
        }
        clock_t now = clock();
        double elapsed_ms = (double)(now - last_update) * 1000.0 / CLOCKS_PER_SEC;
        
        if (elapsed_ms >= 100.0) {
            display_progress(audio_decoder_tell(decoder), 
                             decoder_format.total_samples, 
                             decoder_format.sample_rate);
            last_update = now;
        }
    }

#ifdef _WIN32
    /* Restore normal thread priority */
    if (use_exclusive) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    }
#endif
    
    /* Wait for buffer to drain */
    if (g_running) {
        printf("\n\nPlayback finished. Draining buffer...\n");
        SLEEP_MS(500);
    }
    
    printf("\n");
    result = 0;
    
cleanup:
    if (audio) {
        audio_output_stop(audio);
        audio_output_close(audio);
    }
    
    if (decoder) {
        audio_decoder_close(decoder);
    }
    
    if (buffer) {
        free(buffer);
    }
    
    return result;
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);

    /* Hide cursor immediately */
    setCursorVisible(0);
    
    printf("========================================\n");
    printf("    Simple Audio Player v1.0\n");
    printf("========================================\n\n");
    
    if (argc < 2) {
        print_usage(argv[0]);
        setCursorVisible(1);
        return 1;
    }
    
    /* Parse command-line arguments */
    const char* filepath = NULL;
    int use_exclusive = 0;
    unsigned int buffer_ms = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            setCursorVisible(1);
            return 0;
        }
#ifdef _WIN32
        else if (strcmp(argv[i], "--exclusive") == 0) {
            use_exclusive = 1;
            printf("Exclusive mode enabled\n");
        }
        else if (strcmp(argv[i], "--buffer") == 0) {
            if (i + 1 < argc) {
                buffer_ms = (unsigned int)atoi(argv[i + 1]);
                if (buffer_ms < 10 || buffer_ms > 5000) {
                    fprintf(stderr, "Error: Buffer size must be between 10 and 5000 ms\n");
                    setCursorVisible(1);
                    return 1;
                }
                printf("Buffer size: %u ms\n", buffer_ms);
                i++; /* Skip next argument */
            } else {
                fprintf(stderr, "Error: --buffer requires a value in milliseconds\n");
                setCursorVisible(1);
                return 1;
            }
        }
#endif
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            setCursorVisible(1);
            return 1;
        }
        else {
            /* This is the file path */
            if (filepath != NULL) {
                fprintf(stderr, "Error: Multiple file paths specified\n");
                setCursorVisible(1);
                return 1;
            }
            filepath = argv[i];
        }
    }
    
    if (filepath == NULL) {
        fprintf(stderr, "Error: No audio file specified\n");
        print_usage(argv[0]);
        setCursorVisible(1);
        return 1;
    }
    
    if (use_exclusive && buffer_ms > 0) {
        fprintf(stderr, "Warning: --buffer option is ignored in exclusive mode\n");
    }
    
    int result = play_audio_file(filepath, use_exclusive, buffer_ms);
    
    if (result == 0) {
        printf("\nPlayback completed successfully!\n");
    } else {
        printf("\nPlayback failed with errors.\n");
    }

    /* Restore cursor */
    setCursorVisible(1);
    
    return result;
}