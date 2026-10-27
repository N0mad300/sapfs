#!/usr/bin/env python3
"""JACK integration tests using a private named JACK2 dummy server; no hardware.

Requires JACK development files and jackd2. JACKD may specify a jackd executable
outside PATH. Normal CFLAGS, LDFLAGS, CC, and pkg-config overrides are respected.
"""

import os
from pathlib import Path
import select
import shlex
import shutil
import signal
import struct
import subprocess
import tempfile
import time

from test_alsa import wav_file

ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    try:
        return subprocess.run(args, timeout=20, check=True, **kwargs)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        for text in (error.stdout, error.stderr):
            if text:
                print(text.decode(errors="replace") if isinstance(text, bytes) else text)
        raise


def stop(process):
    if process.poll() is None:
        process.send_signal(signal.SIGINT)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=5)


def main():
    os.chdir(ROOT)
    jackd = os.environ.get("JACKD") or shutil.which("jackd")
    if not jackd:
        raise SystemExit("JACK2 dummy server required; install jackd2 or set JACKD")
    run(["bash", "build_linux.sh"], env=dict(os.environ, WITH_JACK="1"))
    binary = ROOT / "dist/sapfs"
    with tempfile.TemporaryDirectory(prefix="sapfs-jack-test-") as directory:
        tmp = Path(directory)
        server_name = f"sapfs-test-{os.getpid()}"
        env = dict(os.environ, JACK_DEFAULT_SERVER=server_name, JACK_NO_START_SERVER="1",
                   XDG_RUNTIME_DIR=str(tmp), XDG_CONFIG_HOME=str(tmp / "config"))
        log = open(tmp / "jack.log", "w+")
        processes = []
        server = subprocess.Popen([jackd, "-r", "-n", server_name, "-t", "2000", "-d", "dummy",
                                   "-r", "48000", "-p", "128", "-w", "2667", "-P", "8", "-C", "0"],
                                  env=env, stdout=log, stderr=log)
        processes.append(server)
        try:
            deadline = time.monotonic() + 8
            while True:
                ports = subprocess.run([str(binary), "--list-jack-ports"], env=env, capture_output=True, timeout=5)
                if ports.returncode == 0:
                    break
                if server.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError("Private JACK dummy server failed to start")
                time.sleep(.05)
            assert b"system:playback_1" in ports.stdout
            run([str(binary), "--list-jack-ports", "--jack-server", server_name],
                env=dict(env, JACK_DEFAULT_SERVER="absent-environment-server"), capture_output=True)
            cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "alsa", "jack"], text=True))
            libs = shlex.split(subprocess.check_output(["pkg-config", "--libs", "alsa", "jack"], text=True))
            helper = tmp / "jack-api-test"
            run([os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE", "-DSAPFS_HAVE_JACK",
                 "-Wall", "-Wextra", "-Werror", "-g", "-Isrc", *cflags,
                 *shlex.split(os.environ.get("CFLAGS", "")), "tests/jack_output_test.c",
                 "src/audio_output/audio_output.c", "src/audio_output/linux_output.c", "src/audio_output/alsa_output.c",
                 "src/audio_output/jack_output.c", "-Wl,--wrap=jack_set_sample_rate_callback",
                 "-Wl,--wrap=jack_set_buffer_size_callback", "-Wl,--wrap=jack_set_xrun_callback",
                 *shlex.split(os.environ.get("LDFLAGS", "")), *libs, "-pthread", "-o", str(helper)])
            run([str(helper)], env=env)

            def play(path, options=(), check=True):
                args = [str(binary), "--output", "jack", *options, str(path)]
                if check:
                    return run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True)
                return subprocess.run(args, env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=12)

            def capture_playback(path, expected, channels=2, options=()):
                raw = tmp / f"capture-{path.stem}.raw"
                capture = subprocess.Popen([str(helper), str(raw), str(channels)], env=env,
                                           stdout=subprocess.PIPE, stderr=log)
                processes.append(capture)
                ready, _, _ = select.select([capture.stdout], [], [], 5)
                assert ready and capture.stdout.readline().strip() == b"READY"
                play(path, ("--device", "^capture:in_", *options))
                time.sleep(.02)
                stop(capture)
                actual = raw.read_bytes()
                assert expected in actual, f"JACK {path.name}: missing/changed/truncated samples ({len(actual)} captured bytes)"

            for bits in (8, 16, 24, 32):
                state = 0x12345678
                values = []
                for _ in range(4800 * 2):
                    state = (state * 1664525 + 1013904223) & 0xffffffff
                    values.append((state >> (32 - bits)) - (1 << (bits - 1)))
                data = b"".join((v + 128).to_bytes(1, "little") if bits == 8 else
                                v.to_bytes(bits // 8, "little", signed=True) for v in values)
                expected = b"".join(struct.pack("<f", v / (1 << (bits - 1))) for v in values)
                path = tmp / f"pcm{bits}.wav"
                wav_file(path, bits, data)
                capture_playback(path, expected, options=("--buffer", "10"))
                if bits == 24:
                    padded = tmp / "padded24.wav"
                    wav_file(padded, 24, b"".join(v.to_bytes(4, "little", signed=True) for v in values))
                    header = bytearray(padded.read_bytes())
                    struct.pack_into("<I", header, 28, 48000 * 8); struct.pack_into("<H", header, 32, 8)
                    padded.write_bytes(header)
                    capture_playback(padded, expected)
                    ext = tmp / "extensible24.wav"
                    wav_file(ext, 32, b"".join((v << 8).to_bytes(4, "little", signed=True) for v in values), valid_bits=24)
                    capture_playback(ext, expected)
            floats = struct.pack("<8f", -.875, .625, -.125, .25, 0, -.0, .5, -.5)
            path = tmp / "float.wav"; wav_file(path, 32, floats, float_pcm=True)
            capture_playback(path, floats)
            path = tmp / "one.wav"; wav_file(path, 16, b"\x01\x00\xff\xff")
            capture_playback(path, struct.pack("<2f", 1 / 32768, -1 / 32768))
            # All eight source channels preserve order and separate JACK ports.
            path = tmp / "eight.wav"
            data = struct.pack("<8h", 100, -200, 300, -400, 500, -600, 700, -800) * 500
            fmt = struct.pack("<HHIIHH", 1, 8, 48000, 48000 * 16, 16, 16)
            chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data
            path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)
            expected = struct.pack("<8f", *[v / 32768 for v in (100, -200, 300, -400, 500, -600, 700, -800)]) * 500
            capture_playback(path, expected, channels=8)
            print("JACK PCM8/16/24/32, float, 24-bit layouts, short EOF and eight-channel comparisons passed", flush=True)

            path = tmp / "pcm16.wav"
            play(path) # Automatic physical playback-port routing.
            play(path, ("--jack-port", "system:playback_2", "--jack-port", "system:playback_1"))
            play(path, ("--jack-server", server_name, "--jack-name", "sapfs-custom"))
            empty = tmp / "empty.wav"; wav_file(empty, 16, b""); play(empty)
            mismatch = tmp / "44100.wav"
            data = bytearray(path.read_bytes()); struct.pack_into("<I", data, 24, 44100); struct.pack_into("<I", data, 28, 44100 * 4)
            mismatch.write_bytes(data)
            failed = play(mismatch, check=False)
            assert failed.returncode != 0 and b"48000 Hz" in failed.stderr
            for options in (("--exclusive",), ("--device", "absent-destination"),
                            ("--jack-port", "system:playback_1"),
                            ("--jack-no-connect", "--device", ".*")):
                assert play(path, options, check=False).returncode != 0

            busy = subprocess.Popen([str(binary), "--output", "jack", "--loop", str(path)],
                                    env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            processes.append(busy)
            time.sleep(.3); assert busy.poll() is None
            play(path) # Shared clients coexist.
            stop(server)
            assert busy.wait(timeout=8) != 0
            failed = play(path, ("--jack-server", "absent-sapfs-test-server"), check=False)
            assert failed.returncode != 0 and b"start a JACK server first" in failed.stderr
            print("JACK CLI routing, shared clients, no-autostart, rate/exclusive rejection and shutdown checks passed", flush=True)
        except BaseException:
            log.flush(); log.seek(0); print(log.read()[-10000:])
            raise
        finally:
            for process in reversed(processes):
                stop(process)
            log.close()


if __name__ == "__main__":
    main()
