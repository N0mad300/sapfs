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
- Windows **ASIO** audio output:
  - Registered ASIO driver discovery
  - Optional driver and output-channel selection
- macOS support
- Linux **ALSA** audio output:
  - Direct hardware access by default, with exact sample rate and channel count
  - Native PCM precision and lossless integer widening; no resampling or dithering
  - Optional ALSA plugin access with `--shared`
- Configurable **buffer size** (Windows shared mode and Linux ALSA)
- Internal **32-bit float pipeline** on Windows/macOS; **native PCM** on Linux
- TPDF **dithering** when converting float → 16-bit PCM

---

## Requirements

### Build

| Platform | Toolchain |
|----------|-----------|
| **Windows** | MSVC (`cl.exe`) — run from a Visual Studio Developer Command Prompt |
| **macOS** | Xcode Command Line Tools (`clang`) — `xcode-select --install` |
| **Linux** | C11 compiler, `pkg-config`, ALSA development files (`libasound2-dev` on Debian/Ubuntu, `alsa-lib-devel` on Fedora) |

### Runtime Dependencies

| Format | Library | Windows DLL | macOS dylib | Linux |
|--------|---------|-------------|-------------|-------|
| FLAC   | [libFLAC](https://xiph.org/flac/) | `libFLAC.dll` | `libFLAC.dylib` | `libFLAC.so.8`, `.12`, or `.14` |
| MP3    | [libmpg123](https://www.mpg123.de/) | `libmpg123-0.dll` | `libmpg123.dylib` | `libmpg123.so.0` |
| WAV    | Built-in | — | — | — |

Codec libraries are loaded at **runtime** via `dlopen`/`LoadLibrary` and are not required at build time. Linux links against ALSA (`libasound`).

---

## Building

### Linux

```bash
# Debian/Ubuntu: build dependencies and optional FLAC/MP3 runtime libraries
sudo apt install build-essential pkg-config libasound2-dev libflac14 libmpg123-0
./build_linux.sh
```

The compiled binary is placed in `dist/sapfs`. The build respects `CC`, `CFLAGS`,
and `LDFLAGS`. Older distributions may name the FLAC runtime package `libflac8`
or `libflac12`.

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
| `--exclusive` | WASAPI exclusive mode, CoreAudio hog mode, or direct ALSA hardware (Linux default) | All |
| `--shared` | Allow ALSA plugins; uses `default` unless `--device` is given | Linux only |
| `--device <pcm>` | Select ALSA PCM, e.g. `hw:CARD=DAC,DEV=0` (default: `hw:0,0`) | Linux only |
| `--buffer <ms>` | Audio buffer duration, 10–5000 ms; ALSA default is 100 ms | Windows shared mode / Linux |
| `--asio` | Use ASIO audio output | Windows only |
| `--output <wasapi\|asio>` | Select the Windows output backend | Windows only |
| `--output alsa` | Select the Linux output backend (`--backend` is an alias) | Linux only |
| `--asio-driver <name>` | Use a specific ASIO driver name | Windows only |
| `--asio-output <n>` | Set the first ASIO output channel, zero-based | Windows only |
| `--list-asio-drivers` | List installed ASIO drivers and exit | Windows only |
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

# List ASIO drivers
sapfs --list-asio-drivers

# Play through ASIO, selecting driver by name
sapfs --asio --asio-driver "Focusrite USB ASIO" song.wav
```

### ALSA listening on Linux

```bash
# List hardware playback devices (alsa-utils)
aplay -l

# Direct hardware; use a stable card name where possible
dist/sapfs --device hw:CARD=DAC,DEV=0 album.flac

# Configure buffering without changing the source sample rate
dist/sapfs --output alsa --device hw:1,0 --buffer 200 song.wav

# Explicit plugin playback; bit-perfect output is not guaranteed
dist/sapfs --shared --device default song.wav
```

Exclusive mode verifies that the opened PCM is a hardware device. It rejects
`default`, `plughw`, `dmix`, and other plugins unless they resolve directly to
hardware. A busy device or unsupported sample rate, channel count, or lossless
sample format causes initialization to fail with a diagnostic; there is no
automatic fallback to shared playback. Another audio service may need to release
the selected hardware first.

ALSA receives native unsigned 8-bit, signed 16/24/32-bit, or float32 PCM.
Packed and padded 24-bit samples are supported. If necessary, integer samples
are widened and aligned without changing their values or adding dither. Devices
with fewer significant bits than the source are rejected. Float sources require
float32 device support; automatic float-to-integer quantization is not performed.
Hardware volume and any processing inside the DAC remain under device control.
MP3 playback preserves the decoder's PCM output; the original MP3 encoding is lossy.

Pause/resume retains queued samples when supported by the device. If hardware
pause is unavailable, the pause command reports an error and playback continues.
Stop drops queued audio and releases the device on close.

---

## Architecture

```
main.c
├── audio_decoder/
│   ├── audio_decoder.c       # Decoder abstraction (float or native PCM output)
│   ├── wave_parser.c         # Native WAV/RIFF parser (PCM, WAVE_FORMAT_EXTENSIBLE)
│   ├── flac_parser.c         # libFLAC wrapper (dynamic loading)
│   └── mp3_parser.c          # libmpg123 wrapper (dynamic loading)
├── audio_output/
│   ├── audio_output.c        # Output abstraction (platform dispatch)
│   ├── windows_output.c      # Windows backend selector (WASAPI / ASIO)
│   ├── wasapi_output.c       # Windows WASAPI backend (shared + exclusive)
│   ├── asio_output.c         # Windows ASIO backend
│   ├── coreaudio_output.c    # macOS CoreAudio AudioUnit backend
│   ├── linux_output.c        # Linux backend selector (currently ALSA)
│   ├── alsa_output.c         # ALSA PCM negotiation, lossless packing and playback
│   └── ring_buffer.c         # Thread-safe ring buffer (producer/consumer)
└── audio_converter.c         # PCM format converters (8/16/24/32-bit ↔ float)
```

### Signal Flow

```
File -> [Decoder] -> float PCM -> [Ring Buffer] -> [Output Backend] -> [Device]
```

- **Windows (WASAPI)**: A dedicated consumer thread (`_beginthreadex`) drains the ring buffer on `WaitForSingleObject`, registered under MMCSS `Pro Audio` priority.
- **Windows (ASIO)**: The ASIO driver owns the real-time callback. The callback drains interleaved float PCM from the ring buffer, de-interleaves it into ASIO output channels, and converts to the driver's sample type.
- **macOS (CoreAudio)**: The AudioUnit engine drives a pull-model render callback on its own real-time thread. The callback drains the ring buffer each time the engine needs a block of samples. Underruns are zero-padded to avoid noise.
- **Linux (ALSA)**: `audio_output.c` delegates to `linux_output.c`, which owns backend selection and dispatch. ALSA uses nonblocking interleaved writes into the device buffer. The player retains partial writes, remains responsive to controls under backpressure, and waits for the device's queued frames at EOF. Underruns and device suspension are recovered where possible. PipeWire, PulseAudio, and JACK can be added as separate backend modules behind the Linux selector.

The Linux signal flow bypasses float conversion:

```
File -> [Native PCM Decoder] -> [ALSA: optional lossless packing] -> [Device Buffer] -> [Device]
```

The ring buffer implementation is fully cross-platform, using `CRITICAL_SECTION` / `CONDITION_VARIABLE` on Windows and `pthread_mutex_t` / `pthread_cond_t` on POSIX platforms.

### ALSA verification

```bash
python3 tests/test_alsa.py
```

Tests use ALSA's file/null plugins without opening hardware. They compare PCM
bytes across WAV formats (including 32-bit integer low bits and short files),
exercise lossless packing, partial writes, state changes, error recovery, and
exclusive-mode rejection of plugins. With `ffmpeg` installed, they also verify
FLAC sample bytes and MP3 playback. Physical device exclusivity and audible
playback require a hardware test. The backend follows the
[ALSA PCM interface](https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html).

---

## Supported WAV Variants

- 8-bit unsigned PCM
- 16-bit signed PCM
- 24-bit packed (3 bytes/sample)
- 24-bit padded in 32-bit container (`WAVE_FORMAT_EXTENSIBLE`)
- 32-bit signed PCM
- 32-bit IEEE float PCM
- Multi-channel layouts (up to 7.1 surround)
