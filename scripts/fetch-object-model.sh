#!/usr/bin/env bash
# Downloads the EdgeTAM model Montage uses for object masks into a folder
# (default: ./object-model) and checks each file's SHA-256. The app fetches the
# same files on first use; this is for CI and offline installs, where
# MONTAGE_OBJECT_MODEL then points at the folder.
#
#   scripts/fetch-object-model.sh [dir]
set -euo pipefail
DIR=${1:-object-model}
BASE=${MONTAGE_OBJECT_MODEL_URL:-https://huggingface.co/jax-image-tools/edgetam-video-onnx/resolve/8ca3d3e4169938e65b552cdf14542fde25e8badb}
mkdir -p "$DIR"
sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
while read -r name hash; do
  f="$DIR/$name"
  if [ -s "$f" ] && [ "$(sha256 "$f")" = "$hash" ]; then continue; fi
  curl -fsSL --retry 3 --max-time 600 -o "$f.part" "$BASE/$name"
  got=$(sha256 "$f.part")
  if [ "$got" != "$hash" ]; then
    echo "$name: SHA-256 $got, expected $hash" >&2
    rm -f "$f.part"
    exit 1
  fi
  mv "$f.part" "$f"
done <<'LIST'
vision_encoder.onnx 57fe1a2b3d500813fe4987c4209b856920f187ab0b7723d81586b607ea2cffc1
mask_decoder.onnx ee24ee1cae6ecc71889912c1893e08b09f645bb63875108ccd180f7218a3647d
memory_attention.onnx 6477d775639905945949508c08a2c652d01e5933c0eb00a02225ae5f6aa1602c
memory_encoder.onnx 035b30f8fb1f7c99211ada6eb9d72f0819de819ef94310b14e8147dc97f120ec
LIST
echo "Object model in $DIR"
