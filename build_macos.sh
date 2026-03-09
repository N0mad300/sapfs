#!/usr/bin/env bash
# build_macos.sh — Build sapfs on macOS (Intel or Apple Silicon)
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
CC=clang
CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O2"
OUTDIR=dist
OUT="$OUTDIR/sapfs"

SRC=(
    src/main.c
    src/audio_decoder/wave_parser.c
    src/audio_decoder/flac_parser.c
    src/audio_decoder/mp3_parser.c
    src/audio_decoder/audio_decoder.c
    src/audio_output/audio_output.c
    src/audio_output/coreaudio_output.c
    src/audio_output/ring_buffer.c
    src/audio_converter.c
)

FRAMEWORKS=(
    -framework CoreAudio
    -framework AudioToolbox
)

LIBS=(-lpthread)

mkdir -p "$OUTDIR"

echo "Building $OUT …"
$CC $CFLAGS \
    -o "$OUT" \
    "${SRC[@]}" \
    "${FRAMEWORKS[@]}" \
    "${LIBS[@]}"

echo "Done: $OUT"

# Show universal / architecture info
lipo -info "$OUT" 2>/dev/null || true
