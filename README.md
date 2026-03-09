# sapfs — Simple Audio Player

A lightweight, cross-platform command-line audio player written in C. Plays WAV, FLAC, and MP3 files with a real-time progress display, pause/resume support, and loop mode.

---

## Features

- Plays **WAV**, **FLAC**, and **MP3** audio files
- Real-time **progress bar** with timestamp display
- **Pause / Resume** with `Space` or `P`
- **Loop mode** to repeat playback indefinitely
- Windows **WASAPI** audio output:
  - Shared mode (default)
  - Exclusive mode for bit-perfect, low-latency playback
- macOS support
- Configurable **buffer size** (Windows shared mode only)
- Internal **32-bit float pipeline** — all formats are decoded to `float` before output
- TPDF **dithering** when converting float → 16-bit PCM

---

## Requirements

### Build

| Platform | Toolchain |
|----------|-----------|
| **Windows** | MSVC (`cl.exe`) — run from a Visual Studio Developer Command Prompt |
| **macOS** | Xcode Command Line Tools (`clang`) — `xcode-select --install` |
| **Linux** | Not yet supported (output backend stub exists) |

### Runtime Dependencies

| Format | Library | Windows DLL | macOS dylib |
|--------|---------|-------------|-------------|
| FLAC   | [libFLAC](https://xiph.org/flac/) | `libFLAC.dll` | `libFLAC.dylib` |
| MP3    | [libmpg123](https://www.mpg123.de/) | `libmpg123-0.dll` | `libmpg123.dylib` |
| WAV    | Built-in | — | — |

All libraries are loaded at **runtime** via `dlopen`/`LoadLibrary` — they are not required at build time.

---

## Building

### Windows

From a Visual Studio Developer Command Prompt:

```bat
build.bat
```

The compiled binary is placed in `dist/sapfs.exe`.

### macOS

```bash
chmod +x build_macos.sh
./build_macos.sh
```

The compiled binary is placed in `dist/sapfs`.

#### Installing runtime dependencies on macOS

```bash
# Homebrew (recommended)
brew install flac mpg123
```

After installing, make the `.dylib` files discoverable at runtime:

- Bash
```bash
# Apple Silicon (Homebrew installs to /opt/homebrew)
export DYLD_LIBRARY_PATH="/opt/homebrew/lib:$DYLD_LIBRARY_PATH"

# Intel Mac (Homebrew installs to /usr/local)
export DYLD_LIBRARY_PATH="/usr/local/lib:$DYLD_LIBRARY_PATH"
```

- NuShell
```
$env.DYLD_LIBRARY_PATH = "/opt/homebrew/lib"

$env.DYLD_LIBRARY_PATH = "/usr/local/lib"
```

Alternatively, copy or symlink the `.dylib` files into the same directory as the `sapfs` binary

---

## Usage

```
sapfs [OPTIONS] <audio_file>
```

### Options

| Option | Description | Platform |
|--------|-------------|----------|
| `--exclusive` | Use WASAPI exclusive mode (bit-perfect) | Windows only |
| `--buffer <ms>` | Set audio buffer size in milliseconds, 10–5000 | Windows shared mode only |
| `--loop` | Loop playback indefinitely | All |
| `-h`, `--help` | Show help message | All |

### Controls

| Key | Action |
|-----|--------|
| `Space` or `P` | Pause / Resume |
| `Ctrl+C` | Stop and exit |

### Examples

```bat
# Basic playback
sapfs song.wav

# Play FLAC in exclusive mode
sapfs --exclusive album.flac

# Loop an MP3 with a 200ms buffer
sapfs --loop --buffer 200 track.mp3
```

---

## Architecture

```
main.c
├── audio_decoder/
│   ├── audio_decoder.c       # Decoder abstraction (format detection, float pipeline)
│   ├── wave_parser.c         # Native WAV/RIFF parser (PCM, WAVE_FORMAT_EXTENSIBLE)
│   ├── flac_parser.c         # libFLAC wrapper (dynamic loading)
│   └── mp3_parser.c          # libmpg123 wrapper (dynamic loading)
├── audio_output/
│   ├── audio_output.c        # Output abstraction (platform dispatch)
│   ├── wasapi_output.c       # Windows WASAPI backend (shared + exclusive)
│   ├── coreaudio_output.c    # macOS CoreAudio AudioUnit backend
│   └── ring_buffer.c         # Thread-safe ring buffer (producer/consumer)
└── audio_converter.c         # PCM format converters (8/16/24/32-bit ↔ float)
```

### Signal Flow

```
File → [Decoder] → float PCM → [Ring Buffer] → [WASAPI Consumer Thread] → [Device]
```

- **Windows (WASAPI)**: A dedicated consumer thread (`_beginthreadex`) drains the ring buffer on `WaitForSingleObject`, registered under MMCSS `Pro Audio` priority.
- **macOS (CoreAudio)**: The AudioUnit engine drives a pull-model render callback on its own real-time thread. The callback drains the ring buffer each time the engine needs a block of samples. Underruns are zero-padded to avoid noise.

The ring buffer implementation is fully cross-platform, using `CRITICAL_SECTION` / `CONDITION_VARIABLE` on Windows and `pthread_mutex_t` / `pthread_cond_t` on POSIX platforms.

---

## Supported WAV Variants

- 8-bit unsigned PCM
- 16-bit signed PCM
- 24-bit packed (3 bytes/sample)
- 24-bit padded in 32-bit container (`WAVE_FORMAT_EXTENSIBLE`)
- 32-bit signed PCM
- 32-bit IEEE float PCM
- Multi-channel layouts (up to 7.1 surround)
