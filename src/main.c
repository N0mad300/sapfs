#include "audio_decoder/audio_decoder.h"
#include "audio_output/audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

/* For cross-platform sleep */
#ifdef _WIN32
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
    printf("Usage: %s <audio_file>\n", program_name);
    printf("\nSimple audio player that plays various audio formats.\n");
    printf("\nControls:\n");
    printf("  Ctrl+C - Stop playback and exit\n");
    printf("\nSupported formats:\n");
    printf("  - WAVE (.wav) - PCM uncompressed audio\n");
    printf("  - MP3 (.mp3) - Coming soon\n");
    printf("  - FLAC (.flac) - Coming soon\n");
    printf("\nAudio specifications:\n");
    printf("  - 8, 16, 24, or 32-bit samples\n");
    printf("  - Mono or multi-channel audio\n");
    printf("  - Sample rates from 8000 Hz to 192000 Hz\n");
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
int play_audio_file(const char* filepath) {
    AudioDecoder* decoder = NULL;
    AudioOutput* audio = NULL;
    AudioDecoderFormat decoder_format;
    AudioFormat audio_format;
    int result = -1;
    
    /* Buffer for reading samples */
    #define BUFFER_FRAMES 1024
    uint8_t* buffer = NULL;
    
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
    
    /* Initialize audio output */
    printf("Initializing audio output...\n");
    audio = audio_output_init(&audio_format);
    if (!audio) {
        fprintf(stderr, "Error: Failed to initialize audio output\n");
        goto cleanup;
    }
    
    /* Allocate buffer */
    size_t buffer_size = BUFFER_FRAMES * decoder_format.block_align;
    buffer = (uint8_t*)malloc(buffer_size);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate buffer\n");
        goto cleanup;
    }
    
    /* Start playback */
    printf("Starting playback...\n\n");
    if (audio_output_start(audio) != 0) {
        fprintf(stderr, "Error: Failed to start audio: %s\n", 
                audio_output_get_error(audio));
        goto cleanup;
    }
    
    /* Playback loop - completely codec-agnostic */
    size_t samples_read;
    uint32_t total_written = 0;
    int consecutive_errors = 0;
    
    while (g_running) {
        /* Read decoded samples */
        samples_read = audio_decoder_read_samples(decoder, buffer, BUFFER_FRAMES);
        
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
            
            if (written == 0) {
                SLEEP_MS(10);
            }
        }
        
        /* Update progress display */
        display_progress(audio_decoder_tell(decoder), decoder_format.total_samples, 
                        decoder_format.sample_rate);
    }
    
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
        return 1;
    }
    
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    
    int result = play_audio_file(argv[1]);
    
    if (result == 0) {
        printf("\nPlayback completed successfully!\n");
    } else {
        printf("\nPlayback failed with errors.\n");
    }

    /* Restore cursor */
    setCursorVisible(1);
    
    return result;
}