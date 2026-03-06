# sapfs — Simple Audio Player

A lightweight, cross-platform command-line audio player written in C. Plays WAV, FLAC, and MP3 files with a real-time progress display, pause/resume support, and loop mode. On Windows, audio output is handled via WASAPI in either shared or bit-perfect exclusive mode.

---

## Features

- Plays **WAV**, **FLAC**, and **MP3** audio files
- Real-time **progress bar** with timestamp display
- **Pause / Resume** with `Space` or `P`
- **Loop mode** to repeat playback indefinitely
- Windows **WASAPI** audio output:
  - Shared mode (default)
  - Exclusive mode for bit-perfect, low-latency playback
- Configurable **buffer size** (shared mode)
- Internal **32-bit float pipeline** — all formats are decoded to `float` before output
- TPDF **dithering** when converting float → 16-bit PCM

---

## Requirements

### Build

- **Windows**: MSVC (`cl.exe`) — run from a Visual Studio Developer Command Prompt
- **Linux / macOS**: Not yet supported (output backend stubs exist)

### Runtime Dependencies

| Format | Library | DLL (Windows) |
|--------|---------|----------------|
| FLAC   | [libFLAC](https://xiph.org/flac/) | `libFLAC.dll` |
| MP3    | [libmpg123](https://www.mpg123.de/) | `libmpg123-0.dll` |
| WAV    | Built-in | — |

Place the required DLLs in the same directory as `sapfs.exe`, or ensure they are on your `PATH`.

---

## Building

From a Visual Studio Developer Command Prompt, run:

```bat
build.bat
```

The compiled binary is placed in the `dist/` directory.

---

## Usage

```
sapfs [OPTIONS] <audio_file>
```

### Options

| Option | Description |
|--------|-------------|
| `--exclusive` | Use WASAPI exclusive mode (bit-perfect) *(Windows only)* |
| `--buffer <ms>` | Set audio buffer size in milliseconds, 10–5000 *(shared mode only)* |
| `--loop` | Loop playback indefinitely |
| `-h`, `--help` | Show help message |

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
│   └── ring_buffer.c         # Thread-safe ring buffer (producer/consumer)
└── audio_converter.c         # PCM format converters (8/16/24/32-bit ↔ float)
```

### Signal Flow

```
File → [Decoder] → float PCM → [Ring Buffer] → [WASAPI Consumer Thread] → [Device]
```

The decoder thread reads raw audio into a float buffer and writes it to a ring buffer. A dedicated WASAPI consumer thread drains the ring buffer and writes converted PCM to the audio device, registered under MMCSS `Pro Audio` priority for low-latency scheduling.

---

## Supported WAV Variants

- 8-bit unsigned PCM
- 16-bit signed PCM
- 24-bit packed (3 bytes/sample)
- 24-bit padded in 32-bit container (`WAVE_FORMAT_EXTENSIBLE`)
- 32-bit signed PCM
- Multi-channel layouts (up to 7.1 surround)