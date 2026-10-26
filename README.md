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
- Linux **PipeWire** audio output:
  - Shared desktop playback with mixing and resampling
  - Exclusive session-manager linking with client resampling/remixing disabled
  - Sink discovery, target selection, remote server selection, and asynchronous draining
- Configurable **buffer size** (Windows shared mode and Linux backends)
- Internal **32-bit float pipeline** on Windows/macOS; **native PCM delivery** to Linux backends
- TPDF **dithering** when converting float → 16-bit PCM

---

## Requirements

### Build

| Platform | Toolchain |
|----------|-----------|
| **Windows** | MSVC (`cl.exe`) — run from a Visual Studio Developer Command Prompt |
| **macOS** | Xcode Command Line Tools (`clang`) — `xcode-select --install` |
| **Linux** | C11 compiler, `pkg-config`, ALSA development files (`libasound2-dev` on Debian/Ubuntu, `alsa-lib-devel` on Fedora) |

PipeWire support additionally needs `libpipewire-0.3` development files, version
0.3.50 or newer (`libpipewire-0.3-dev` on Debian/Ubuntu, `pipewire-devel` on Fedora).

### Runtime Dependencies

| Format | Library | Windows DLL | macOS dylib | Linux |
|--------|---------|-------------|-------------|-------|
| FLAC   | [libFLAC](https://xiph.org/flac/) | `libFLAC.dll` | `libFLAC.dylib` | `libFLAC.so.8`, `.12`, or `.14` |
| MP3    | [libmpg123](https://www.mpg123.de/) | `libmpg123-0.dll` | `libmpg123.dylib` | `libmpg123.so.0` |
| WAV    | Built-in | — | — | — |

Codec libraries are loaded at **runtime** via `dlopen`/`LoadLibrary` and are not required at build time. Linux links against ALSA (`libasound`).
Builds with PipeWire enabled also link against `libpipewire-0.3`. Playback through
PipeWire requires a running server and a session manager providing audio routing
and exclusive-link policy, such as WirePlumber.

---

## Building

### Linux

```bash
# Debian/Ubuntu: build dependencies and optional FLAC/MP3 runtime libraries
sudo apt install build-essential pkg-config libasound2-dev libflac14 libmpg123-0
# Optional PipeWire backend
sudo apt install libpipewire-0.3-dev
./build_linux.sh
```

The compiled binary is placed in `dist/sapfs`. The build respects `CC`, `CFLAGS`,
and `LDFLAGS`. Older distributions may name the FLAC runtime package `libflac8`
or `libflac12`.

The build automatically enables PipeWire when its development files are found.
Use `WITH_PIPEWIRE=1 ./build_linux.sh` to require it, or
`WITH_PIPEWIRE=0 ./build_linux.sh` for an ALSA-only binary. A binary without
PipeWire reports a build-support error if that backend is selected.

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
| `--exclusive` | WASAPI exclusive, CoreAudio hog, direct ALSA hardware, or PipeWire exclusive linking | All |
| `--shared` | Allow ALSA plugins or use PipeWire mixed playback | Linux only |
| `--device <name>` | ALSA PCM or PipeWire sink `node.name` / `object.serial` | Linux only |
| `--buffer <ms>` | Buffer duration, 10–5000 ms; Linux default 100 ms | Windows shared mode / Linux |
| `--asio` | Use ASIO audio output | Windows only |
| `--output <wasapi\|asio>` | Select the Windows output backend | Windows only |
| `--output <alsa\|pipewire>` | Select the Linux output backend; default ALSA (`--backend` is an alias) | Linux only |
| `--pipewire-remote <name>` | Select a PipeWire server; otherwise use the standard PipeWire environment/default | Linux only |
| `--list-pipewire-devices` | List sink names, descriptions, and object serials, then exit | Linux only |
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

### PipeWire playback on Linux

```bash
# Shared desktop playback (the default mode for this backend)
dist/sapfs --output pipewire song.flac

# Find available playback sinks
dist/sapfs --list-pipewire-devices

# Select a sink by its node.name or the object.serial printed by the list
dist/sapfs --output pipewire --device alsa_output.usb-DAC.analog-stereo song.wav

# Exclusive session-manager linking, with no client resampling or channel remixing
dist/sapfs --output pipewire --exclusive --device alsa_output.usb-DAC.analog-stereo album24.flac

# Select another server and configure the application's queue
dist/sapfs --output pipewire --pipewire-remote pipewire-0 --buffer 200 song.wav
```

ALSA continues to default to exclusive hardware playback; selecting PipeWire
defaults to shared playback. An explicit `--shared` or `--exclusive` overrides
the selected backend's default regardless of argument order. Shared playback
without an explicit target follows session-manager routing; explicit targets and
exclusive streams remain pinned. `--pipewire-remote` overrides the inherited
`PIPEWIRE_REMOTE` value for this CLI process. In PipeWire mode,
`--buffer` controls the application FIFO and suggests a graph latency; the server
chooses the actual graph quantum and device buffering.

| PipeWire mode | Behavior | Precision guarantee |
|---------------|----------|---------------------|
| Shared | Session-manager routing, mixing, volume controls, format conversion and resampling | Normal PipeWire processing; full 32-bit integer source precision may be reduced by the float32 graph |
| Exclusive | Requests exclusive sink linking, disables client resampling, channel remixing and dithering, requests the source graph rate and verifies it before submitting audio | Does not certify a bit-perfect path through the server and hardware |

Exclusive access depends on the session manager honoring PipeWire's exclusive
link request. A busy sink is rejected under WirePlumber policy. Sapfs does not
disconnect other applications or change server configuration to seize a device.
The graph can adopt the source rate when idle and configured to allow that rate;
an incompatible graph rate fails instead of silently resampling in exclusive
mode. The backend does not force a global graph rate. A selected target does not
fall back to another sink, and target/server failures are reported. Connection
and stalled-graph waits are bounded (five seconds by default).

**Exclusive linking is not a bit-perfect certification.** The standard PipeWire
graph uses float32 processing. Sink format conversion, sink volume, effects,
clock adaptation, hardware resampling and device controls can still affect the
output. Integer sources with more than 24 valid bits are rejected in exclusive
mode because float32 cannot preserve their full precision; use direct ALSA for
those sources. Shared mode accepts them with normal PipeWire conversion. For
strict sample-preserving hardware playback, use `--output alsa --exclusive`.
See the official [PipeWire properties](https://docs.pipewire.org/page_man_pipewire-props_7.html)
and [stream API](https://docs.pipewire.org/group__pw__stream.html).

PipeWire accepts unsigned 8-bit, signed 16/24/32-bit, and float32 application
PCM, including packed and padded 24-bit samples. Channel masks and standard
mono-to-7.1 layouts are passed to PipeWire. Writes are nonblocking and preserve
complete frames under backpressure. Pause/resume keeps the application queue;
stop discards it. EOF uses PipeWire's drain notification, including queued
converter/device data, and a completed stream can be reused for loop playback.

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
│   ├── linux_output.c        # Linux backend selector and operation dispatch
│   ├── alsa_output.c         # ALSA PCM negotiation, lossless packing and playback
│   ├── pipewire_output.c     # PipeWire stream, PCM queue, sink discovery and drain
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
- **Linux**: `audio_output.c` delegates to `linux_output.c`, which owns backend selection and dispatch through a table of operations. PulseAudio and JACK can be added as separate backend modules.
- **ALSA**: Uses nonblocking interleaved writes into the device buffer. The player retains partial writes, remains responsive to controls under backpressure, and waits for the device's queued frames at EOF. Underruns and device suspension are recovered where possible.
- **PipeWire**: Uses an independent `pw_thread_loop` and a bounded, frame-aligned FIFO. Process callbacks and API operations are serialized by the loop; `RT_PROCESS` is deliberately omitted so application locking and error reporting do not run on PipeWire's realtime data thread. The stream negotiates the native application PCM format and channel layout. Server processing follows the selected mode. EOF is an explicit nonblocking drain operation rather than an inference from constant graph latency.

The Linux application preserves native decoder PCM up to the backend:

```
File -> [Native PCM Decoder] -> [ALSA: optional lossless packing] -> [Device Buffer] -> [Device]
                           \-> [PipeWire FIFO] -> [PipeWire Graph] -> [Sink/Device]
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

### PipeWire verification

```bash
python3 tests/test_pipewire.py
```

Requires PipeWire tools/development files, WirePlumber 0.5 or newer, and
`dbus-run-session`. The suite launches a private server with virtual sinks and a
policy-only session manager, without using hardware or the desktop audio server.
The virtual clients disable the RTKit priority helper; normal playback continues
to honor the installed PipeWire client configuration.
It compares captured shared-mode PCM sample sequences, and exercises pause,
backpressure, drain/restart, short and empty files, shared resampling, exclusive
rate rejection, busy-sink rejection, missing targets, graph stalls and server
disconnects. Physical sink behavior and end-to-end DAC precision require hardware
verification.

---

## Supported WAV Variants

- 8-bit unsigned PCM
- 16-bit signed PCM
- 24-bit packed (3 bytes/sample)
- 24-bit padded in 32-bit container (`WAVE_FORMAT_EXTENSIBLE`)
- 32-bit signed PCM
- 32-bit IEEE float PCM
- Multi-channel layouts (up to 7.1 surround)
