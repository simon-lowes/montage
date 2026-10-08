#!/usr/bin/env bash
# Downloads the models Montage fetches on first use, for CI and offline
# installs, checking each file's SHA-256:
#   <dir>/edgetam-video  object masks       (MONTAGE_OBJECT_MODEL)
#   <dir>/speakers       speaker labels     (MONTAGE_SPEAKER_MODEL)
#   <dir>/clip-vit-b32   Find Shots         (MONTAGE_VISUAL_MODEL)
#   <dir>/speech-enhance Enhance Speech     (MONTAGE_SPEECH_MODEL)
#   <dir>/upscale        Super Scale        (MONTAGE_UPSCALE_MODEL)
#   <dir>/faces          Find People        (MONTAGE_FACE_MODEL)
#   <dir>/depth          depth effects      (MONTAGE_DEPTH_MODEL)
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
CLIP=https://huggingface.co/Xenova/clip-vit-base-patch32/resolve/d15189d7028b43f1d3e65039190477f6af591c2a
fetch clip-vit-b32 text_model_q4.onnx "$CLIP/onnx/text_model_q4.onnx" e4ccd15d806b8af841a036884b42034535d58e04c52df00f56e73f72d3c166d5
fetch clip-vit-b32 vision_model_q4.onnx "$CLIP/onnx/vision_model_q4.onnx" 0769eb1d2f6f68927bbfa6e5330df4c4c3c112f89cd43eae28baafa9e6bd34b4
fetch clip-vit-b32 vocab.json "$CLIP/vocab.json" 5047b556ce86ccaf6aa22b3ffccfc52d391ea4accdab9c2f2407da5b742d4363
fetch clip-vit-b32 merges.txt "$CLIP/merges.txt" 9fd691f7c8039210e0fced15865466c65820d09b63988b0174bfe25de299051a
fetch speech-enhance deepfilternet3.onnx \
  https://huggingface.co/kimtos-labs/denoiser-dfn3/resolve/888f33c41851d7168ae31c3637c323b9e2f3c2a4/denoiser_model.onnx \
  fe5eb64fa2e4154c83f8e4935e82871c850c154387ee892e0ab65fe179e7d8c9
fetch upscale realesr-general-x4v3.onnx \
  https://huggingface.co/jonathanst29/tinier-upscale-models/resolve/899dc1e4b22bbf1955c2f1739c085edc080cb366/realesr-general-x4v3.onnx \
  924ebad6532777303582d4ce7811849b88869231a3ae7093e0f21200249df8d5
fetch faces face_detection_yunet_2023mar.onnx \
  https://huggingface.co/opencv/face_detection_yunet/resolve/3cc26e7f1014a5ee5d74a42acee58bafc9d0a310/face_detection_yunet_2023mar.onnx \
  8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4
fetch faces face_recognition_sface_2021dec.onnx \
  https://huggingface.co/opencv/face_recognition_sface/resolve/3d7082438a6e4551e840c9b2bb60b71e8da4b524/face_recognition_sface_2021dec.onnx \
  0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79
fetch depth depth_anything_v2_small.onnx \
  https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/onnx/model.onnx \
  afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c
echo "Models in $DIR: MONTAGE_OBJECT_MODEL=$DIR/edgetam-video MONTAGE_SPEAKER_MODEL=$DIR/speakers MONTAGE_VISUAL_MODEL=$DIR/clip-vit-b32 MONTAGE_SPEECH_MODEL=$DIR/speech-enhance MONTAGE_UPSCALE_MODEL=$DIR/upscale MONTAGE_FACE_MODEL=$DIR/faces MONTAGE_DEPTH_MODEL=$DIR/depth"
