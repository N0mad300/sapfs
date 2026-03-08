#include "audio_decoder/audio_decoder.h"
#include "audio_output/audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* Platform-specific */
#ifdef _WIN32
    #include "audio_output/wasapi_output.h"
    #include <windows.h>
    #include <conio.h>
    #define SLEEP_MS(ms) Sleep(ms)

    void setup_terminal_input() {}
    void restore_terminal_input() {}
    
    int check_keypress() {
        if (_kbhit()) {
            return _getch();
        }
        return -1;
    }
#else
    #include <unistd.h>
    #include <termios.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #define SLEEP_MS(ms) usleep((ms) * 1000)

    static struct termios orig_termios;
    static int terminal_configured = 0;

    void restore_terminal_input() {
        if (terminal_configured) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
            terminal_configured = 0;
        }
    }

    void setup_terminal_input() {
        if (!isatty(STDIN_FILENO)) return;
        
        tcgetattr(STDIN_FILENO, &orig_termios);
        atexit(restore_terminal_input);
        
        struct termios raw = orig_termios;
        raw.c_lflag &= ~(ICANON | ECHO); /* Disable line buffering and echo */
        raw.c_cc[VMIN] = 0;              /* Non-blocking read */
        raw.c_cc[VTIME] = 0;
        
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        terminal_configured = 1;
    }

    int check_keypress() {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            return c;
        }
        return -1;
    }
#endif

static volatile int g_running = 1;

void setCursorVisible(int visible) {
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

    if (g_running) {
        printf("\n\nReceived interrupt signal. Stopping playback...\n");
        setCursorVisible(1);
        restore_terminal_input();
        g_running = 0;
    }
}

void print_usage(const char* program_name) {
    printf("Usage: %s [OPTIONS] <audio_file>\n", program_name);
    printf("\nSimple audio player that plays various audio formats.\n");
    printf("\nOptions:\n");
#ifdef _WIN32
    printf("  --exclusive      Use WASAPI exclusive mode (bit-perfect)\n");
    printf("  --buffer <ms>    Set buffer size in ms (shared mode only)\n");
#endif
    printf("  --loop           Loop playback indefinitely\n");
    printf("  -h, --help       Show this help message\n");
    printf("\nControls:\n");
    printf("  Space / P        Pause/Resume playback\n");
    printf("  Ctrl+C           Stop playback and exit\n");
    printf("\nSupported formats:\n");
    printf("  - WAVE (.wav)\n");
    printf("  - FLAC (.flac)\n");
    printf("  - MP3  (.mp3)\n");
}

/* Convert decoder format to output format */
void decoder_format_to_audio_format(const AudioDecoderFormat* dec_fmt, AudioFormat* audio_fmt) {
    audio_fmt->sample_rate = dec_fmt->sample_rate;
    audio_fmt->num_channels = dec_fmt->num_channels;
    audio_fmt->bits_per_sample = dec_fmt->bits_per_sample;
    audio_fmt->block_align = dec_fmt->block_align;
    audio_fmt->valid_bits_per_sample = dec_fmt->valid_bits_per_sample;
    audio_fmt->channel_mask = dec_fmt->channel_mask;
}

void display_progress(uint32_t current_sample, uint32_t total_samples, uint32_t sample_rate, int paused) {
    double current_time = (double)current_sample / sample_rate;
    double total_time = (double)total_samples / sample_rate;
    double percentage = total_samples > 0 ? ((double)current_sample / total_samples) * 100.0 : 0.0;
    
    int current_min = (int)current_time / 60;
    int current_sec = (int)current_time % 60;
    int total_min = (int)total_time / 60;
    int total_sec = (int)total_time % 60;
    
    /* Print progress bar */
    printf("\r%s[", paused ? "[PAUSED] " : "");
    int bar_width = paused ? 31 : 40;
    int filled = (int)(percentage / 100.0 * bar_width);

    for (int i = 0; i < bar_width; i++) {
        if (i < filled) printf("=");
        else if (i == filled) printf(">");
        else printf(" ");
    }

    printf("] %3.0f%% [%d:%02d / %d:%02d]", 
           percentage, current_min, current_sec, total_min, total_sec);
    fflush(stdout);
}

int play_audio_file(const char* filepath, int use_exclusive, unsigned int buffer_ms, int loop_mode) {
    AudioDecoder* decoder = NULL;
    AudioOutput* audio = NULL;
    AudioDecoderFormat decoder_format;
    AudioFormat audio_format;
    int result = -1;
    
    uint8_t* buffer = NULL;
    size_t buffer_frames = 0;
    
    printf("Opening file: %s\n", filepath);
    decoder = audio_decoder_open(filepath);
    if (!decoder) {
        fprintf(stderr, "Error: Failed to open audio file\n");
        return -1;
    }
    
    if (audio_decoder_get_format(decoder, &decoder_format) != 0) {
        fprintf(stderr, "Error: Failed to get format: %s\n", audio_decoder_get_error(decoder));
        goto cleanup;
    }
    
    printf("Codec: %s | %u Hz | %u Channels | %u bits\n", 
           decoder_format.codec_name, decoder_format.sample_rate, 
           decoder_format.num_channels, decoder_format.bits_per_sample);
    if (loop_mode) printf("Loop mode: ENABLED\n");
    
    decoder_format_to_audio_format(&decoder_format, &audio_format);

    /* Init Audio Output */
#ifdef _WIN32
    WasapiConfig config;
    config.mode = use_exclusive ? WASAPI_MODE_EXCLUSIVE : WASAPI_MODE_SHARED;
    if (use_exclusive) printf("Exclusive mode: ENABLED\n");
    config.buffer_duration_ms = buffer_ms > 0 ? buffer_ms : 0;
    audio = audio_output_init(&audio_format, &config);
#else
    audio = audio_output_init(&audio_format, NULL);
#endif

    if (!audio) {
        fprintf(stderr, "Error: Failed to initialize audio output: %s\n", audio_output_get_error(audio));
        goto cleanup;
    }
    
    /* Buffer Size: 100ms chunk for reading */
    buffer_frames = decoder_format.sample_rate / 10;
    size_t buffer_size = buffer_frames * decoder_format.block_align;
    buffer = (uint8_t*)malloc(buffer_size);
    if (!buffer) goto cleanup;
    
    if (audio_output_start(audio) != 0) {
        fprintf(stderr, "Error: Failed to start audio: %s\n", audio_output_get_error(audio));
        goto cleanup;
    }

    printf("\n");
    
    size_t samples_read = 0;
    clock_t last_update = 0;
    int is_draining = 0;
    int is_paused = 0;

    uint32_t max_buffer_frames = decoder_format.sample_rate * 2;
    
    while (g_running) {
        int key = check_keypress();
        if (key == 'p' || key == 'P' || key == ' ') {
            is_paused = !is_paused;
            
            if (is_paused) {
                audio_output_pause(audio);
                last_update = 0; 
            } else {
                audio_output_resume(audio);
                last_update = 0;
            }
        }

        if (is_paused) {
            clock_t now = clock();
            if ((double)(now - last_update) * 1000.0 / CLOCKS_PER_SEC >= 200.0) {

                uint32_t decoder_pos = audio_decoder_tell(decoder);
                int free_frames = audio_output_get_available_frames(audio);
                uint32_t queued_frames = 0;
                if (free_frames >= 0 && (uint32_t)free_frames <= max_buffer_frames) {
                    queued_frames = max_buffer_frames - (uint32_t)free_frames;
                }
                
                uint32_t actual_pos = (decoder_pos > queued_frames) ? decoder_pos - queued_frames : 0;
                display_progress(actual_pos, decoder_format.total_samples, decoder_format.sample_rate, 1);
                last_update = now;
            }
            SLEEP_MS(50);
            continue; /* Skip reading/writing */
        }

        if (!is_draining) {
            samples_read = audio_decoder_read_samples(decoder, buffer, buffer_frames);
            
            if (samples_read == (size_t)-1) {
                const char* err = audio_decoder_get_error(decoder);
                fprintf(stderr, "\nError reading samples: %s\n", err ? err : "unknown");
                break;
            }
            
            if (samples_read == 0) {
                is_draining = 1;
            } else {
                if (audio_output_write(audio, buffer, samples_read) < 0) break;
            }
        }

        if (is_draining) {
            int free_frames = audio_output_get_available_frames(audio);
            
            if (free_frames >= 0 && (uint32_t)free_frames >= (max_buffer_frames - 1000)) {
                if (loop_mode && g_running) {
                    /* LOOP: Seek to start and reset draining flag */
                    if (audio_decoder_seek(decoder, 0) == 0) {
                        is_draining = 0;
                        last_update = 0; 
                        continue;
                    } else {
                        fprintf(stderr, "\nError: Seek failed during loop.\n");
                        break;
                    }
                } else {
                    /* NO LOOP: Finish */
                    display_progress(decoder_format.total_samples, 
                                     decoder_format.total_samples, 
                                     decoder_format.sample_rate, 0);
                    break;
                }
            }

            SLEEP_MS(20);
        }

        clock_t now = clock();
        if ((double)(now - last_update) * 1000.0 / CLOCKS_PER_SEC >= 100.0) {
            uint32_t decoder_pos = audio_decoder_tell(decoder);

            int free_frames = audio_output_get_available_frames(audio);
            uint32_t queued_frames = 0;
            if (free_frames >= 0 && (uint32_t)free_frames <= max_buffer_frames) {
                queued_frames = max_buffer_frames - (uint32_t)free_frames;
            }

            uint32_t actual_pos = 0;
            if (is_draining) {
                 if (decoder_format.total_samples > queued_frames)
                    actual_pos = decoder_format.total_samples - queued_frames;
            } else {
                 if (decoder_pos > queued_frames)
                    actual_pos = decoder_pos - queued_frames;
            }

            display_progress(actual_pos, decoder_format.total_samples, decoder_format.sample_rate, 0);
            last_update = now;
        }
    }
    
    printf("\n");
    result = 0;
    
cleanup:
    if (audio) {
        audio_output_stop(audio);
        audio_output_close(audio);
    }
    
    if (decoder) audio_decoder_close(decoder);
    if (buffer) free(buffer);
    
    return result;
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    setup_terminal_input();
    setCursorVisible(0);
    
    printf("========================================\n");
    printf("    Simple Audio Player\n");
    printf("========================================\n");
    
    if (argc < 2) {
        print_usage(argv[0]);
        restore_terminal_input();
        setCursorVisible(1);
        return 1;
    }
    
    /* Parse command-line arguments */
    const char* filepath = NULL;
    int use_exclusive = 0;
    int loop_mode = 0;
    unsigned int buffer_ms = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            restore_terminal_input();
            setCursorVisible(1);
            return 0;
        }
#ifdef _WIN32
        else if (strcmp(argv[i], "--exclusive") == 0) {
            use_exclusive = 1;
        }
        else if (strcmp(argv[i], "--buffer") == 0) {
            if (i + 1 < argc) {
                buffer_ms = (unsigned int)atoi(argv[i + 1]);
                
                if (buffer_ms < 10 || buffer_ms > 5000) {
                    fprintf(stderr, "Error: Buffer size must be between 10 and 5000 ms\n");
                    restore_terminal_input();
                    setCursorVisible(1);
                    return 1;
                }
                
                printf("Buffer size: %u ms\n", buffer_ms);
                i++; /* Skip next argument (the number value) */
            } else {
                fprintf(stderr, "Error: --buffer requires a value in milliseconds\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
        }
#endif
        else if (strcmp(argv[i], "--loop") == 0) {
            loop_mode = 1;
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            restore_terminal_input();
            setCursorVisible(1);
            return 1;
        }
        else {
            if (filepath != NULL) {
                fprintf(stderr, "Error: Multiple file paths specified\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
            filepath = argv[i];
        }
    }
    
    if (!filepath) {
        fprintf(stderr, "Error: No audio file specified\n");
        restore_terminal_input();
        setCursorVisible(1);
        return 1;
    }
    
    if (use_exclusive && buffer_ms > 0) {
        fprintf(stderr, "Warning: --buffer option is ignored in exclusive mode\n");
    }
    
    int result = play_audio_file(filepath, use_exclusive, buffer_ms, loop_mode);
    
    setCursorVisible(1);
    
    return result;
}