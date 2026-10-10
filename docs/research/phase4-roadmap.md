# Phase 4 roadmap (October 2026)

A third gap analysis, made after Phase 3 and the features since (HDR10+, control surfaces, stereo 3D, ambisonics, AAF, DCP and IMF among them). It compares Montage with Premiere 25.6 to 26.5, DaVinci Resolve 21 and 21.1, Final Cut Pro 11 and 12, and Media Composer 2025.12. Every candidate was checked against `README.md` and the `PROGRESS.md` checklists, so nothing below is already there.

Ranking is value to a professional or prosumer editor × feasibility in C++/Qt/FFmpeg with optional local ONNX models, building only on permissive licences (MIT, BSD, Apache, LGPL). Sizes: S ≤ 2, M 2–6, L 6–12 person-weeks.

| # | Feature | Who has it | Size | Notes |
|---|---------|-----------|------|-------|
| 1 | ProRes RAW with RAW controls (exposure, white balance, tint), plus current camera logs: Apple Log 2 (Apple Gamut), DJI D-Log2 (D-Gamut2), GoPro Log, Leica L-Log | Premiere 26.5, Final Cut (iPhone 17 Pro), Resolve 21.1 | M (logs alone S) | FFmpeg 8.0's ProRes RAW decoder (LGPL); BRAW, R3D and ARRIRAW need proprietary SDKs, so optional at most |
| 2 | HDR viewer on HDR and EDR displays (PQ and HLG at real brightness in the monitors and clean feed) | Premiere (EDR, DirectX HDR), Resolve, Final Cut | M | Qt 6.6+ QRhi HDR10 and scRGB swap chains; only the viewer needs QRhi |
| 3 | Stacked timelines and editing from a sequence (two sequences open, three-point edits and drags from a selects reel; insert a sequence as its clips rather than a nest) | Resolve, Premiere, Avid | S–M | Existing timeline code |
| 4 | Aux sends and full mixer automation (pre/post-fader sends to effect returns; automation of insert parameters, sends, mute and EQ) | Fairlight, Premiere Audio Track Mixer, Pro Tools | M | In-house; the Mackie surface support follows |
| 5 | Voice-cloned speech (regenerate misspoken words, dub into languages other than English) | Resolve 21 Speech Generator, Descript | M | Chatterbox Multilingual (MIT, watermarked) with Opus-MT; consent required |
| 6 | Stem separation and prompted sound isolation (vocals/drums/bass/other, dialogue from a mix, "a dog barking", an object clicked in the picture) | Resolve Music Remixer and Dialogue Separator, CapCut | L | SAM Audio (check the SAM License); Demucs and RoFormer weights unclear |
| 7 | Editable motion-graphics templates (designer-exposed text, colour and image fields in the Inspector; CSV batch fill) | Premiere MOGRTs, Final Cut published parameters, Resolve 21 macros and OGraf | M (L with OGraf) | Lottie slots through ThorVG (check its slot API) |
| 8 | Audio search by description ("glass breaking", "applause") across the project and SFX libraries | Premiere 25.6 | S–M | LAION-CLAP (confirm the checkpoint licence) or YAMNet (Apache-2.0) tags |
| 9 | ALE and ASC CDL interchange (.ale with scene, take, reel and CDL columns; .cdl/.cc/.ccc; CDLs in EDLs) | Resolve, Media Composer, LiveGrade | S | Plain text; OCIO has a CDLTransform |
| 10 | Editing growing files (MXF, fragmented MOV, TS still recording) | Avid, Premiere, Resolve 21.1 | M | FFmpeg re-probing with the watch-folder code |
| 11 | In-app scripting and extension panels | Premiere UXP, Resolve (Studio from 21.1), Media Composer Extensions | M–L | pybind11, sol2 or QJSEngine over the MCP tool schemas |
| 12 | Motion Deblur | Resolve 21 | M | NAFNet (MIT per mirrors; confirm) on ONNX Runtime |
| 13 | Control surfaces beyond Mackie: Tangent colour panels, HUI, editor keyboards with jog and shuttle | Resolve, Premiere, Avid | M | Tangent Hub's TIPC protocol; hidapi (BSD) |
| 14 | Freeform storyboard bin (arrange thumbnails freely, assemble in that order) | Premiere Freeform view, Avid Frame and Script views | M | QGraphicsView over the bin model |
| 15 | Spatial video (MV-HEVC) in and out | Final Cut 11, Resolve 21 | M | FFmpeg 7.1+ decoding; x265 4.0+ multiview encoding |

Near misses: drawn annotations and version comparison on the review page; remote render nodes driven by `montage-cli`; face reshaping and ageing; direct upload to YouTube and TikTok (cloud APIs); generative video and sound effects (cloud only).

## Present but weaker than the leaders

1. **Performance.** The compositor runs on the CPU and ONNX Runtime without CUDA, CoreML or DirectML, so AI effects take 1–4 s a frame. Resolve is GPU-first.
2. **Proxies.** Fixed 960 px x264 proxies without sound: no codec or size presets, no 10-bit or HDR proxies, no camera proxies attached. Final Cut 12.3 makes HDR HEVC proxies; Avid links dual-resolution media.
3. **Grading structure.** Effects stack in a line, with group pre- and post-clip grades. Resolve has a node graph with parallel and layer mixers and key routing.
4. **Collaboration.** Montage locks whole projects; Resolve's Project Server locks per bin and per timeline, and Avid locks bins.
5. **Object removal and masks.** LaMa fills each frame on its own, so fills shimmer, and a mask holds one object. The temporally consistent inpainters (ProPainter, DiffuEraser) are non-commercial.

Licence points that come only from mirrors or secondary pages and need checking before anything ships: LatentSync, NAFNet, the CLAP checkpoints and ThorVG's slot API.

## Sources

- Premiere 26.5: https://community.adobe.com/announcements-727/what-s-new-in-adobe-premiere-26-5-september-2026-1641187
- Final Cut Camera 2.0, ProRes RAW and Apple Log 2 on iPhone 17 Pro: https://www.newsshooter.com/2025/09/09/apple-announces-final-cut-camera-2-0-with-prores-raw-and-genlock-on-iphone-17-pro-and-iphone-17-pro-max/
- Resolve 21.1: https://www.redsharknews.com/davinci-resolve-21.1-new-features-release
- FFmpeg 8.0 ProRes RAW decoder: https://ubuntuhandbook.org/index.php/2025/08/ffmpeg-8-0-released-apv-prores-raw-decoder/
- Qt QRhiSwapChain HDR formats: https://doc.qt.io/qt-6/qrhiswapchain.html
- Premiere EDR monitoring: https://community.adobe.com/t5/premiere-pro-discussions/14-0-1-new-feature-extended-dynamic-range-monitoring-quot/m-p/10938430
- Resolve stacked timelines: https://larryjordan.com/articles/hidden-features-in-davinci-resolve-20-that-make-editing-easier/
- Fairlight: https://www.blackmagicdesign.com/products/davinciresolve/fairlight
- Resolve 21: https://www.blackmagicdesign.com/products/davinciresolve/whatsnew
- Descript dubbing: https://help.descript.com/hc/en-us/articles/37194900295821-Dub-speech-to-add-translated-voiceover
- Chatterbox: https://huggingface.co/ResembleAI/chatterbox
- SAM Audio: https://github.com/facebookresearch/sam-audio
- Lottie slots: https://docs.lottiefiles.com/en/runtimes/distributions/js/v0.x/core-concepts/slots
- Premiere 25.6 Audio Search: https://www.newsshooter.com/2025/11/21/adobe-premiere-pro-25-6/
- LAION-CLAP: https://github.com/LAION-AI/CLAP
- ASC CDL with LiveGrade and Resolve: https://kb.pomfort.com/livegrade/hands-on/export-looks-stills-metadata/asc-cdl-workflow-livegrade-and-resolve/
- Premiere UXP: https://blog.developer.adobe.com/en/publish/2025/12/uxp-arrives-in-premiere-a-new-era-for-plugin-development
- Resolve 21.1 scripting: https://www.cined.com/davinci-resolve-21-1-released-ai-assistant-integration-via-mcp-individual-hdr-trims-and-python-scripting-moves-to-studio/
- NAFNet: https://github.com/megvii-research/NAFNet
- Tangent via Hammerspoon: https://hammerspoon.org/docs/hs.tangent.html
- Premiere Freeform view: https://nofilmschool.com/freeform-view-storyboard-video-editing-premiere-pro
- Final Cut spatial video: https://uploadvr.com/apple-final-cut-pro-for-mac-spatial-video-editing
- FFmpeg MV-HEVC: https://lwn.net/Articles/992496
- x265 4.2: https://x265.readthedocs.io/en/4.2/releasenotes.html
- Final Cut proxies: https://support.apple.com/102825
- Resolve collaboration: https://www.blackmagicdesign.com/products/davinciresolve/collaboration
