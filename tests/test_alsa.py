#!/usr/bin/env python3
"""Run hardware-independent ALSA integration tests using its file/null plugins.

Requires a C compiler, pkg-config and ALSA development files. ffmpeg enables
additional FLAC and MP3 playback checks. No sound device is opened.
"""

import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import wave


ROOT = Path(__file__).resolve().parents[1]


def run(args, **kwargs):
    return subprocess.run(args, check=True, timeout=30, **kwargs)


def wav_file(path, bits, data, float_pcm=False, valid_bits=None):
    channels, rate = 2, 48000
    align = channels * (bits // 8)
    fmt = struct.pack("<HHIIHH", 3 if float_pcm else 1, channels, rate,
                      rate * align, align, bits)
    if valid_bits is not None:
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * align,
                          align, bits, 22, valid_bits, 3)
        fmt += bytes.fromhex("0100000000001000800000aa00389b71")
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    chunks += b"data" + struct.pack("<I", len(data)) + data
    path.write_bytes(b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks)


def main():
    os.chdir(ROOT)
    run(["bash", "build_linux.sh"])
    binary = ROOT / "dist/sapfs"
    with tempfile.TemporaryDirectory(prefix="sapfs-alsa-tests-") as tmp:
        tmp = Path(tmp)
        capture = tmp / "capture.raw"
        config = tmp / "alsa.conf"
        config.write_text('pcm.null { type null }\n'
                          'pcm.capture { type file slave.pcm "null" '
                          f'file "{capture}" format "raw" }}\n')
        env = dict(os.environ, ALSA_CONFIG_PATH=str(config))
        pkg_cflags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "alsa"], text=True))
        pkg_libs = shlex.split(subprocess.check_output(["pkg-config", "--libs", "alsa"], text=True))
        wrappers = ["snd_pcm_hw_params_test_format", "snd_pcm_hw_params_set_rate",
                    "snd_pcm_hw_params_get_sbits", "snd_pcm_pause",
                    "snd_pcm_writei", "snd_pcm_resume", "snd_pcm_delay"]
        run([os.environ.get("CC", "cc"), "-std=c11", "-D_DEFAULT_SOURCE",
             "-Wall", "-Wextra", "-Werror", "-g", "-Isrc", *pkg_cflags,
             *shlex.split(os.environ.get("CFLAGS", "")),
             "tests/alsa_output_test.c", "src/audio_output/audio_output.c",
             "src/audio_output/linux_output.c", "src/audio_output/alsa_output.c",
             *[f"-Wl,--wrap={name}" for name in wrappers],
             *shlex.split(os.environ.get("LDFLAGS", "")), *pkg_libs,
             "-o", str(tmp / "backend-test")])
        run([str(tmp / "backend-test"), str(capture)], env=env)

        def play(path, expected=None, options=()):
            result = run([str(binary), "--output", "alsa", "--shared", "--device",
                          "capture", *options, str(path)], env=env,
                         stdin=subprocess.DEVNULL, capture_output=True)
            assert b"100%" in result.stdout, result.stdout
            data = capture.read_bytes()
            if expected is not None:
                assert data == expected, f"{path.name}: {len(data)} vs {len(expected)} bytes or samples differ"
            return data

        for bits in (8, 16, 24, 32):
            width = bits // 8
            values = ([0, 1, 127, 128, 129, 255] if bits == 8 else
                      [-(1 << (bits - 1)), -1, 0, 1, (1 << (bits - 1)) - 1, 127])
            # More than two hardware buffers; includes low bits lost by float32.
            data = b"".join(v.to_bytes(width, "little", signed=bits != 8) for v in values) * 4000
            path = tmp / f"pcm{bits}.wav"
            wav_file(path, bits, data)
            play(path, data, ("--buffer", "10"))
            short = tmp / f"short{bits}.wav"
            wav_file(short, bits, data[:2 * width])
            play(short, data[:2 * width])
        floats = struct.pack("<8f", -1, -0.0, 0, 0.125, 0.5, 1, -0.75, 0.25)
        path = tmp / "float.wav"
        wav_file(path, 32, floats, float_pcm=True)
        play(path, floats)
        left_aligned = b"".join((v << 8).to_bytes(4, "little", signed=True)
                                for v in (-8388608, -1, 0, 1, 8388607, 0))
        path = tmp / "extensible24.wav"
        wav_file(path, 32, left_aligned, valid_bits=24)
        play(path, left_aligned)

        for options in (("--device", "null"), ("--exclusive", "--device", "null"),
                        ("--output", "pipewire"), ("--buffer", "12bad"),
                        ("--buffer", "9"), ("--buffer", "5001"),
                        ("--buffer",), ("--device",)):
            result = subprocess.run([str(binary), *options, str(path)], env=env,
                                    stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
            assert result.returncode != 0, options
        result = subprocess.run([str(binary), "--exclusive", "--device", "null", str(path)],
                                env=env, stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
        assert b"hardware PCM" in result.stderr

        if shutil.which("ffmpeg"):
            for bits in (16, 24):
                src = tmp / f"pcm{bits}.wav"
                dst = tmp / f"pcm{bits}.flac"
                run(["ffmpeg", "-v", "error", "-i", str(src), "-c:a", "flac", str(dst)])
                with wave.open(str(src)) as wave_input:
                    data = wave_input.readframes(wave_input.getnframes())
                if bits == 24:
                    data = b"".join(data[i:i + 3] + (b"\xff" if data[i + 2] & 0x80 else b"\x00")
                                    for i in range(0, len(data), 3))
                play(dst, data)
            mp3 = tmp / "test.mp3"
            run(["ffmpeg", "-v", "error", "-i", str(tmp / "pcm16.wav"), str(mp3)])
            assert len(play(mp3)) > 0
            print("FLAC 16/24-bit byte comparisons and MP3 playback passed")
        else:
            print("ffmpeg unavailable: FLAC/MP3 integration checks skipped")
    print("ALSA CLI and end-to-end PCM bit comparisons passed")


if __name__ == "__main__":
    main()
