#!/usr/bin/env python3
"""Integration tests on a private PipeWire graph with virtual sinks only.

Requires PipeWire >= 0.3.50 development files/tools, WirePlumber >= 0.5,
dbus-run-session, and a C compiler. Does not use the desktop server or hardware.
"""

import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import struct
import subprocess
import tempfile
import time

from test_alsa import wav_file

ROOT = Path(__file__).resolve().parents[1]
SERVER_CONFIG = """
context.properties = {
    core.daemon = true
    core.name = sapfs-test
    support.dbus = false
    default.clock.rate = 48000
    default.clock.allowed-rates = [ 48000 ]
    default.clock.quantum = 128
    default.clock.min-quantum = 128
    default.clock.max-quantum = 128
}
context.spa-libs = {
    support.* = support/libspa-support
    audio.convert.* = audioconvert/libspa-audioconvert
}
context.modules = [
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-access }
]
context.objects = [
    { factory = spa-node-factory args = {
        factory.name = support.node.driver
        node.name = Dummy-Driver
        priority.driver = 20000
    } }
    { factory = adapter args = {
        factory.name = support.null-audio-sink
        node.name = test-sink
        node.description = "sapfs isolated test sink"
        media.class = Audio/Sink
        audio.channels = 2
        audio.position = [ FL FR ]
        adapter.auto-port-config = { mode = dsp monitor = true control = false position = preserve }
    } }
]
"""


def run(args, **kwargs):
    try:
        return subprocess.run(args, check=True, timeout=15, **kwargs)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        if error.stdout:
            print(error.stdout.decode(errors="replace"))
        if error.stderr:
            print(error.stderr.decode(errors="replace"))
        raise


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def main():
    os.chdir(ROOT)
    for tool in ("pipewire", "wireplumber", "pw-dump", "pw-cat", "dbus-run-session"):
        if not shutil.which(tool):
            raise SystemExit(f"Required integration test tool unavailable: {tool}")
    build_env = dict(os.environ, WITH_PIPEWIRE="1")
    run(["bash", "build_linux.sh"], env=build_env)
    binary = ROOT / "dist/sapfs"
    with tempfile.TemporaryDirectory(prefix="sapfs-pipewire-test-") as directory:
        tmp = Path(directory)
        env = dict(os.environ, XDG_RUNTIME_DIR=str(tmp), PIPEWIRE_REMOTE="sapfs-test",
                   PIPEWIRE_RUNTIME_DIR=str(tmp), XDG_CONFIG_HOME=str(tmp / "config"),
                   XDG_STATE_HOME=str(tmp / "state"), XDG_CACHE_HOME=str(tmp / "cache"),
                   GIO_USE_VFS="local", GIO_USE_VOLUME_MONITOR="unix")
        # A virtual graph needs no RTKit scheduling. Isolate test clients from
        # the system D-Bus priority helper, which retains libdbus allocations
        # even in a minimal context-create/destroy LeakSanitizer probe.
        client_config = tmp / "config/pipewire/client.conf.d"
        client_config.mkdir(parents=True)
        (client_config / "sapfs-test.conf").write_text("context.properties = { module.rt = false }\n")
        config = tmp / "pipewire.conf"
        config.write_text(SERVER_CONFIG)
        logs = open(tmp / "server.log", "w+")
        processes = []
        manager = None
        server = subprocess.Popen(["pipewire", "-c", str(config)], env=env, stdout=logs, stderr=logs)
        processes.append(server)
        try:
            deadline = time.monotonic() + 5
            while not (tmp / "sapfs-test").exists():
                if server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Private PipeWire server failed to start")
                time.sleep(0.01)
            # The policy profile deliberately excludes every hardware monitor.
            manager = subprocess.Popen(["dbus-run-session", "--", "wireplumber", "-p", "policy"],
                                       env=env, stdout=logs, stderr=logs, start_new_session=True)
            result = run([str(binary), "--list-pipewire-devices"], env=env, capture_output=True)
            assert b"test-sink" in result.stdout
            serial = re.search(rb"serial (\d+)", result.stdout).group(1).decode()
            # An explicit remote overrides an environment-provided server name.
            run([str(binary), "--list-pipewire-devices", "--pipewire-remote", "sapfs-test"],
                env=dict(env, PIPEWIRE_REMOTE="absent-environment-server"), capture_output=True)
            time.sleep(0.3)

            cflags = shlex.split(subprocess.check_output(
                ["pkg-config", "--cflags", "alsa", "libpipewire-0.3"], text=True))
            libs = shlex.split(subprocess.check_output(
                ["pkg-config", "--libs", "alsa", "libpipewire-0.3"], text=True))
            helper = tmp / "api-test"
            run([os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-DSAPFS_HAVE_PIPEWIRE",
                 "-Wall", "-Wextra", "-Werror", "-g", "-Isrc", *cflags,
                 *shlex.split(os.environ.get("CFLAGS", "")),
                 "tests/pipewire_output_test.c", "src/audio_output/audio_output.c",
                 "src/audio_output/linux_output.c", "src/audio_output/alsa_output.c",
                 "src/audio_output/pipewire_output.c", *libs, "-pthread",
                 *shlex.split(os.environ.get("LDFLAGS", "")), "-o", str(helper)])
            run([str(helper)], env=env)

            def play(path, options=(), check=True):
                args = [str(binary), "--output", "pipewire", "--device", "test-sink", *options, str(path)]
                if check:
                    return run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True)
                return subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=10)

            def capture_playback(path, expected, options=()):
                capture = tmp / f"capture-{path.stem}.raw"
                recorder = subprocess.Popen(["pw-cat", "-r", "-a", "--target", "test-sink",
                                             "--rate", "48000", "--channels", "2", "--format", "f32", str(capture)],
                                            env=env, stdout=logs, stderr=logs)
                processes.append(recorder)
                time.sleep(.15)
                result = play(path, options)
                assert b"shared output" in result.stdout, result.stdout
                time.sleep(.05)
                stop(recorder)
                actual = capture.read_bytes()
                assert expected in actual, f"PipeWire {path.name} sample sequence changed or truncated ({len(actual)} bytes captured)"

            for bits in (8, 16, 24, 32):
                # A nonperiodic sequence detects dropped, duplicated, and truncated samples.
                state = 0x12345678
                values = []
                for _ in range(4800 * 2):
                    state = (state * 1664525 + 1013904223) & 0xFFFFFFFF
                    value = (state >> (32 - bits)) - (1 << (bits - 1))
                    values.append(value)
                raw = b"".join((v + 128).to_bytes(1, "little") if bits == 8 else
                               v.to_bytes(bits // 8, "little", signed=True) for v in values)
                expected = b"".join(struct.pack("<f", v / (1 << (bits - 1))) for v in values)
                path = tmp / f"pcm{bits}.wav"
                wav_file(path, bits, raw)
                capture_playback(path, expected)
                if bits == 24:
                    padded = tmp / "padded24.wav"
                    wav_file(padded, 24, b"".join(v.to_bytes(4, "little", signed=True) for v in values))
                    header = bytearray(padded.read_bytes())
                    struct.pack_into("<I", header, 28, 48000 * 8)
                    struct.pack_into("<H", header, 32, 8)
                    padded.write_bytes(header)
                    capture_playback(padded, expected)
                    extensible = tmp / "extensible24.wav"
                    wav_file(extensible, 32, b"".join((v << 8).to_bytes(4, "little", signed=True) for v in values), valid_bits=24)
                    capture_playback(extensible, expected, ("--device", serial))
            print("PipeWire shared PCM8/16/24/32 sample sequence comparisons passed", flush=True)

            path = tmp / "pcm16.wav"
            run([str(binary), "--output", "pipewire", str(path)], env=env,
                stdin=subprocess.DEVNULL, capture_output=True) # Default sink routing.
            result = play(path, ("--exclusive",))
            assert b"exclusive" in result.stdout
            # Short file, empty file, and float application PCM all drain correctly.
            for name, data, is_float in (("one", b"\x01\x00\xff\xff", False),
                                          ("empty", b"", False),
                                          ("float", struct.pack("<4f", .125, -.25, .5, -.5), True)):
                short = tmp / f"{name}.wav"
                wav_file(short, 32 if is_float else 16, data, float_pcm=is_float)
                play(short)
            # Shared mode can resample; exclusive mode must reject a mismatched graph rate.
            mismatch = tmp / "44100.wav"
            data = bytearray(path.read_bytes())
            struct.pack_into("<I", data, 24, 44100)
            struct.pack_into("<I", data, 28, 44100 * 4)
            mismatch.write_bytes(data)
            play(mismatch)
            rejected = play(mismatch, ("--exclusive",), check=False)
            assert rejected.returncode != 0
            assert b"graph rate" in rejected.stderr or b"did not start" in rejected.stderr or b"format" in rejected.stderr

            # A running shared client prevents exclusive session-manager linking.
            busy = subprocess.Popen([str(binary), "--output", "pipewire", "--device", "test-sink",
                                     "--loop", str(path)], env=env, stdin=subprocess.DEVNULL, stdout=logs, stderr=logs)
            processes.append(busy)
            time.sleep(.3)
            assert busy.poll() is None
            rejected = play(path, ("--exclusive",), check=False)
            assert rejected.returncode != 0, "Exclusive playback unexpectedly shared a busy sink"
            stop(busy)
            # Explicit target never falls back to the default virtual sink.
            rejected = subprocess.run([str(binary), "--output", "pipewire", "--device", "absent-test-sink", str(path)],
                                      env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=8)
            assert rejected.returncode != 0

            # Detect a live connection whose graph no longer consumes frames.
            busy = subprocess.Popen([str(binary), "--output", "pipewire", "--device", "test-sink", "--loop", str(path)],
                                    env=env, stdin=subprocess.DEVNULL, stdout=logs, stderr=logs)
            processes.append(busy)
            time.sleep(.3)
            assert busy.poll() is None
            server.send_signal(signal.SIGSTOP)
            try:
                assert busy.wait(timeout=8) != 0
            finally:
                server.send_signal(signal.SIGCONT)

            # Loss of the private server must end active playback with an error.
            busy = subprocess.Popen([str(binary), "--output", "pipewire", "--device", "test-sink", "--loop", str(path)],
                                    env=env, stdin=subprocess.DEVNULL, stdout=logs, stderr=logs)
            processes.append(busy)
            time.sleep(.3)
            stop(server)
            assert busy.wait(timeout=5) != 0
            print("PipeWire exclusive/busy sink, resampling, short EOF, looping and disconnect checks passed", flush=True)
        except BaseException:
            logs.flush()
            logs.seek(0)
            print(logs.read()[-10000:])
            raise
        finally:
            for process in reversed(processes):
                stop(process)
            if manager is not None:
                # Also stop the private D-Bus process launched by dbus-run-session.
                try:
                    os.killpg(manager.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                manager.wait(timeout=5)
            logs.close()


if __name__ == "__main__":
    main()
