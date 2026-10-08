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
#   <dir>/rife           AI slow motion     (MONTAGE_RIFE_MODEL)
#   <dir>/matte          Remove Background  (MONTAGE_MATTE_MODEL)
#   <dir>/tts            Generate Voiceover (MONTAGE_TTS_MODEL)
#   <dir>/inpaint        Object Removal     (MONTAGE_INPAINT_MODEL)
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
fetch rife RIFEv4.26_0921.zip \
  https://huggingface.co/hzwer/RIFE/resolve/01fdc7e97404120c243c3ea7b427046e5dc7643e/RIFEv4.26_0921.zip \
  1fa9b9cda3d9b8c3e301359e2595960902f97bf926c08598b0e9957a3f3f760e
fetch matte modnet.onnx \
  https://huggingface.co/Xenova/modnet/resolve/fa2fa546052fba4c08921230a26cc69a333fca12/onnx/model.onnx \
  07c308cf0fc7e6e8b2065a12ed7fc07e1de8febb7dc7839d7b7f15dd66584df9
KOKORO=https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/1939ad2a8e416c0acfeecc08a694d14ef25f2231
fetch tts kokoro-v1.0-fp16.onnx "$KOKORO/onnx/model_fp16.onnx" ba4527a874b42b21e35f468c10d326fdff3c7fc8cac1f85e9eb6c0dfc35c334a
fetch tts tokenizer.json "$KOKORO/tokenizer.json" 77a02c8e164413299b4b4c403b14f8e0e1c1b727db4d46a09d6327b861060a34
fetch tts misaki-0.7.4-py3-none-any.whl \
  https://files.pythonhosted.org/packages/06/e9/f092172a37c0994bf1e57e0d2f3b16bd18f63b98744d6039f18a055c2ad5/misaki-0.7.4-py3-none-any.whl \
  f9cf1afc0a2c0e77dadf02ae8203d3a7c312ee67b235d3b658c08367919b17c1
fetch tts af_heart.bin "$KOKORO/voices/af_heart.bin" d583ccff3cdca2f7fae535cb998ac07e9fcb90f09737b9a41fa2734ec44a8f0b
fetch tts af_bella.bin "$KOKORO/voices/af_bella.bin" f69d836209b78eb8c66e75e3cda491e26ea838a3674257e9d4e5703cbaf55c8b
fetch tts af_sarah.bin "$KOKORO/voices/af_sarah.bin" 4409fbc125afabacc615d94db5398d847006a737b0247d6892b7a9a0007a2f0a
fetch tts am_michael.bin "$KOKORO/voices/am_michael.bin" 1d1f21dd8da39c30705cd4c75d039d265e9bc4a2a93ed09bc9e1b1225eb95ba1
fetch tts am_adam.bin "$KOKORO/voices/am_adam.bin" 162b035ed91cfc48b6046982184c645f72edcdd1b82843347f605d7bf7b15716
fetch tts am_puck.bin "$KOKORO/voices/am_puck.bin" fcf73c989033e9233e0b98713eca600c8c74dcc1614b37009d5450ff4a2274a0
fetch tts bf_emma.bin "$KOKORO/voices/bf_emma.bin" 669fe0647f9dd04fcab92f1439a40eeb4c8b4ab1f82e4996fe3d918ce4a63b73
fetch tts bf_isabella.bin "$KOKORO/voices/bf_isabella.bin" 3754352c4aaa46d17f27654ab7518d65b62ad6163a0f55a5f4330c2da2c4e94f
fetch tts bm_george.bin "$KOKORO/voices/bm_george.bin" c4b235a4c1f2cd3b939fed08b899ce9385638b763f7b73a59616c4fc9bd6c9bc
fetch tts bm_lewis.bin "$KOKORO/voices/bm_lewis.bin" b8f671cef828c30e66fdf0b0756a76bba58f6bb3398cbbf27058642acbcedb97
fetch inpaint lama_fp32.onnx \
  https://huggingface.co/Carve/LaMa-ONNX/resolve/c3c0c9e468934d62e79c329e35d82dd09ff8c444/lama_fp32.onnx \
  1faef5301d78db7dda502fe59966957ec4b79dd64e16f03ed96913c7a4eb68d6
echo "Models in $DIR: MONTAGE_OBJECT_MODEL=$DIR/edgetam-video MONTAGE_SPEAKER_MODEL=$DIR/speakers MONTAGE_VISUAL_MODEL=$DIR/clip-vit-b32 MONTAGE_SPEECH_MODEL=$DIR/speech-enhance MONTAGE_UPSCALE_MODEL=$DIR/upscale MONTAGE_FACE_MODEL=$DIR/faces MONTAGE_DEPTH_MODEL=$DIR/depth MONTAGE_RIFE_MODEL=$DIR/rife MONTAGE_MATTE_MODEL=$DIR/matte MONTAGE_TTS_MODEL=$DIR/tts MONTAGE_INPAINT_MODEL=$DIR/inpaint"
