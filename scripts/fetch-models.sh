#!/usr/bin/env bash
# Downloads the models Montage fetches on first use, for CI and offline
# installs, checking each file's SHA-256:
#   <dir>/edgetam-video  object masks       (MONTAGE_OBJECT_MODEL)
#   <dir>/speakers       speaker labels     (MONTAGE_SPEAKER_MODEL)
#
#   scripts/fetch-models.sh [dir]      (default: ./models)
set -euo pipefail
DIR=${1:-models}
sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
fetch() {  # folder, file name, URL, SHA-256
  mkdir -p "$DIR/$1"
  local f="$DIR/$1/$2"
  if [ -s "$f" ] && [ "$(sha256 "$f")" = "$4" ]; then return; fi
  curl -fsSL --retry 3 --max-time 600 -o "$f.part" "$3"
  local got
  got=$(sha256 "$f.part")
  if [ "$got" != "$4" ]; then
    echo "$2: SHA-256 $got, expected $4" >&2
    rm -f "$f.part"
    exit 1
  fi
  mv "$f.part" "$f"
}
EDGETAM=https://huggingface.co/jax-image-tools/edgetam-video-onnx/resolve/8ca3d3e4169938e65b552cdf14542fde25e8badb
fetch edgetam-video vision_encoder.onnx "$EDGETAM/vision_encoder.onnx" 57fe1a2b3d500813fe4987c4209b856920f187ab0b7723d81586b607ea2cffc1
fetch edgetam-video mask_decoder.onnx "$EDGETAM/mask_decoder.onnx" ee24ee1cae6ecc71889912c1893e08b09f645bb63875108ccd180f7218a3647d
fetch edgetam-video memory_attention.onnx "$EDGETAM/memory_attention.onnx" 6477d775639905945949508c08a2c652d01e5933c0eb00a02225ae5f6aa1602c
fetch edgetam-video memory_encoder.onnx "$EDGETAM/memory_encoder.onnx" 035b30f8fb1f7c99211ada6eb9d72f0819de819ef94310b14e8147dc97f120ec
fetch speakers pyannote-segmentation-3.0.onnx \
  https://huggingface.co/csukuangfj/sherpa-onnx-pyannote-segmentation-3-0/resolve/9403a6902bb58e3d5ae8c7e77c3422de279db2e0/model.onnx \
  220ad67ca923bef2fa91f2390c786097bf305bceb5e261d4af67b38e938e1079
fetch speakers campplus-voxceleb.onnx \
  https://huggingface.co/csukuangfj/speaker-embedding-models/resolve/0743f301363dec56491a490f6d6cbc9d67f9a3bf/3dspeaker_speech_campplus_sv_en_voxceleb_16k.onnx \
  357a834f702b80161e5b981182c038e18553c1f2ca752ed6cec2052365d4129b
echo "Models in $DIR: MONTAGE_OBJECT_MODEL=$DIR/edgetam-video MONTAGE_SPEAKER_MODEL=$DIR/speakers"
