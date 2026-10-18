#!/bin/sh
# Headless boot smoke test: screenshots at frames 120 and 600.
# usage: tools/boot_test.sh ROM OUTDIR   (THPS1 shows the Activision logo by ~600)
set -e
ROM="$1"; OUT="$2"
mkdir -p "$OUT"
timeout 240 mupen64plus --noosd --resolution 640x480 --testshots 120,600 \
  --sshotdir "$OUT" "$ROM" > "$OUT/mupen.log" 2>&1 || true
ls -la "$OUT"
