# Phase 5 roadmap (October 2026)

A fourth gap analysis, made as Phase 4 drew to a close. It compares Montage with Premiere 25.6 to 26.5, DaVinci Resolve 20, 21 and 21.1, Final Cut Pro 11 to 12.4 and Media Composer 2025.12 and 2026.8. Every candidate was checked against `README.md` and the `PROGRESS.md` checklists (iXML, 608 captions, dynamic JKL trimming, audio-reactive motion, Lens Flare, EXR/DPX pulls, RIFE, ACES 2.0, multicam Auto Switch and Relight are all already there; Dolby Vision and per-bin locking are declined there).

Ranking is value to a professional or prosumer editor × feasibility in C++/Qt/FFmpeg with optional local ONNX models, building only on permissive licences. Sizes: S ≤ 2, M 2–6, L 6–12 person-weeks.

## Phase 4 leftovers, researched

- **#12 Motion Deblur.** NAFNet-GoPro-width32 (17M parameters, about 16 GMACs at 256², about 0.5 TMAC a 1080p frame, so seconds a frame on a CPU, run in overlapping tiles). Code MIT (megvii-research/NAFNet, with BasicSR's Apache-2.0); no separate licence on the weights; trained on GoPro and REDS, both CC BY 4.0 (credit them). The official weights are only on Google Drive and Baidu, so export fp32 ONNX with dynamic size from the official `.pth` (replacing the custom LayerNorm2d autograd Function with plain ops, and checking whether the published scores use the NAFNetLocal variant) and host it in a project-owned Hugging Face repo pinned by revision and SHA-256, as the Spleeter graph is. Unofficial exports exist (ailia-models, `bihanikeshav/selects-onnx`, `litert-community/NAFNet-GoPro-width32-LiteRT`) but none with a clear licence. Restormer (MIT, 26M, about nine times the compute; weights on a GitHub release) is the fallback; MPRNet is non-commercial; DeblurGAN-v2 (BSD) is far weaker. GoPro blur is synthetic, so expect good results on camera shake and weaker ones on real-world blur.
- **#13b Surfaces beyond Mackie and HUI**, in order of value per effort:
  1. An **OSC control endpoint** reusing the tool schemas (S): Bitfocus Companion (MIT) drives Stream Deck, Loupedeck Live/CT, Razer and X-keys and sends OSC through its generic OSC connection, so one endpoint covers them all.
  2. **Blackmagic Speed Editor and Editor Keyboard** over HID (S–M): Sylvain Munaut's reverse engineering (Apache-2.0; VID 0x1edb, PID 0xda0e; a challenge/response on feature report 6 that must be renewed within about ten minutes; jog modes relative, absolute and absolute with a dead zero on report 3; LEDs on reports 2 and 4; keys on report 4, jog on report 3, battery on report 7) and CommandPost's MIT hs.blackmagic. hidapi (BSD) as the dependency. Resolve holds the device while it runs.
  3. **Tangent** panels (M): TUBE 3.2 / TIPC rev 4 over TCP to Tangent Hub on 127.0.0.1:64246 (big-endian length-prefixed commands; the app sends ApplicationDefinition 0x81 and ParameterValue 0x82 and receives ParameterChange 0x02, MenuChange 0x05...), with controls and default maps in XML. Documented in Tangent's free Developer Support Pack (terms not confirmed) and in Hammerspoon's and CommandPost's open-source clients. Needs Tangent Hub (Mac and Windows).
- **#15 MV-HEVC.** Decoding is in FFmpeg 7.1 and later (two views at most: the decoder's `view_ids` option, -1 for both; `view_ids_available`/`view_pos_available`; each frame tagged with `AV_FRAME_DATA_VIEW_ID`, and with `AV_FRAME_DATA_STEREO3D` left/right when the SEI says). Ubuntu 24.04's FFmpeg 6.1 has none, while Homebrew (9.0.2), gyan.dev (9.0.2) and BtbN (8.1/9.0/master) do, so decoding is a build-time feature (`LIBAVCODEC_VERSION`) tested where present. Encoding: FFmpeg's libx265 wrapper has no MV-HEVC even on master (and x265 is GPL); `hevc_nvenc` encodes it from FFmpeg 8.0 on GPUs reporting the capability; VideoToolbox through FFmpeg does not, Apple's own API does. `movenc` already writes the spatial boxes (`vexu`, `eyes`, `hero`, `cams`, `blin`, `hfov`). No permissively licensed sample exists: make test files with x265 or NVENC.

## Phase 5 candidates

| # | Feature | Who has it | Size | Notes |
|---|---------|-----------|------|-------|
| 1 | Proxy overhaul: ProRes Proxy, DNxHR LB and 10-bit HEVC (HDR) proxies at chosen sizes, proxies with sound, camera proxies attached, proxies for AI analysis | Final Cut 12.3, Resolve 21.1, Avid | S–M | FFmpeg encoders already used for mastering |
| 2 | Drawn review annotations (frame drawings with replies and status, version comparison) on the review page and back as markers | Resolve 21.1, Premiere (Frame.io v4) | S–M | In-house; extends the review package |
| 3 | Colour-chart calibration (ColorChecker / DSC charts found and matched) | Resolve | S–M | OpenCV mcc (Apache-2.0) or an in-house detector |
| 4 | Multi-object masks with edge refinement | Premiere 26.5, Final Cut 12.3, Resolve | S–M | EdgeTAM (Apache-2.0); BiRefNet-lite (MIT) |
| 5 | GPU and NPU inference (CoreML, WinML/DirectML, CUDA, OpenVINO execution providers) | All four | M | ONNX Runtime (MIT); DirectML is in sustained engineering, WinML is the path forward |
| 6 | Voice conversion (speech to speech: ADR in the actor's voice) | Resolve 20 Voice Convert | S–M | Chatterbox's VC path (MIT, watermarked), consent as for cloning |
| 7 | C2PA Content Credentials (sign exports with AI-use assertions; keep camera signatures on import) | Premiere 26.2 | S–M | c2pa-rs (MIT/Apache, C FFI); the user's signing certificate |
| 8 | Lens-profile correction (distortion, vignetting, chromatic aberration; action-camera presets; DNG opcodes) | Premiere, Resolve 21.1 | M | lensfun (LGPL-3.0, database CC BY-SA 3.0) |
| 9 | SDI/HDMI/NDI reference output (10-bit, HDR metadata, embedded audio, A/V offset), capture later | All four | M | AJA libajantv2 (MIT); DeckLink SDK fetched at build time (its EULA forbids redistribution); NDI SDK proprietary but royalty-free |
| 10 | Node-based grade graph (parallel and layer mixers, key inputs, shared nodes) | Resolve; Premiere 26.2 colour layers | L | In-house; the GPU compositor (Phase 2 #15) first |
| 11 | Python scripting next to Lua | Resolve (Studio from 21.1), Avid | S–M | pybind11 (BSD); a Python to ship or find |
| 12 | Remote render nodes driven by `montage-cli` | Resolve | M | In-house |
| 13 | Direct publishing to YouTube, Vimeo, TikTok and X | Resolve 21 | M | OAuth app registration, platform audits and quotas |
| 14 | 3D camera tracking to place 3D titles | Resolve Fusion, After Effects | L | libmv (verify licence), OpenCV sfm, COLMAP (BSD) |
| 15 | OGraf HTML graphics | Resolve 21 | L | QtWebEngine (LGPL) or CEF (BSD) |

Lower priority: a stills page (Resolve 21), sky replacement, Cinematic-mode refocus (Final Cut 12.4, Apple's framework only), SOFA HRTF binaural rendering (libmysofa, BSD), face reshaping and ageing, temporally consistent inpainting (the good models are non-commercial).

## Sources

Resolve 21 (blackmagicdesign.com/products/davinciresolve/whatsnew), Resolve 21.1 (redsharknews.com, 8 September 2026), Resolve 20 New Features Guide (Voice Convert), Premiere 26.0 (redsharknews.com), 26.2 and 26.5 (community.adobe.com announcements), 26.3 (larryjordan.com), 25.6 (newsshooter.com, 21 November 2025), Final Cut Pro release notes (support.apple.com/102825), Media Composer 2025.12 (avid.com) and 2026.8 (newsshooter.com, 27 August 2026); NAFNet's LICENSE and the GoPro/REDS dataset pages; Hammerspoon's hs.tangent and CommandPost's hs.blackmagic; smunaut/blackmagic-misc; Bitfocus Companion; FFmpeg's Changelog and hevc FATE tests, x265 4.x release notes; packages.ubuntu.com, formulae.brew.sh, gyan.dev and BtbN release pages.
