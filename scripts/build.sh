#!/usr/bin/env bash
# Cross-compile OpGrid for Ableton Move (aarch64 Linux) and package it.
# Uses aarch64-linux-gnu-gcc if installed, otherwise a Debian bookworm
# Docker image (same glibc as Schwung's own builds).
set -euo pipefail
cd "$(dirname "$0")/.."

OUT=build/opgrid
CFLAGS="-O3 -shared -fPIC -Wall -Wextra -Iinclude -Isrc -mcpu=cortex-a53"
SRC="src/og_engine.c src/og_machines.c src/og_seq.c src/og_fx.c src/og_rec.c src/og_song.c src/og_plugin.c"

rm -rf "$OUT"
mkdir -p "$OUT"
cp module/module.json "$OUT/"
cp src/ui.js "$OUT/ui.js"
cp module/help.json "$OUT/"
cp module/sounds.json "$OUT/"

if command -v aarch64-linux-gnu-gcc >/dev/null; then
    aarch64-linux-gnu-gcc $CFLAGS $SRC -o "$OUT/dsp.so" -lm
else
    docker run --rm -v "$PWD":/w -w /w debian:bookworm bash -c "
        apt-get update -qq && apt-get install -y -qq gcc-aarch64-linux-gnu >/dev/null &&
        aarch64-linux-gnu-gcc $CFLAGS $SRC -o $OUT/dsp.so -lm"
fi

# The Move runs Debian bookworm (glibc 2.36): refuse newer symbol versions.
if command -v aarch64-linux-gnu-objdump >/dev/null; then
    newest=$(aarch64-linux-gnu-objdump -T "$OUT/dsp.so" | grep -o 'GLIBC_[0-9.]*' | sort -V | tail -1)
    echo "newest glibc symbol: $newest"
    if [ "$(printf '%s\nGLIBC_2.36\n' "$newest" | sort -V | tail -1)" != "GLIBC_2.36" ]; then
        echo "ERROR: dsp.so needs $newest, newer than the Move's glibc 2.36" >&2
        exit 1
    fi
fi

tar --owner=0 --group=0 -czf build/opgrid-module.tar.gz -C build opgrid
echo "Built build/opgrid-module.tar.gz:"
tar -tzf build/opgrid-module.tar.gz
