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
- Linux **JACK** audio output:
  - Shared planar float32 playback at the server's sample rate
  - Automatic physical-port routing, destination patterns, explicit channel mapping, or manual patchbay routing
  - Lock-free callback buffering, server selection, port discovery, pause/resume and drain
- Linux **PulseAudio** audio output:
  - Shared desktop playback with server-managed mixing, resampling and volume
  - Native PCM delivery, sink/server selection and playback-sink discovery
  - Bounded partial writes, pause/resume, stop/restart and asynchronous EOF draining
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
JACK support additionally needs a JACK development package providing `jack.pc`
(`libjack-jackd2-dev` on Debian/Ubuntu).
PulseAudio support additionally needs `libpulse` development files
(`libpulse-dev` on Debian/Ubuntu, `pulseaudio-libs-devel` on Fedora). Each optional
backend can be disabled independently.

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
Builds with JACK enabled link against `libjack` and require an already running
JACK server, or a compatible JACK implementation. Sapfs never starts a server.
Builds with PulseAudio enabled link against `libpulse` and require a running
PulseAudio server or a compatible service such as `pipewire-pulse`. PulseAudio
autospawn is disabled for both playback and discovery.

---

## Building

### Linux

```bash
# Debian/Ubuntu: build dependencies and optional FLAC/MP3 runtime libraries
sudo apt install build-essential pkg-config libasound2-dev libflac14 libmpg123-0
# Optional PipeWire backend
sudo apt install libpipewire-0.3-dev
# Optional JACK backend
sudo apt install libjack-jackd2-dev
# Optional PulseAudio backend
sudo apt install libpulse-dev
./build_linux.sh
```

The compiled binary is placed in `dist/sapfs`. The build respects `CC`, `CFLAGS`,
and `LDFLAGS`. Older distributions may name the FLAC runtime package `libflac8`
or `libflac12`.

The build automatically enables PipeWire when its development files are found.
Use `WITH_PIPEWIRE=1 ./build_linux.sh` to require it, or
`WITH_PIPEWIRE=0 ./build_linux.sh` to exclude PipeWire. A binary without
PipeWire reports a build-support error if that backend is selected.
JACK is also enabled automatically when its development files are found.
`WITH_JACK=1 ./build_linux.sh` requires JACK support; `WITH_JACK=0` excludes it.
PulseAudio follows the same convention: `WITH_PULSEAUDIO=1 ./build_linux.sh`
requires support; `WITH_PULSEAUDIO=0` excludes it; the default is `auto`.
For ALSA alone, set `WITH_PIPEWIRE=0 WITH_JACK=0 WITH_PULSEAUDIO=0`.
The build prints the enabled backends.

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
| `--shared` | Allow ALSA plugins, use PipeWire mixed playback, use JACK's shared graph, or use PulseAudio mixed playback | Linux only |
| `--device <name>` | ALSA PCM, PipeWire sink `node.name` / `object.serial`, PulseAudio sink name, or JACK destination-port regex | Linux only |
| `--buffer <ms>` | Buffer duration, 10–5000 ms; Linux default 100 ms | Windows shared mode / Linux |
| `--asio` | Use ASIO audio output | Windows only |
| `--output <wasapi\|asio>` | Select the Windows output backend | Windows only |
| `--output <alsa\|pipewire\|jack\|pulseaudio>` | Select the Linux output backend; default ALSA (`--backend` is an alias) | Linux only |
| `--pipewire-remote <name>` | Select a PipeWire server; otherwise use the standard PipeWire environment/default | Linux only |
| `--list-pipewire-devices` | List sink names, descriptions, and object serials, then exit | Linux only |
| `--jack-server <name>` | Select an already running JACK server; otherwise use the JACK default/environment | Linux only |
| `--jack-name <name>` | Request a JACK client name, default `sapfs`; JACK may make it unique | Linux only |
| `--jack-port <name>` | Repeat once per source channel to specify destination audio input ports in source order | Linux only |
| `--jack-no-connect` | Route `out_1` through `out_N` manually with a JACK patchbay | Linux only |
| `--list-jack-ports` | List destination audio input ports and exit; accepts `--jack-server` and `--device` filtering | Linux only |
| `--pulse-server <address>` | Select a running PulseAudio server; overrides `PULSE_SERVER` | Linux only |
| `--list-pulseaudio-devices` | List PulseAudio sink names, descriptions and indexes, then exit | Linux only |
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

### JACK playback on Linux

```bash
# Connect source channels to the first matching physical playback ports
dist/sapfs --output jack song.flac

# List destination audio input ports, including virtual clients
dist/sapfs --list-jack-ports

# Select an already running named server
dist/sapfs --output jack --jack-server studio song.wav

# Choose destination ports by a JACK regular expression
dist/sapfs --output jack --device '^effects:in_' song.wav

# Explicit channel mapping: left -> playback_3, right -> playback_4
dist/sapfs --output jack --jack-port system:playback_3 --jack-port system:playback_4 song.wav

# Use a patchbay to connect the registered output ports yourself
dist/sapfs --output jack --jack-name music --jack-no-connect song.wav
```

JACK playback uses the server's shared graph. `--shared` is accepted and is the
default for this backend. **JACK does not provide per-client exclusive hardware
access**, so `--exclusive` reports an error instead of implying bit-perfect
playback. This backend does not change the server's sample rate, period size,
global transport, or other clients' connections, and opens with `JackNoStartServer`.

The file's sample rate must exactly match the JACK server's rate. A mismatch
reports both rates and fails without resampling. JACK's audio ports use float32:
the backend converts unsigned 8-bit and signed 16/24/32-bit PCM on the writing
thread, accepts packed/padded 24-bit containers and float32, and deinterleaves
frames into one JACK output port per source channel. Conversion preserves
integer samples up to 24 bits; full 32-bit integer precision cannot be preserved
in float32. Downstream mixing, effects, gain, conversion and hardware behavior
remain outside this backend's control. Direct ALSA is available for strict
hardware playback. These semantics also apply when a PipeWire JACK compatibility
library supplies the JACK API.

Automatic routing requires at least as many matching destination ports as source
channels and selects them in JACK's returned order. Explicit `--jack-port`
arguments must provide one distinct audio input port per source channel, in the
file's channel order. JACK ports have no implicit speaker-layout mapping; use
explicit mappings when multichannel hardware order differs. A destination pattern
and explicit mappings are mutually exclusive. `--jack-no-connect` accepts neither.

Manual routing retains queued frames until **every source output port** is
connected, so connecting channels one at a time cannot discard the other channels.
Waiting for manual connections is intentional and continues until you connect
the ports or stop playback. Automatic routing holds the queue during a temporary
disconnection, then reports an error if a channel remains disconnected for five
seconds by default. It does not silently select replacement ports.

`--buffer` controls the application FIFO, not the server quantum. Its frame
capacity is rounded up to a power of two and holds at least two initial server
periods, even when the requested duration is shorter. PCM conversion and port queries stay
off the real-time thread; the process callback uses preallocated storage and
lock-free atomics, and writes silence during underflow, pause or stop. Pause and
stop wait for callback acknowledgement; pause preserves the remaining queue,
while stop clears it. Both preserve port connections by keeping the client
active and silent. Closing the output unregisters its ports and releases the
client. Drain includes a graph cycle and reported downstream playback latency,
and the same client can be reused for looping. Server shutdown, rate changes and
stalled processing are reported; period changes that fit the preallocated FIFO
are supported without callback allocation. A larger period fails with a request
to reopen with a larger buffer. JACK xruns are counted and processing continues.

See the official [JACK client API](https://jackaudio.org/api/group__ClientFunctions.html),
[callback requirements](https://jackaudio.org/api/group__ClientCallbacks.html), and
[latency API](https://jackaudio.org/api/group__LatencyFunctions.html).

### PulseAudio playback on Linux

```bash
# Shared playback through the default sink
dist/sapfs --output pulseaudio song.flac

# List sinks, then select one by name
dist/sapfs --list-pulseaudio-devices
dist/sapfs --output pulseaudio --device alsa_output.usb-DAC.analog-stereo song.wav

# Choose a running server and request a 200 ms server playback queue
dist/sapfs --output pulseaudio --pulse-server unix:/run/user/1000/pulse/native --buffer 200 song.wav
```

PulseAudio defaults to shared playback. `--shared` is accepted; `--exclusive`
is rejected because the PCM client API does not provide exclusive hardware
ownership. Encoded passthrough is a separate facility and is not an exclusive
mode for decoded music. For strict hardware playback, use direct ALSA.

The backend sends native little-endian unsigned 8-bit, signed 16/24/32-bit or
float32 PCM without application resampling, dithering or conversion to float.
Packed 24-bit, signed 24-bit in the low bits of 32-bit words, and left-aligned
WAVE extensible PCM containers are distinguished. Speaker masks and standard
mono-to-7.1 layouts are supplied in source order. This preserves PCM delivery
**to the server**, including full integer low bits; the server can resample,
remix channels, mix applications, convert formats and apply gain. There is no
end-to-end bit-perfect guarantee, including through `pipewire-pulse`.

Without `--device`, routing follows server policy and the stream may be moved.
An explicit sink is pinned with `PA_STREAM_DONT_MOVE`; missing or removed sinks
report an error instead of falling back to another device. `--pulse-server`
overrides `PULSE_SERVER` for this connection; without it, normal PulseAudio
configuration and environment selection apply. Sapfs disables server autospawn
and does not change global volume, sample rate, routing policy or configuration.

`--buffer` requests the server-side per-stream queue duration. PulseAudio can
adjust buffer metrics to suit the sink; the backend honors its writable size and
retains complete frames under backpressure. Writes go directly to the bounded
server queue. Pause corks playback while retaining queued data; stop corks
and flushes it. EOF polls an asynchronous server drain operation, which also
starts playback for files shorter than the prebuffer. The same stream supports
repeated drain/write segments for looping. Queue progress uses reported latency;
EOF completion uses the drain acknowledgement rather than a latency estimate.
Each output owns an independent threaded mainloop and context. Connection and
control waits, stalled playback and server failures have bounded error handling
(five seconds by default). Drain allows the queued playback time plus the
timeout, excluding time spent paused.

See the official [PulseAudio stream API](https://www.freedesktop.org/software/pulseaudio/doxygen/stream_8h.html),
[buffer attributes](https://www.freedesktop.org/software/pulseaudio/doxygen/structpa__buffer__attr.html),
and [threaded mainloop](https://www.freedesktop.org/software/pulseaudio/doxygen/threaded_mainloop.html).

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
│   ├── jack_output.c         # JACK client, SPSC float FIFO, port routing and drain
│   ├── pulseaudio_output.c   # PulseAudio context, native PCM stream, discovery and drain
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
- **Linux**: `audio_output.c` delegates to `linux_output.c`, which owns backend selection and dispatch through a table of operations. ALSA, PipeWire, JACK and PulseAudio remain separate backend modules.
- **ALSA**: Uses nonblocking interleaved writes into the device buffer. The player retains partial writes, remains responsive to controls under backpressure, and waits for the device's queued frames at EOF. Underruns and device suspension are recovered where possible.
- **PipeWire**: Uses an independent `pw_thread_loop` and a bounded, frame-aligned FIFO. Process callbacks and API operations are serialized by the loop; `RT_PROCESS` is deliberately omitted so application locking and error reporting do not run on PipeWire's realtime data thread. The stream negotiates the native application PCM format and channel layout. Server processing follows the selected mode. EOF is an explicit nonblocking drain operation rather than an inference from constant graph latency.
- **JACK**: The writing thread converts native PCM into a bounded interleaved float SPSC queue. JACK's process callback deinterleaves it into output ports without locks, allocation or I/O. Atomic control acknowledgements coordinate pause/stop with queue resets. Port-connection notifications invalidate an owner-thread routing snapshot so the callback can hold data until every channel is connected.
- **PulseAudio**: Each output owns a `pa_threaded_mainloop` and context. The native PCM stream starts corked; controls use bounded, acknowledged cork/flush operations. Polled writable sizes limit nonblocking partial writes to the server queue. Asynchronous drain reports EOF completion, including short segments, and can be cancelled on stop/close. Sink enumeration returns a snapshot outside the loop lock.

The Linux application preserves native decoder PCM up to the backend:

```
File -> [Native PCM Decoder] -> [ALSA: optional lossless packing] -> [Device Buffer] -> [Device]
                           \-> [PipeWire FIFO] -> [PipeWire Graph] -> [Sink/Device]
                           \-> [JACK float FIFO] -> [JACK Ports/Graph] -> [Destination]
                           \-> [PulseAudio PCM Stream] -> [Server Mix/Resampler] -> [Sink/Device]
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

### JACK verification

```bash
python3 tests/test_jack.py
```

Requires JACK development files and JACK2's `jackd` with the dummy driver.
`JACKD` can point to a server executable outside `PATH`. Tests create a private
named server with virtual playback ports and capture clients; no audio hardware
or existing desktop/JACK server is used. They compare captured PCM and float
sample sequences, including packed/padded 24-bit, one-frame files and eight
channels, and exercise queue wrap/backpressure, pause/resume, drain/restart,
period changes, manual/explicit routing, disconnected ports, simultaneous
clients, rate changes, missing servers and shutdown. Hardware latency and audible
output still require a physical-device test.

### PulseAudio verification

```bash
python3 tests/test_pulseaudio.py
```

Requires `libpulse` development files, PulseAudio, `pactl` and `parec`
(`pulseaudio` and `pulseaudio-utils` on Debian/Ubuntu). Tests launch a private
server with null sinks, without touching the desktop server or hardware.
Capture sinks disable rewinds so monitor samples form deterministic sequences.
`PULSEAUDIO`, `PACTL`, `PAREC` and `PULSE_MODULE_PATH` can select tools/modules
outside the usual paths. They verify native client PCM bytes and formats,
monitor sample sequences, short/empty files, partial writes, pause/resume,
stop/restart, drain cancellation/reuse, simultaneous clients, sink/server
selection, eight-channel mapping, shared resampling, argument validation,
suspension, sink removal, control/connection/stall timeouts and server shutdown.
Hardware playback and behavior specific to a different PulseAudio-compatible
server need separate verification.

---

## Supported WAV Variants

- 8-bit unsigned PCM
- 16-bit signed PCM
- 24-bit packed (3 bytes/sample)
- 24-bit padded in 32-bit container (`WAVE_FORMAT_EXTENSIBLE`)
- 32-bit signed PCM
- 32-bit IEEE float PCM
- Multi-channel layouts (up to 7.1 surround)
