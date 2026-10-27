#!/usr/bin/env python3
"""PulseAudio integration tests on a private server with virtual sinks only.

Requires libpulse development files, PulseAudio, pactl, and parec. PULSEAUDIO,
PACTL, PAREC and PULSE_MODULE_PATH can select tools outside PATH. No desktop
server or audio hardware is accessed. Compiler flags from the environment apply.
"""
import os
from pathlib import Path
import shlex
import select
import shutil
import signal
import socket as sockets
import struct
import subprocess
import tempfile
import time

from test_alsa import wav_file

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=40, **kwargs)


def main():
    os.chdir(ROOT)
    tools = [os.environ.get(var) or shutil.which(name) for var, name in
             (("PULSEAUDIO", "pulseaudio"), ("PACTL", "pactl"), ("PAREC", "parec"))]
    if not all(tools):
        raise SystemExit("PulseAudio tests require pulseaudio, pactl and parec")
    pulseaudio, pactl, parec = tools
    run(["bash", "build_linux.sh"], env=dict(os.environ, WITH_PULSEAUDIO="1"))
    binary = str(ROOT / "dist/sapfs")
    with tempfile.TemporaryDirectory(prefix="sapfs-pulse-tests-") as directory:
        tmp = Path(directory)
        runtime = tmp / "runtime"
        runtime.mkdir(mode=0o700)
        socket = runtime / "native"
        env = dict(os.environ, PULSE_SERVER=f"unix:{socket}", PULSE_RUNTIME_PATH=str(runtime),
                   PULSE_STATE_PATH=str(tmp / "state"), XDG_RUNTIME_DIR=str(runtime),
                   XDG_CONFIG_HOME=str(tmp / "config"),
                   DBUS_SESSION_BUS_ADDRESS=f"unix:path={tmp / 'unused-session-bus'}")
        client_config = tmp / "client.conf"
        client_config.write_text("autospawn = no\nenable-shm = no\n")
        env["PULSE_CLIENTCONFIG"] = str(client_config)
        config = tmp / "server.pa"
        # Rewinds on a null sink can discard monitor data already delivered to
        # recorders, including with the reference paplay client. Disable them.
        config.write_text(f"load-module module-native-protocol-unix socket={socket} auth-anonymous=1\n"
                          "load-module module-null-sink sink_name=test-sink norewinds=1 format=float32le rate=48000 channels=2\n"
                          "load-module module-null-sink sink_name=other-sink norewinds=1 format=float32le rate=48000 channels=2\n"
                          "set-default-sink test-sink\n")
        args = [pulseaudio, "-n", f"--file={config}", "--daemonize=no", "--use-pid-file=no",
                "--exit-idle-time=-1", "--disable-shm=yes", "--realtime=no", "--high-priority=no",
                f"--log-target=file:{tmp / 'server.log'}"]
        if os.environ.get("PULSE_MODULE_PATH"):
            args += [f"--dl-search-path={os.environ['PULSE_MODULE_PATH']}"]
        server = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        children = []
        try:
            for _ in range(200):
                if server.poll() is not None:
                    raise AssertionError((tmp / "server.log").read_text() + server.stderr.read().decode())
                result = subprocess.run([pactl, "info"], env=env, capture_output=True, timeout=2)
                if result.returncode == 0:
                    break
                time.sleep(.025)
            else:
                raise AssertionError("Private PulseAudio server did not start\n" + (tmp / "server.log").read_text() + result.stderr.decode())
            listing = run([binary, "--list-pulseaudio-devices"], env=env, capture_output=True)
            assert b"test-sink" in listing.stdout and b"other-sink" in listing.stdout
            # An explicit CLI server must override an inherited PULSE_SERVER.
            run([binary, "--list-pulseaudio-devices", "--pulse-server", f"unix:{socket}"],
                env=dict(env, PULSE_SERVER="unix:/nonexistent/sapfs-pulse"), capture_output=True)
            pkg_cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "libpulse", "alsa"], text=True))
            pkg_libs = shlex.split(subprocess.check_output(["pkg-config", "--libs", "libpulse", "alsa"], text=True))
            helper = str(tmp / "api-test")
            run([os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-DSAPFS_HAVE_PULSEAUDIO",
                 "-Wall", "-Wextra", "-Werror", "-g", "-Isrc", *pkg_cflags,
                 *shlex.split(os.environ.get("CFLAGS", "")), "tests/pulseaudio_output_test.c",
                 "src/audio_output/audio_output.c", "src/audio_output/linux_output.c",
                 "src/audio_output/alsa_output.c", "src/audio_output/pulseaudio_output.c",
                 "-Wl,--wrap=pa_stream_write", "-Wl,--wrap=pa_stream_drain", *shlex.split(os.environ.get("LDFLAGS", "")),
                 *pkg_libs, "-o", helper])
            run([helper], env=env)

            with sockets.socket(sockets.AF_UNIX, sockets.SOCK_STREAM) as unresponsive:
                unresponsive.bind(str(tmp / "unresponsive"))
                unresponsive.listen(1)
                run([helper, "connect", f"unix:{tmp / 'unresponsive'}"], env=env)

            def play(path, options=(), expected=None, channels=2, monitor="test-sink.monitor"):
                if expected is None:
                    return run([binary, "--output", "pulseaudio", *options, str(path)],
                               env=env, stdin=subprocess.DEVNULL, capture_output=True)
                capture = tmp / "capture.raw"
                mapping = []
                if channels == 8:
                    mapping = ["--channel-map=front-left,front-right,front-center,lfe,rear-left,rear-right,side-left,side-right"]
                with capture.open("wb") as output:
                    recorder = subprocess.Popen([parec, "--raw", "--format=float32le", "--rate=48000",
                                                 f"--channels={channels}", f"--device={monitor}", "--latency-msec=100", *mapping],
                                                env=env, stdout=output, stderr=subprocess.PIPE)
                    children.append(recorder)
                    for _ in range(100):
                        sources = run([pactl, "list", "short", "source-outputs"], env=env, capture_output=True)
                        if sources.stdout.strip():
                            break
                        if recorder.poll() is not None:
                            raise AssertionError(recorder.stderr.read().decode())
                        time.sleep(.01)
                    else:
                        raise AssertionError("Capture client did not connect")
                    # Monitor streams may discard initial device-latency data.
                    # Wait for actual silence to arrive before submitting music.
                    for _ in range(300):
                        if capture.stat().st_size >= channels * 4 * 4800:
                            break
                        time.sleep(.01)
                    else:
                        raise AssertionError("Capture monitor did not begin delivering samples")
                    result = run([binary, "--output", "pulseaudio", *options, str(path)],
                                 env=env, stdin=subprocess.DEVNULL, capture_output=True)
                    time.sleep(.15)
                    recorder.terminate()
                    recorder.communicate(timeout=5)
                actual = capture.read_bytes()
                assert expected in actual, f"{path.name}: sample sequence missing ({len(actual)} capture bytes)"
                assert b"100%" in result.stdout
                return result

            for bits in (8, 16, 24, 32):
                width = bits // 8
                state = 0x12345678
                values = []
                for _ in range(2400):
                    state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
                    value = state >> (32 - bits)
                    values.append(value if bits == 8 else value - (1 << (bits - 1)))
                data = b"".join(v.to_bytes(width, "little", signed=bits != 8) for v in values)
                expected = b"".join(struct.pack("<f", (v - 128) / 128 if bits == 8 else v / (1 << (bits - 1))) for v in values)
                path = tmp / f"pcm{bits}.wav"
                wav_file(path, bits, data)
                play(path, ("--buffer", "100"), expected)
                if bits == 24:
                    padded = tmp / "padded24.wav"
                    wav_file(padded, 24, b"".join(v.to_bytes(4, "little", signed=True) for v in values))
                    header = bytearray(padded.read_bytes())
                    struct.pack_into("<I", header, 28, 48000 * 8)
                    struct.pack_into("<H", header, 32, 8)
                    padded.write_bytes(header)
                    play(padded, expected=expected)
                short = tmp / f"short{bits}.wav"
                wav_file(short, bits, data[:2 * width])
                play(short, expected=expected[:8])
            values = [((i * 79) % 1024 - 512) / 512 for i in range(2400)]
            floats = struct.pack(f"<{len(values)}f", *values)
            path = tmp / "float.wav"
            wav_file(path, 32, floats, float_pcm=True)
            play(path, expected=floats)
            left_aligned = b"".join((v << 8).to_bytes(4, "little", signed=True)
                                     for v in range(-1200, 1200))
            path = tmp / "extensible24.wav"
            wav_file(path, 32, left_aligned, valid_bits=24)
            play(path, expected=b"".join(struct.pack("<f", v / 8388608) for v in range(-1200, 1200)))
            mapping = "front-left,front-right,front-center,lfe,rear-left,rear-right,side-left,side-right"
            module = run([pactl, "load-module", "module-null-sink", "sink_name=eight-sink", "norewinds=1", "format=float32le",
                          "rate=48000", "channels=8", f"channel_map={mapping}"], env=env, capture_output=True).stdout.strip().decode()
            multi = tmp / "eight.wav"
            pcm = b"".join((i * 31 - 20000).to_bytes(2, "little", signed=True) for i in range(1024))
            fmt = struct.pack("<HHIIHH", 1, 8, 48000, 48000 * 16, 16, 16)
            chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(pcm)) + pcm
            multi.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)
            play(multi, ("--device", "eight-sink"),
                 b"".join(struct.pack("<f", (i * 31 - 20000) / 32768) for i in range(1024)),
                 channels=8, monitor="eight-sink.monitor")
            run([pactl, "unload-module", module], env=env, capture_output=True)
            # Server resampling is allowed and does not alter the client rate.
            resampled = bytearray(path.read_bytes())
            struct.pack_into("<I", resampled, 24, 44100)
            struct.pack_into("<I", resampled, 28, 44100 * 8)
            path = tmp / "44100.wav"
            path.write_bytes(resampled)
            play(path)
            empty = tmp / "empty.wav"
            wav_file(empty, 16, b"")
            play(empty)
            path = tmp / "float.wav"
            # Explicit sink routing and default-sink policy use distinct monitors.
            play(path, ("--device", "other-sink"), floats, monitor="other-sink.monitor")
            run([pactl, "set-default-sink", "other-sink"], env=env, capture_output=True)
            play(path, expected=floats, monitor="other-sink.monitor")
            run([pactl, "set-default-sink", "test-sink"], env=env, capture_output=True)
            for options, fragment in [(("--exclusive",), b"exclusive"),
                                      (("--device", "missing-sapfs-sink"), b"sink"),
                                      (("--pulse-server", "unix:/nonexistent/sapfs-pulse"), b"PulseAudio"),
                                      (("--pipewire-remote", "test"), b"pipewire"),
                                      (("--jack-no-connect",), b"JACK")]:
                result = subprocess.run([binary, "--output", "pulseaudio", *options, str(path)],
                                        env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
                assert result.returncode != 0 and fragment in result.stderr, result.stderr
            for options in (("--pulse-server",), ("--output", "alsa", "--pulse-server", "test"),
                            ("--list-pulseaudio-devices", "--list-pipewire-devices"),
                            ("--list-pulseaudio-devices", "--list-jack-ports")):
                result = subprocess.run([binary, *options], env=env, capture_output=True, timeout=10)
                assert result.returncode != 0
            def failure(mode, action, device="test-sink"):
                client = subprocess.Popen([helper, mode, device], env=env, stdin=subprocess.PIPE,
                                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                children.append(client)
                assert select.select([client.stdout], [], [], 6)[0], "Failure client did not become ready"
                assert client.stdout.readline() == b"READY\n"
                action()
                output, stderr = client.communicate(b"go\n", timeout=8)
                assert client.returncode == 0 and b"bounded failure" in output, stderr

            failure("drain-ack", lambda: None)
            for mode in ("control", "stall"):
                try:
                    failure(mode, lambda: server.send_signal(signal.SIGSTOP))
                finally:
                    server.send_signal(signal.SIGCONT)
            try:
                failure("suspend", lambda: run([pactl, "suspend-sink", "test-sink", "1"], env=env, capture_output=True))
            finally:
                run([pactl, "suspend-sink", "test-sink", "0"], env=env, capture_output=True)
            module = run([pactl, "load-module", "module-null-sink", "sink_name=removed-sink"],
                         env=env, capture_output=True).stdout.strip().decode()
            failure("removed", lambda: run([pactl, "unload-module", module], env=env, capture_output=True), "removed-sink")
            # Simultaneous streams and looping, then shutdown of the owned server.
            loop = subprocess.Popen([binary, "--output", "pulseaudio", "--device", "test-sink", "--loop", str(path)],
                                    env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            children.append(loop)
            time.sleep(.5)
            assert loop.poll() is None
            play(path)
            server.terminate()
            server.communicate(timeout=5)
            _, stderr = loop.communicate(timeout=10)
            assert loop.returncode != 0 and b"PulseAudio" in stderr, stderr
            result = subprocess.run([binary, "--list-pulseaudio-devices"], env=env, capture_output=True, timeout=10)
            assert result.returncode != 0
            print("PulseAudio monitor PCM8/16/24/32, float, short EOF, 24-bit layouts and eight-channel comparisons passed")
            print("PulseAudio routing, resampling, shared clients, looping, timeouts, suspension, sink removal and shutdown checks passed")
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
                    child.communicate(timeout=5)
            if server.poll() is None:
                server.terminate()
                server.communicate(timeout=5)


if __name__ == "__main__":
    main()
