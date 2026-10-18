#!/usr/bin/env bash
# Downloads the models Montage fetches on first use, for CI and offline
# installs, checking each file's SHA-256:
#   <dir>/edgetam-video  object masks       (MONTAGE_OBJECT_MODEL)
#   <dir>/speakers       speaker labels     (MONTAGE_SPEAKER_MODEL)
#   <dir>/clip-vit-b32   Find Shots         (MONTAGE_VISUAL_MODEL)
#   <dir>/speech-enhance Enhance Speech     (MONTAGE_SPEECH_MODEL)
#   <dir>/upscale        Super Scale        (MONTAGE_UPSCALE_MODEL)
#   <dir>/deblur         Motion Deblur      (MONTAGE_DEBLUR_MODEL)
#   <dir>/faces          Find People, Face Shape, Look at Lens (MONTAGE_FACE_MODEL)
#   <dir>/depth          depth effects      (MONTAGE_DEPTH_MODEL)
#   <dir>/video-depth    steady video depth (MONTAGE_VIDEO_DEPTH_MODEL)
#   <dir>/rife           AI slow motion     (MONTAGE_RIFE_MODEL)
#   <dir>/matte          Remove Background  (MONTAGE_MATTE_MODEL)
#   <dir>/tts            Generate Voiceover (MONTAGE_TTS_MODEL)
#   <dir>/inpaint        Object Removal     (MONTAGE_INPAINT_MODEL)
#   <dir>/minilm-qa      Find What's Said   (MONTAGE_SENTENCE_MODEL)
#   <dir>/ocr            Reading text       (MONTAGE_OCR_MODEL)
#   <dir>/sound          What's Heard       (MONTAGE_SOUND_MODEL)
#   <dir>/stems          Separate Stems     (MONTAGE_STEM_MODELS; 4 stems, and 2 and 5 with --stems; dialogue,
#                                           music and effects with --dme: 447 MB, CC BY-SA 4.0)
#   <dir>/voice-clone    Cloned voices      (MONTAGE_VOICE_CLONE_MODEL; 1.6 GB, only with --voice-clone)
#   <dir>/lipsync        Lip Sync           (MONTAGE_LIPSYNC_MODEL; 3.9 GB, only with --lipsync)
#   <dir>/lensfun-db     Lens Correction    (MONTAGE_LENS_DB; lensfun's lens profiles, CC BY-SA 3.0)
#   <dir>/sadie-ku100    binaural ambisonics (MONTAGE_HRTF_DIR; SADIE II's KU100 HRIRs, Apache-2.0; only with --hrtf)
#
#   scripts/fetch-models.sh [dir] [--voice-clone] [--stems] [--dme] [--hrtf] [--lipsync]      (default: ./models)
set -euo pipefail
VOICE_CLONE=0
ALL_STEMS=0
DME=0
HRTF=0
LIPSYNC=0
ARGS=()
for a in "$@"; do
  if [ "$a" = "--voice-clone" ]; then VOICE_CLONE=1; elif [ "$a" = "--stems" ]; then ALL_STEMS=1; elif [ "$a" = "--dme" ]; then DME=1; elif [ "$a" = "--hrtf" ]; then HRTF=1; elif [ "$a" = "--lipsync" ]; then LIPSYNC=1; else ARGS+=("$a"); fi
done
DIR=${ARGS[0]:-models}
sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }
fetch() {  # folder, file name, URL, SHA-256[, seconds allowed]
  mkdir -p "$DIR/$1"
  local f="$DIR/$1/$2"
  if [ -s "$f" ] && [ "$(sha256 "$f")" = "$4" ]; then return; fi
  curl -fsSL --retry 3 --max-time "${5:-600}" -o "$f.part" "$3"
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
fetch deblur deblurring_nafnet_2025may.onnx \
  https://huggingface.co/opencv/deblurring_nafnet/resolve/f1f255116cdb628a311d2b5749871189a4639d84/deblurring_nafnet_2025may.onnx \
  07263f416febecce10193dd648e950b22e397cf521eedab1a114ef77b2bc9587
fetch faces face_detection_yunet_2023mar.onnx \
  https://huggingface.co/opencv/face_detection_yunet/resolve/3cc26e7f1014a5ee5d74a42acee58bafc9d0a310/face_detection_yunet_2023mar.onnx \
  8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4
fetch faces face_recognition_sface_2021dec.onnx \
  https://huggingface.co/opencv/face_recognition_sface/resolve/3d7082438a6e4551e840c9b2bb60b71e8da4b524/face_recognition_sface_2021dec.onnx \
  0ba9fbfa01b5270c96627c4ef784da859931e02f04419c829e83484087c34e79
fetch faces face_landmarks.onnx \
  https://huggingface.co/naklitechie/face-landmarks-onnx/resolve/575c33816c840c5b156398adb485e4f8d138adc2/face_landmarks.onnx \
  f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c
fetch depth depth_anything_v2_small.onnx \
  https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/4472b7362082ad9968fee890ca0f1e5aca36b93d/onnx/model.onnx \
  afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c
fetch video-depth video_depth_anything_vits.pth \
  https://huggingface.co/depth-anything/Video-Depth-Anything-Small/resolve/256875362cff76724b920335dfb4b29dd611f66e/video_depth_anything_vits.pth \
  13379300b739e659f076a59d52e9801bd8d38c541a7e71f73bbca4dcfb013609
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
MINILM=https://huggingface.co/Xenova/multi-qa-MiniLM-L6-cos-v1/resolve/3c96c1df3fec9a98a5de76e80877c8bec18498f6
fetch minilm-qa model_q4.onnx "$MINILM/onnx/model_q4.onnx" e798e950284f27b1cc028ab16b4175b997126dd3091304e4355e318c24c89bbc
fetch minilm-qa vocab.txt "$MINILM/vocab.txt" 07eced375cec144d27c900241f3e339478dec958f92fddbc551f295c992038a3
OCRDET=https://huggingface.co/SWHL/RapidOCR/resolve/1cfba2e90fc938db55889873735088de210cc173/PP-OCRv4
OCRREC=https://huggingface.co/monkt/paddleocr-onnx/resolve/7b02d0a30a07ba2b92ad1ff5a8941ae2c633de65/languages
fetch ocr ch_PP-OCRv4_det_infer.onnx "$OCRDET/ch_PP-OCRv4_det_infer.onnx" d2a7720d45a54257208b1e13e36a8479894cb74155a5efe29462512d42f49da9
fetch ocr en_rec.onnx "$OCRREC/english/rec.onnx" 4e16deb22c4da6468bdca539b2cd3c8687825538b67109177c47d359ab994cd7
fetch ocr en_dict.txt "$OCRREC/english/dict.txt" e025a66d31f327ba0c232e03f407ae8d105e1e709e7ccb3f408aa778c24e70d6
fetch ocr latin_rec.onnx "$OCRREC/latin/rec.onnx" 614ffc2d6d3902d360fad7f1b0dd455ee45e877069d14c4e51a99dc4ef144409
fetch ocr latin_dict.txt "$OCRREC/latin/dict.txt" 3c0a8a79b612653c25f765271714f71281e4e955962c153e272b7b8c1d2b13ff
CLAP=https://huggingface.co/Xenova/clap-htsat-unfused/resolve/c28f2883575e590e04d3146ff0713c2448d691ba
fetch sound audio_model.onnx "$CLAP/onnx/audio_model.onnx" a1c2b43c44f71e0fa841a4b86700886c199bf87699ea45632c4d831bc6c88957
fetch sound text_model_fp16.onnx "$CLAP/onnx/text_model_fp16.onnx" c1fd1e4cdd02acbcacaafe7a8e608dcfc8f84a00bfea3e2ca7df710957b4c3a5
fetch sound vocab.json "$CLAP/vocab.json" ed19656ea1707df69134c4af35c8ceda2cc9860bf2c3495026153a133670ab5e
fetch sound merges.txt "$CLAP/merges.txt" 1ce1664773c50f3e0cc8842619a93edc4624525b728b188a9e0be33b7726adc5
# Spleeter's own release (Deezer Research, MIT).
SPLEETER=https://github.com/deezer/spleeter/releases/download/v1.4.0
fetch stems/spleeter-4stems 4stems.tar.gz "$SPLEETER/4stems.tar.gz" 3adb4a50ad4eb18c7c4d65fcf4cf2367a07d48408a5eb7d03cd20067429dfaa8
if [ "$ALL_STEMS" = 1 ]; then
  fetch stems/spleeter-2stems 2stems.tar.gz "$SPLEETER/2stems.tar.gz" f3a90b39dd2874269e8b05a48a86745df897b848c61f3958efc80a39152bd692
  fetch stems/spleeter-5stems 5stems.tar.gz "$SPLEETER/5stems.tar.gz" 25a1e87eb5f75cc72a4d2d5467a0a50ac75f05611f877c278793742513cc7218
fi
if [ "$DME" = 1 ]; then
  # Bandit v2's English model (Watcharasupat, Wu and Lerch; CC BY-SA 4.0), from the authors' Zenodo record.
  fetch stems/bandit-v2-eng checkpoint-eng.ckpt https://zenodo.org/records/12701995/files/checkpoint-eng.ckpt \
    3d14c48399ddb42be6fca6c316adc1b7d67d0e648e4e937359811aa6fa8b8cd2 1800
fi
if [ "$VOICE_CLONE" = 1 ]; then
  CHATTERBOX=https://huggingface.co/onnx-community/chatterbox-multilingual-ONNX/resolve/452d3f434aa592098f1eedac9099f33642ab2da5
  fetch voice-clone speech_encoder.onnx "$CHATTERBOX/onnx/speech_encoder.onnx" 8f1c8a0f89b77bf9cd5dd8f2e034eb2c79dc00fe70d41196b28c257643b00ccb
  fetch voice-clone speech_encoder.onnx_data "$CHATTERBOX/onnx/speech_encoder.onnx_data" 92f8f290fc9720e169bc2412c507209e20b03f6564bc3243739e25c56f7dfb8f
  fetch voice-clone embed_tokens.onnx "$CHATTERBOX/onnx/embed_tokens.onnx" f785819ca4f6271262d5bb8971d62796c3a909e3b031982c113dbe83a4c3b854
  fetch voice-clone embed_tokens.onnx_data "$CHATTERBOX/onnx/embed_tokens.onnx_data" 2a15f7dd73b2ee47f6edf87740324011594b5a528ed6471ae55e327ed6cad68c
  fetch voice-clone language_model_q4.onnx "$CHATTERBOX/onnx/language_model_q4.onnx" 7f8cdca83b2493536cbf3acf421199808a3d68736f55f4eabd20ef8a99da4313
  fetch voice-clone language_model_q4.onnx_data "$CHATTERBOX/onnx/language_model_q4.onnx_data" e79ab8784122a501718868b9631ff46e151c552d9b24e50f25d721f375e3526c
  fetch voice-clone conditional_decoder.onnx "$CHATTERBOX/onnx/conditional_decoder.onnx" 1656d0d31332bae1854839959a3139300ebb67c178651dfa3f8c5fbfa5351351
  fetch voice-clone conditional_decoder.onnx_data "$CHATTERBOX/onnx/conditional_decoder.onnx_data" 51d58345a272747665ec9d5bb61e01835258a940e321a288582ac4c18cf01b5a
  fetch voice-clone tokenizer.json "$CHATTERBOX/tokenizer.json" 29d48c4a178f6af3ad5130097c34744639e9294847b38a7b912c8c68027cb819
  fetch voice-clone Cangjie5_TC.json "$CHATTERBOX/Cangjie5_TC.json" 7073fd9de919443ae88e0bd2449917a65fe54898a4413ed1edcc4b67f28bce8c
  echo "Voice cloning model: MONTAGE_VOICE_CLONE_MODEL=$DIR/voice-clone"
fi
if [ "$LIPSYNC" = 1 ]; then
  # MuseTalk 1.5's UNet (MIT), Stability AI's sd-vae-ft-mse (MIT) and OpenAI's whisper-tiny (Apache-2.0), at the
  # revisions media/LipSync.cpp pins; the graphs that run them are written beside them on first use.
  fetch lipsync unet.pth https://huggingface.co/TMElyralab/MuseTalk/resolve/2bcb936e2fddb4d86db4c62fd45b387d0c061571/musetalkV15/unet.pth \
    7ebf6c98c181e20838e4c0054e96e944ac60d5d692cc01db42839fe11b787007 3600
  fetch lipsync sd-vae-ft-mse.safetensors \
    https://huggingface.co/stabilityai/sd-vae-ft-mse/resolve/31f26fdeee1355a5c34592e401dd41e45d25a493/diffusion_pytorch_model.safetensors \
    a1d993488569e928462932c8c38a0760b874d166399b14414135bd9c42df5815
  fetch lipsync whisper-tiny.safetensors \
    https://huggingface.co/openai/whisper-tiny/resolve/169d4a4341b33bc18d8881c4b69c2e104e1cc0af/model.safetensors \
    7ebd0e69e78190ffe1438491fa05cc1f5c1aa3a4c4db3bc1723adbb551ea2395
  echo "Lip sync model: MONTAGE_LIPSYNC_MODEL=$DIR/lipsync"
fi
# lensfun's database at a pinned commit (media/LensDatabase.cpp lists the same files).
LENSFUN=https://raw.githubusercontent.com/lensfun/lensfun/43f9e001ab66c4fcdd4f400463f13b72f94b2288/data/db
while read -r name sum; do fetch lensfun-db "$name" "$LENSFUN/$name" "$sum"; done <<'LENSES'
6x6.xml ca13c2756b32d728675372695c072517a0e38af5eff2ecd3e17599dee7fc58b3
actioncams.xml 79a5da0322574b95690464444c9d9ba1da0e1f827da44fd6bff120a0ec10e4b4
compact-canon.xml bae47e4b3a0f8852641038ae2dcca5b28ed7965e65ca795fd676f9de857c4670
compact-casio.xml ab35433879c4574ef8f97cd4e3d114d7ff8db4e7279da9d502fc9e911b51ee27
compact-fujifilm.xml 58af9ebef2410c428ae2c5d01003d2247330d47d918823e63522b4d897a997d1
compact-kodak.xml 377018ccd74f58e966b37a11e5a8689be4268153bebece9f1f2a0dbfa8234845
compact-konica-minolta.xml 832a7473951ab625e10c48af6cf60f89b57280016d45729e2584cf9e41ad3cc7
compact-leica.xml 8e4676679377576dc19e7900ce86fc69b0248ea1d4538aa22ee1ee09431a3be3
compact-nikon.xml 5393bdb8223e13d21b85818e96571e97c035470b49b0452dfcedc2e1ff512d3b
compact-olympus.xml 7637d469ad54a02ebce9492a9aa9424a2ff1ec07b0398158076f762fdc08389b
compact-panasonic.xml f191fc2a5d49fb50c06f4c399ee8c1a83a05f93a0efaecd352ec5c1479b8e60b
compact-pentax.xml 596fae3edaadcf2d90ae930fc1bec42597ca0b2bd11842e686e1b915f3736390
compact-ricoh.xml 46df96ebc367d5170cb6b7dfc3742093f0ad946264518fc5488573ba052f29d7
compact-samsung.xml 36e3a778046d56385c387d48474cd32a89a5da94d99e640b5ce716ce747f898d
compact-sigma.xml 0ddc265e3e580da05dcd4664d1b9c2464838d74dd02519c826520950c8f1759e
compact-sony.xml be146388b4f6f7bf0d73394e20386ce6ccef3559ab39f1268ba3c14aaef03207
contax.xml 5eba12e6fd4913470afb6fe25fee47a263084b6cec830297273909872f47669a
generic.xml cd572d4110981546b2c4cd296c2ff16755b30063fa136d8b06ad2ca13ab4c59f
mil-canon.xml 2de8e9467a6af6f83cc9da834d3d3b94d1aaf54519f60c68ac6bc02780a77516
mil-fujifilm.xml 64586336153b948fe2325f6c46679149a4e3c114a70bee4bd1de88721cb4de72
mil-hasselblad.xml 13e1fa55d36c2ed7d22964c21ce6c21d887bfe4506e840e425da1ce4157c3756
mil-leica.xml c691037e2e0743388473c2a0545e340462c412b7e3b57e451f4f9c89dde2388a
mil-nikon.xml dcae5ec2e505411a7f00b6b8dd03825e55620403b558b156be15e9ddfbeaf159
mil-olympus.xml 989a3d88ef07a286efc2755df214792fdf71d60366932a1b369a77ad39cf2abb
mil-panasonic.xml eb5f01f33e65721f3900786d5970c7e220dd042aaa56968be33c009a358ff3c5
mil-pentax.xml 675a94f52794868ad9f1d79e17b7128783c34b3db23e9eac346c5963812a16e5
mil-samsung.xml 10130e9e125e0882a16fd3033cfed0175e9c14cb7d35e630cdb94736f27e5dc3
mil-samyang.xml ae3bf262f12facdc6f81b837cf6d36fdde2a930d2bfb0918d5abaa47fcfea3ab
mil-sigma.xml 5f39b232efba5c208e5a98d8afe92cb2eee879213322d47827fcc6f0dc3a06f9
mil-sony.xml f9f7e5d7461ac58f76eb55dcc5b211f2e9a451402ddb4a9ecb12d1e4dc04f2b8
mil-tamron.xml 2f0c9c4dfb9b0419ba0fac2e618bc251c2e71e068b6e09a23d7994b0ff62db0a
mil-tokina.xml addb27bd82b0d729644b9da1342f92401f63cc5945309ed17f38f4b7af1649e3
mil-zeiss.xml ef8a238904fc9ea6fdb6171863dfd70cd94814af19ecd8c31ee000ca1864589d
misc.xml 2000a347d9dc37d908ea5b56c760de5b2b4ab475857959ae2e901c31339b01bb
om-system.xml a53a627e6ba79fbabb0fa7552ce2b6e56e206425d2eaf0e8ccdb6b8c40c20755
rf-leica.xml f93636e1980ad308d1873cc3ac078cdc77c0d529d777cd45c5c8d0e4781981e3
slr-canon.xml 5244e6df8f2522f829c57e2073313ce31a624e9ed9605310fe43613b9d588c99
slr-hasselblad.xml 97602c467699904bf101179ea2fa24f05f3e1879250498a6ba306cd95a3b7a6b
slr-konica-minolta.xml 5639ca94f121f609247c2c120b65830b3ef7784db6ecfa7e8eb3d29ff5046c1b
slr-leica.xml c5e74cb511712e99ed60a066e85c842fea58f693c5039c3819485619f5293d0e
slr-nikon.xml 5e591e8b1032203cce4c822f1da3d49524a78e7d71c594ea5a4533f2e57aacdf
slr-olympus.xml f7693b6779edd8ed10d7dc23113669255bfa4987cea7db0e843f7f5f2bbbe615
slr-panasonic.xml cd79acb49d9ce5f2a4cecc572b3e117f03da380bed2dcbfd52327192d8ce2381
slr-pentax.xml 2fba773e41a1cfb70f0c85efdab6d78944bf80ddf89bc2035ef6db39da352f29
slr-ricoh.xml c835798136a53535fdf4f81312ac9aad0ea3de3ef732c183d8cc4d2f432011d9
slr-samsung.xml dc0a29e424ff96e4316ddeb76126fa6f6c73a8836905a3e1f56f0c493826148f
slr-samyang.xml fb1585bea91f5f59e1fae8811dd76c92252ee5d52e3647df2aff4695048443b5
slr-schneider.xml de607cfe4ba6e7debc61e282ce39b72825c07601572e14af2bc9108ba67faf81
slr-sigma.xml f55f41c144fbb72e3c51d2dfef40e918930d37db1b4150bd02bd746406c036d2
slr-soligor.xml 1aba0aab19bce87ea86175b6551cb5306a512eb54be1698b89991b86008f6a04
slr-sony.xml e5ca7f532afe23ff380bf2f9e5458f687bed603355d73a254d85c4e692942fdc
slr-tamron.xml c62090789cebe6bbb663111c6e97beeb1111852bb735088e29cf8782df4bab45
slr-tokina.xml b1213eda81a613bc3976340398c6b8ce8b7dc29423476fc565d5f8e5cf612ecf
slr-ussr.xml 1fd6acc8a8f40373379bea2899ffd96fc34b22d9b044d0474dabd4c89af44efb
slr-vivitar.xml c5c67e3fe38e8fa19044acadc7730047690435f66aac563f55ffe04997a88111
slr-zeiss.xml 49aa211a41fd5fe391d26af5fa65bdf9428d60863ffff15d3916fa1c520dc101
LENSES
# SADIE II's KU100 dummy head (University of York, zenodo.org/records/12092466; media/Sofa.cpp names the same file).
if [ "$HRTF" = 1 ]; then
  fetch sadie-ku100 D1_HRIR_SOFA.zip "https://zenodo.org/records/12092466/files/D1_HRIR_SOFA.zip?download=1" 366321fa78f211bacc0ec6bea96701625b196a5db54ec16748b0eab0b9705f75
  echo "KU100 HRIRs: MONTAGE_HRTF_DIR=$DIR/sadie-ku100"
fi
echo "Models in $DIR: MONTAGE_OBJECT_MODEL=$DIR/edgetam-video MONTAGE_SPEAKER_MODEL=$DIR/speakers MONTAGE_VISUAL_MODEL=$DIR/clip-vit-b32 MONTAGE_SPEECH_MODEL=$DIR/speech-enhance MONTAGE_UPSCALE_MODEL=$DIR/upscale MONTAGE_DEBLUR_MODEL=$DIR/deblur MONTAGE_FACE_MODEL=$DIR/faces MONTAGE_DEPTH_MODEL=$DIR/depth MONTAGE_VIDEO_DEPTH_MODEL=$DIR/video-depth MONTAGE_RIFE_MODEL=$DIR/rife MONTAGE_MATTE_MODEL=$DIR/matte MONTAGE_TTS_MODEL=$DIR/tts MONTAGE_INPAINT_MODEL=$DIR/inpaint MONTAGE_SENTENCE_MODEL=$DIR/minilm-qa MONTAGE_OCR_MODEL=$DIR/ocr MONTAGE_SOUND_MODEL=$DIR/sound MONTAGE_STEM_MODELS=$DIR/stems MONTAGE_LENS_DB=$DIR/lensfun-db"
