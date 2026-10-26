#include "audio_decoder/audio_decoder.h"
#include "audio_output/audio_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>

/* Platform-specific */
#ifdef _WIN32
    #include "audio_output/windows_output.h"
    #include "audio_output/wasapi_output.h"
    #include "audio_output/asio_output.h"
    #include <windows.h>
    #include <conio.h>
    #define SLEEP_MS(ms) Sleep(ms)

    void setup_terminal_input(void) {}
    void restore_terminal_input(void) {}
    
    int check_keypress(void) {
        if (_kbhit()) {
            return _getch();
        }
        return -1;
    }
#else
#ifdef __APPLE__
    #include "audio_output/coreaudio_output.h"
#elif defined(__linux__)
    #include "audio_output/linux_output.h"
    #include "audio_output/alsa_output.h"
    #include "audio_output/pipewire_output.h"
#endif
    #include <unistd.h>
    #include <termios.h>
    #include <fcntl.h>
    #include <sys/select.h>
    #define SLEEP_MS(ms) usleep((ms) * 1000)

    static struct termios orig_termios;
    static int terminal_configured = 0;

    void restore_terminal_input(void) {
        if (terminal_configured) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
            terminal_configured = 0;
        }
    }

    void setup_terminal_input(void) {
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

    int check_keypress(void) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            return (int)c;
        }
        return -1;
    }
#endif

static volatile int g_running = 1;

static uint64_t monotonic_ms(void) {
#ifdef _WIN32
    return (uint64_t)GetTickCount64();
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

#ifdef _WIN32
static void print_asio_driver_name(const char* driver_name, void* user_data) {
    int* count = (int*)user_data;
    printf("  %s\n", driver_name);
    if (count) (*count)++;
}
#endif

#if defined(__linux__) && defined(SAPFS_HAVE_PIPEWIRE)
static void print_pipewire_device(const char* name, const char* description,
                                  const char* serial, void* user_data) {
    int* count = user_data;
    printf("  %s\n    %s (serial %s)\n", name, description, serial);
    ++*count;
}
#endif

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
    printf("  --asio           Use ASIO audio output\n");
    printf("  --output <name>  Select Windows backend: wasapi or asio\n");
    printf("  --asio-driver <name>  Use a specific ASIO driver\n");
    printf("  --asio-output <n>     First ASIO output channel, zero-based\n");
    printf("  --list-asio-drivers   List installed ASIO drivers\n");
#endif
#ifdef __APPLE__
    printf(" --exclusive       Use macOS Core Audio hog mode (bit-perfect)\n");
#endif
#ifdef __linux__
    printf("  --output <name>  Linux backend: alsa (default) or pipewire\n");
    printf("  --device <name>  ALSA PCM or PipeWire sink node.name/object.serial\n");
    printf("  --exclusive      ALSA hardware / PipeWire exclusive linking, no client resampling\n");
    printf("  --shared         ALSA plugins / PipeWire mixed playback\n");
    printf("                   Defaults: ALSA exclusive, PipeWire shared\n");
    printf("  --buffer <ms>    ALSA hardware / PipeWire application queue, 10-5000 (default: 100)\n");
    printf("  --pipewire-remote <name>  Select a PipeWire server\n");
    printf("  --list-pipewire-devices  List PipeWire playback sinks and exit\n");
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
    audio_fmt->is_float = dec_fmt->is_float;
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

int play_audio_file(const char* filepath,
                    int use_exclusive,
                    unsigned int buffer_ms,
                    int loop_mode,
                    int use_asio,
                    const char* asio_driver_name,
                    long asio_output_offset,
                    const char* output_device,
                    const char* linux_backend,
                    const char* pipewire_remote) {
    AudioDecoder* decoder = NULL;
    AudioOutput* audio = NULL;
    AudioDecoderFormat decoder_format;
    AudioFormat audio_format;
    int result = -1;
    
    uint8_t* buffer = NULL;
    size_t buffer_frames = 0;
    
    printf("Opening file: %s\n", filepath);
#ifdef __linux__
    decoder = audio_decoder_open_native(filepath);
#else
    (void)output_device;
    (void)linux_backend;
    (void)pipewire_remote;
    decoder = audio_decoder_open(filepath);
#endif
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
    if (use_asio) {
        AsioConfig asio_config;
        WindowsAudioConfig windows_config;

        memset(&asio_config, 0, sizeof(asio_config));
        asio_config.driver_name = asio_driver_name;
        asio_config.output_channel_offset = asio_output_offset;

        windows_config.backend = WINDOWS_AUDIO_BACKEND_ASIO;
        windows_config.backend_config = &asio_config;

        printf("ASIO output: ENABLED\n");
        if (asio_driver_name) printf("ASIO driver: %s\n", asio_driver_name);
        if (asio_output_offset > 0) printf("ASIO output offset: %ld\n", asio_output_offset);

        audio = audio_output_init(&audio_format, &windows_config);
    } else {
        WasapiConfig wasapi_config;
        WindowsAudioConfig windows_config;

        wasapi_config.mode = use_exclusive ? WASAPI_MODE_EXCLUSIVE : WASAPI_MODE_SHARED;
        if (use_exclusive) printf("WASAPI exclusive mode: ENABLED\n");
        wasapi_config.buffer_duration_ms = buffer_ms > 0 ? buffer_ms : 0;

        windows_config.backend = WINDOWS_AUDIO_BACKEND_WASAPI;
        windows_config.backend_config = &wasapi_config;

        audio = audio_output_init(&audio_format, &windows_config);
    }
#elif defined(__APPLE__)
    (void)buffer_ms;
    (void)use_asio;
    (void)asio_driver_name;
    (void)asio_output_offset;
    {
        CoreAudioConfig config;
        config.mode = use_exclusive ? COREAUDIO_MODE_HOG : COREAUDIO_MODE_SHARED;
        if (use_exclusive)
            printf("CoreAudio hog mode: ENABLED\n");
        audio = audio_output_init(&audio_format, &config);
    }
#elif defined(__linux__)
    (void)use_asio;
    (void)asio_driver_name;
    (void)asio_output_offset;
    {
        AlsaConfig alsa_config = {0};
        LinuxAudioConfig linux_config;
        alsa_config.mode = use_exclusive ? ALSA_MODE_EXCLUSIVE : ALSA_MODE_SHARED;
        alsa_config.device = output_device;
        alsa_config.buffer_duration_ms = buffer_ms;
        linux_config.backend = LINUX_AUDIO_BACKEND_ALSA;
        linux_config.backend_config = &alsa_config;
        PipewireConfig pipewire_config = {0};
        if (strcmp(linux_backend, "pipewire") == 0) {
            pipewire_config.mode = use_exclusive ? PIPEWIRE_MODE_EXCLUSIVE : PIPEWIRE_MODE_SHARED;
            pipewire_config.target = output_device;
            pipewire_config.remote_name = pipewire_remote;
            pipewire_config.stream_name = filepath;
            pipewire_config.buffer_duration_ms = buffer_ms;
            linux_config.backend = LINUX_AUDIO_BACKEND_PIPEWIRE;
            linux_config.backend_config = &pipewire_config;
            printf("PipeWire %s output: %s\n", use_exclusive ? "exclusive (server/device bit-perfect path unverified)" : "shared",
                   output_device ? output_device : "default sink");
        } else {
            printf("ALSA %s output: %s\n", use_exclusive ? "exclusive" : "plugin",
                   output_device ? output_device : use_exclusive ? "hw:0,0" : "default");
        }
        audio = audio_output_init(&audio_format, &linux_config);
    }
#else
    (void)use_exclusive;
    (void)buffer_ms;
    (void)use_asio;
    (void)asio_driver_name;
    (void)asio_output_offset;
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
    size_t pending_offset = 0;
    uint64_t last_update = 0;
    int is_draining = 0;
    int is_paused = 0;

    int playback_failed = 0;
    
    while (g_running) {
        int key = check_keypress();
        if (key == 'p' || key == 'P' || key == ' ') {
            int status = is_paused ? audio_output_resume(audio) : audio_output_pause(audio);
            if (status == 0) {
                is_paused = !is_paused;
                last_update = 0;
            } else {
                const char* err = audio_output_get_error(audio);
                fprintf(stderr, "\nError changing playback state: %s\n", err ? err : "unknown");
            }
        }

        if (is_paused) {
            uint64_t now = monotonic_ms();
            if (now - last_update >= 200) {

                uint32_t decoder_pos = audio_decoder_tell(decoder);
                int queued = audio_output_get_queued_frames(audio);
                uint32_t queued_frames = (queued > 0 ? (uint32_t)queued : 0)
                                       + (uint32_t)(samples_read - pending_offset);
                
                uint32_t actual_pos = (decoder_pos > queued_frames) ? decoder_pos - queued_frames : 0;
                display_progress(actual_pos, decoder_format.total_samples, decoder_format.sample_rate, 1);
                last_update = now;
            }
            SLEEP_MS(50);
            continue; /* Skip reading/writing */
        }

        if (!is_draining && pending_offset == samples_read) {
            samples_read = audio_decoder_read_samples(decoder, buffer, buffer_frames);
            pending_offset = 0;
            
            if (samples_read == (size_t)-1) {
                const char* err = audio_decoder_get_error(decoder);
                fprintf(stderr, "\nError reading samples: %s\n", err ? err : "unknown");
                playback_failed = 1;
                break;
            }
            
            if (samples_read == 0) {
                is_draining = 1;
            }
        }

        if (pending_offset < samples_read) {
            int written = audio_output_write(audio,
                buffer + pending_offset * decoder_format.block_align, samples_read - pending_offset);
            if (written < 0) {
                const char* err = audio_output_get_error(audio);
                fprintf(stderr, "\nError writing audio: %s\n", err ? err : "unknown");
                playback_failed = 1;
                break;
            }
            pending_offset += (size_t)written;
            if (written == 0) SLEEP_MS(5);
        }

        if (is_draining) {
            int drained = audio_output_drain(audio);
            if (drained < 0) {
                const char* err = audio_output_get_error(audio);
                fprintf(stderr, "\nError draining audio: %s\n", err ? err : "unknown");
                playback_failed = 1;
                break;
            }
            
            if (drained) {
                if (loop_mode && g_running) {
                    /* LOOP: Seek to start and reset draining flag */
                    if (audio_decoder_seek(decoder, 0) == 0) {
                        is_draining = 0;
                        last_update = 0; 
                        continue;
                    } else {
                        fprintf(stderr, "\nError: Seek failed during loop.\n");
                        playback_failed = 1;
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

        uint64_t now = monotonic_ms();
        if (now - last_update >= 100) {
            uint32_t decoder_pos = audio_decoder_tell(decoder);

            int queued = audio_output_get_queued_frames(audio);
            uint32_t queued_frames = (queued > 0 ? (uint32_t)queued : 0)
                                   + (uint32_t)(samples_read - pending_offset);

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
    result = playback_failed ? -1 : 0;
    
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
#ifdef __linux__
    int use_exclusive = -1; /* Backend-specific default, applied after parsing. */
#else
    int use_exclusive = 0;
#endif
    int loop_mode = 0;
    unsigned int buffer_ms = 0;
    int use_asio = 0;
#ifdef _WIN32
    int list_asio_drivers = 0;
#endif
    const char* asio_driver_name = NULL;
    long asio_output_offset = 0;
    const char* output_device = NULL;
    const char* linux_backend = "alsa";
    const char* pipewire_remote = NULL;
#ifdef __linux__
    int list_pipewire_devices = 0;
#endif
    
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
        else if (strcmp(argv[i], "--asio") == 0) {
            use_asio = 1;
        }
        else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "--backend") == 0) {
            if (i + 1 < argc) {
                if (strcmp(argv[i + 1], "asio") == 0) {
                    use_asio = 1;
                } else if (strcmp(argv[i + 1], "wasapi") == 0) {
                    use_asio = 0;
                } else {
                    fprintf(stderr, "Error: --output must be 'wasapi' or 'asio'\n");
                    restore_terminal_input();
                    setCursorVisible(1);
                    return 1;
                }
                i++;
            } else {
                fprintf(stderr, "Error: --output requires a backend name\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--asio-driver") == 0) {
            if (i + 1 < argc) {
                asio_driver_name = argv[++i];
                use_asio = 1;
            } else {
                fprintf(stderr, "Error: --asio-driver requires a driver name\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--asio-output") == 0) {
            if (i + 1 < argc) {
                asio_output_offset = atol(argv[++i]);
                if (asio_output_offset < 0) {
                    fprintf(stderr, "Error: --asio-output must be zero or greater\n");
                    restore_terminal_input();
                    setCursorVisible(1);
                    return 1;
                }
                use_asio = 1;
            } else {
                fprintf(stderr, "Error: --asio-output requires a channel index\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
        }
        else if (strcmp(argv[i], "--list-asio-drivers") == 0) {
            list_asio_drivers = 1;
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
#elif defined(__APPLE__)
        else if (strcmp(argv[i], "--exclusive") == 0) {
            use_exclusive = 1;
        }
#elif defined(__linux__)
        else if (strcmp(argv[i], "--exclusive") == 0) {
            use_exclusive = 1;
        }
        else if (strcmp(argv[i], "--shared") == 0) {
            use_exclusive = 0;
        }
        else if (strcmp(argv[i], "--device") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-' || !argv[i + 1][0]) {
                fprintf(stderr, "Error: --device requires an ALSA PCM or PipeWire sink name\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
            output_device = argv[++i];
        }
        else if (strcmp(argv[i], "--output") == 0 || strcmp(argv[i], "--backend") == 0) {
            if (i + 1 >= argc || (strcmp(argv[i + 1], "alsa") != 0 && strcmp(argv[i + 1], "pipewire") != 0)) {
                fprintf(stderr, "Error: --output must be 'alsa' or 'pipewire'\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
            linux_backend = argv[++i];
        }
        else if (strcmp(argv[i], "--list-pipewire-devices") == 0) {
            list_pipewire_devices = 1;
        }
        else if (strcmp(argv[i], "--pipewire-remote") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '-' || !argv[i + 1][0]) {
                fprintf(stderr, "Error: --pipewire-remote requires a server name\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
            pipewire_remote = argv[++i];
        }
        else if (strcmp(argv[i], "--buffer") == 0) {
            char* end = NULL;
            unsigned long value = 0;
            errno = 0;
            if (i + 1 < argc) value = strtoul(argv[++i], &end, 10);
            if (!end || end == argv[i] || *end || errno || value < 10 || value > 5000) {
                fprintf(stderr, "Error: --buffer requires an integer between 10 and 5000 ms\n");
                restore_terminal_input();
                setCursorVisible(1);
                return 1;
            }
            buffer_ms = (unsigned int)value;
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

#ifdef _WIN32
    if (list_asio_drivers) {
        int count = 0;
        printf("Installed ASIO drivers:\n");
        asio_output_list_drivers(print_asio_driver_name, &count);
        if (count == 0) printf("  (none found)\n");
        restore_terminal_input();
        setCursorVisible(1);
        return 0;
    }
#endif
    
#ifdef __linux__
    if (use_exclusive < 0) use_exclusive = strcmp(linux_backend, "alsa") == 0;
    if (pipewire_remote && !list_pipewire_devices && strcmp(linux_backend, "pipewire") != 0) {
        fprintf(stderr, "Error: --pipewire-remote requires --output pipewire\n");
        restore_terminal_input();
        setCursorVisible(1);
        return 1;
    }
    /* libpipewire gives PIPEWIRE_REMOTE precedence over remote.name. Make an
     * explicit CLI choice override the inherited environment for this process. */
    if (pipewire_remote && setenv("PIPEWIRE_REMOTE", pipewire_remote, 1) != 0) {
        perror("Error selecting PipeWire server");
        restore_terminal_input();
        setCursorVisible(1);
        return 1;
    }
    if (list_pipewire_devices) {
        int status = -1;
#ifdef SAPFS_HAVE_PIPEWIRE
        int count = 0;
        printf("PipeWire playback sinks:\n");
        status = pipewire_output_list_devices(pipewire_remote, print_pipewire_device, &count);
        if (status < 0) fprintf(stderr, "Error: %s\n", pipewire_output_get_last_error());
        else if (!count) printf("  (none found)\n");
#else
        fprintf(stderr, "Error: PipeWire support was not built; rebuild with WITH_PIPEWIRE=1\n");
#endif
        restore_terminal_input();
        setCursorVisible(1);
        return status < 0 ? 1 : 0;
    }
#endif

    if (!filepath) {
        fprintf(stderr, "Error: No audio file specified\n");
        restore_terminal_input();
        setCursorVisible(1);
        return 1;
    }
    
#ifdef _WIN32
    if (use_asio) {
        if (use_exclusive) {
            fprintf(stderr, "Warning: --exclusive option is ignored with ASIO output\n");
        }
        if (buffer_ms > 0) {
            fprintf(stderr, "Warning: --buffer option is ignored with ASIO output\n");
        }
    } else if (use_exclusive && buffer_ms > 0) {
        fprintf(stderr, "Warning: --buffer option is ignored in exclusive mode\n");
    }
#endif
    
    int result = play_audio_file(filepath,
                                 use_exclusive,
                                 buffer_ms,
                                 loop_mode,
                                 use_asio,
                                 asio_driver_name,
                                 asio_output_offset,
                                 output_device,
                                 linux_backend,
                                 pipewire_remote);
    
    setCursorVisible(1);
    
    return result;
}
