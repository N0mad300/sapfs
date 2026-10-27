#!/usr/bin/env bash
# Build sapfs with ALSA and optional PipeWire/JACK output.
set -euo pipefail
cd "$(dirname "$0")"

if ! pkg-config --exists alsa; then
    echo "ALSA development files are required (Debian/Ubuntu: libasound2-dev)." >&2
    exit 1
fi

read -r -a ALSA_CFLAGS <<< "$(pkg-config --cflags alsa)"
read -r -a ALSA_LIBS <<< "$(pkg-config --libs alsa)"
PIPEWIRE_SRC=()
PIPEWIRE_CFLAGS=()
PIPEWIRE_LIBS=()
case "${WITH_PIPEWIRE:-auto}" in
    auto|0|1) ;;
    *) echo "WITH_PIPEWIRE must be auto, 0, or 1" >&2; exit 1 ;;
esac
if [ "${WITH_PIPEWIRE:-auto}" != 0 ] && pkg-config --atleast-version=0.3.50 libpipewire-0.3; then
    PIPEWIRE_SRC=(src/audio_output/pipewire_output.c)
    read -r -a PIPEWIRE_CFLAGS <<< "$(pkg-config --cflags libpipewire-0.3)"
    PIPEWIRE_CFLAGS+=(-DSAPFS_HAVE_PIPEWIRE)
    read -r -a PIPEWIRE_LIBS <<< "$(pkg-config --libs libpipewire-0.3)"
elif [ "${WITH_PIPEWIRE:-auto}" = 1 ]; then
    echo "PipeWire >= 0.3.50 development files are required (Debian/Ubuntu: libpipewire-0.3-dev)." >&2
    exit 1
fi
JACK_SRC=()
JACK_CFLAGS=()
JACK_LIBS=()
case "${WITH_JACK:-auto}" in
    auto|0|1) ;;
    *) echo "WITH_JACK must be auto, 0, or 1" >&2; exit 1 ;;
esac
if [ "${WITH_JACK:-auto}" != 0 ] && pkg-config --exists jack; then
    JACK_SRC=(src/audio_output/jack_output.c)
    read -r -a JACK_CFLAGS <<< "$(pkg-config --cflags jack)"
    JACK_CFLAGS+=(-DSAPFS_HAVE_JACK)
    read -r -a JACK_LIBS <<< "$(pkg-config --libs jack)"
elif [ "${WITH_JACK:-auto}" = 1 ]; then
    echo "JACK development files are required (Debian/Ubuntu: libjack-jackd2-dev)." >&2
    exit 1
fi
read -r -a EXTRA_CFLAGS <<< "${CFLAGS:-}"
read -r -a EXTRA_LDFLAGS <<< "${LDFLAGS:-}"
mkdir -p dist
"${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -O2 \
    "${EXTRA_CFLAGS[@]}" "${ALSA_CFLAGS[@]}" "${PIPEWIRE_CFLAGS[@]}" "${JACK_CFLAGS[@]}" \
    src/main.c src/audio_decoder/audio_decoder.c \
    src/audio_decoder/wave_parser.c src/audio_decoder/flac_parser.c \
    src/audio_decoder/mp3_parser.c src/audio_converter.c \
    src/audio_output/audio_output.c src/audio_output/linux_output.c \
    src/audio_output/alsa_output.c "${PIPEWIRE_SRC[@]}" "${JACK_SRC[@]}" \
    "${EXTRA_LDFLAGS[@]}" "${ALSA_LIBS[@]}" "${PIPEWIRE_LIBS[@]}" "${JACK_LIBS[@]}" -pthread -ldl -o dist/sapfs
BACKENDS="ALSA"
if [ "${#PIPEWIRE_SRC[@]}" -gt 0 ]; then BACKENDS+=" + PipeWire"; fi
if [ "${#JACK_SRC[@]}" -gt 0 ]; then BACKENDS+=" + JACK"; fi
echo "Built dist/sapfs ($BACKENDS)"
