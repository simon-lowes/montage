# Phase 3 roadmap (October 2026)

A second gap analysis, made after Phase 2 and the features listed under "Beyond the roadmap" in `PROGRESS.md`. It compares Montage with Premiere Pro 25.2 to 26.5, DaVinci Resolve 20 and 21, Final Cut Pro 12.0 to 12.4, Media Composer 2026.8, CapCut desktop and Descript.

Montage already has most of the 2024–2026 "utility AI" features:
- object masks;
- transcripts with speaker labels;
- multicam auto-switching;
- semantic shot search;
- auto reframe;
- colour matching.

The GPU compositor with an HDR viewer (Phase 2 #15) is still deferred, and the render cache is now done. Neither is ranked below.

Ranking is value to a professional or prosumer editor × feasibility in C++/Qt/FFmpeg with optional local ONNX models. Sizes: S ≤ 2, M 2–6, L 6–12 person-weeks.

| # | Feature | Who has it | Size | Notes |
|---|---------|-----------|------|-------|
| 1 | Animated word-by-word captions (one word at a time, the spoken word highlighted, a pop on each word) | Resolve 20, Premiere 26.3, FCP 12.3, CapCut, Descript | S | Word timings and title animation already exist |
| 2 | Voiceover recording onto the timeline (countdown, punch-in between In and Out) | all six | S | QAudioSource |
| 3 | Burn-ins on export: timecode, clip name, metadata, watermark or logo | Premiere, Resolve, Avid, FCP | S | Needed for review copies |
| 4 | Grading essentials: Hue/Luma/Sat curves, lift/gamma/gain wheels, split-screen compare against a still | Premiere Lumetri, Resolve, FCP | S–M | Plain algorithms |
| 5 | Build a cut from a script: align a script with transcripts, best take of each line, alternates on extra tracks | Resolve IntelliScript, Premiere Paper Edit, Avid ScriptSync AI | M | Smith–Waterman over words |
| 6 | Speech enhancement and stem separation | Premiere Enhance Speech, Descript Studio Sound, Resolve Voice Isolation and Music Remixer | M | DeepFilterNet3 (MIT/Apache-2.0); Spleeter (MIT) or Demucs (check the weights' licence) |
| 7 | Beat markers and fitting music to length | FCP 12 Beat Detection, Resolve AI Music Editor, Premiere Remix, CapCut | M | Beat This! (MIT; confirm); self-similarity jump points |
| 8 | One-click mix: classify dialogue, music and effects, ride dialogue levels, match tone and room | Resolve AI Audio Assistant and Dialogue Matcher, Premiere | M | YAMNet (Apache-2.0) or PANNs (MIT) plus DSP |
| 9 | Caption and transcript translation on the device | Premiere (27 languages), CapCut, Descript | M | CTranslate2 (MIT) with Opus-MT (CC-BY-4.0) or MADLAD-400 (Apache-2.0); not NLLB (non-commercial) |
| 10 | Bézier keyframes and a graph editor | Premiere, Resolve 21, FCP, Avid | M | |
| 11 | Surround and multichannel deliverables: 5.1/7.1 buses and panning, split-track masters, stems | Fairlight, Premiere, Avid, FCP | M–L | FFmpeg channel layouts |
| 12 | Temporal video noise reduction | Resolve UltraNR, FCP | M | Motion-compensated NLM using the existing optical flow |
| 13 | Shape layers and Lottie motion graphics | Resolve 21, Premiere MOGRT, FCP Motion, CapCut | M | ThorVG (MIT) |
| 14 | AI upscaling | Resolve SuperScale, CapCut | M | Real-ESRGAN general-x4v3 (BSD-3) |
| 15 | AAF export to Pro Tools and Fairlight | Premiere, Resolve, Avid | L | pyaaf2-style writer (MIT), or the AAF SDK if its licence fits |

Near misses:
- **Quick wins (S):**
  - censor or bleep words from the transcript;
  - checkerboard dialogue by speaker onto separate tracks;
  - "Find Similar" shots from the CLIP index.
- **People search and face retouch:** YuNet (MIT) with SFace (Apache-2.0).
- **Depth map and refocus:** Depth Anything V2 Small (Apache-2.0; the larger models are non-commercial).
- **Better slow-motion frames:** RIFE (MIT) in place of the current optical flow.
- **Object removal:** the good video models are non-commercial, and LaMa (Apache-2.0) flickers frame by frame.

Left out because they need vendor servers or very large models: Generative Extend, text-to-video, AI sound effects, voice cloning and dubbing, eye contact, and LLM highlight reels. External agents can already script highlight reels through the MCP server.

Sources: Newsshooter (Resolve 21, Premiere 25.6, FCP 12.0), No Film School (Resolve 20), Adobe's Premiere release notes, Larry Jordan (FCP 12.3), Videomaker (Media Composer 2026.8), and a 2026 review of CapCut's AI features.
