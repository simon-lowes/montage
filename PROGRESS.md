# Montage MVP progress

Living checklist for the MVP. Each item is checked only when it is built *and* covered by a test or a
verified run.

## Engine (C++20, no GUI)
- [x] Project model: media, sequences, video/audio tracks, clips, transitions, markers, keyframed params
- [x] Edit operations: insert, overwrite, razor / add edit, lift, ripple delete, extract, move, trim (normal / ripple), roll, slip, slide, speed / reverse, close gap, link / unlink, copy / paste, compound clips, snapping
- [x] Snapshot undo / redo
- [x] Project file (.montage JSON) with relative-path relinking
- [x] SMPTE timecode incl. drop-frame
- [x] FFmpeg probe + frame-accurate video decode (rotation, SAR, colourspace aware), audio decode, frame cache
- [x] Compositor: fit/fill transform, crop, rotation, opacity, 13 blend modes, nested sequences
- [x] Video effects: primaries colour correction, curves, HSL, .cube LUTs, B&W, invert, chroma key, luma key, blur, sharpen, vignette, mosaic, mirror, drop shadow
- [x] Generators: titles, colour matte, gradient, bars
- [x] Transitions: dissolve, dip to black/white, wipe, push, slide, iris, zoom; audio crossfades
- [x] Audio mixer: clip gain/pan keyframes, track volume/pan/mute/solo, EQ, compressor, limiter, filters, delay, meters
- [x] Export: H.264, H.265, ProRes (incl. 4444 alpha), DNxHR, VP9, AV1, WAV, AAC; presets; stills
- [x] `montage-cli`: probe, new, info, render, frame, presets
- [x] Unit tests for the engine (ctest)
- [x] Scene-cut detection
- [x] Proxy generation and proxy playback
- [x] Match sequence settings to the first clip
- [x] Loudness measurement and normalisation (BS.1770 / R128)
- [x] EDL (CMX 3600) and OpenTimelineIO export
- [x] Auto colour; synchronise clips by audio

## Application (Qt 6 Widgets)
- [x] Main window with dockable panels, dark theme
- [x] Media bin with import (dialog + drag & drop), thumbnails, proxies
- [x] Source monitor with In/Out and insert / overwrite (buttons, keys, drag)
- [x] Program monitor with playback, J/K/L, scrubbing, playback resolution, loop, safe guides
- [x] Timeline: tracks, clips with thumbnails/waveforms, tools (select, razor, ripple, roll, slip, slide, hand), snapping, zoom, markers, transitions, drag & drop
- [x] Inspector with keyframes for every effect parameter
- [x] Effects & transitions browser
- [x] Scopes: waveform, RGB parade, vectorscope, histogram
- [x] Audio meters and track mixer
- [x] Export dialog with presets, range, progress and cancel
- [x] Save / open / recent / autosave, sequence settings, new sequences
- [x] Keyboard shortcuts matching industry conventions (F1 lists them)
- [x] Offscreen GUI integration tests (test_app)

## Polish before calling the MVP done
- [x] Playback: render ahead and cache frames so playback and stepping are smooth
- [x] Audio scrubbing while dragging the playhead
- [x] Keyframe markers on timeline clips
- [x] Correctness review of edit operations, playback and export; fix findings (12 found, all fixed, regression tests added)

## Delivery
- [x] CI (GitHub Actions): build + tests
- [x] README with build and usage docs
- [x] Builds against FFmpeg 6.1 through 9 (tested with 9.0.2, which Homebrew ships)
- [x] macOS DMG and Windows installer + zip from the Package workflow (built, and each packaged app launched, on CI runners)

# Phase 2: rival the market leaders

Ranked by the research in `docs/research/phase2-roadmap.md` (impact versus effort; S ≤ 2, M 2–6, L 6–12, XL > 12 person-weeks). Each item lands with tests and keeps CI and the Package workflow green.

- [x] 1. Crash-safe project history and recovery (S): live recovery copy after each edit, crash detection via session locks, recovery dialog that keeps the project path, plugin safe mode, rolling timestamped snapshots.
- [x] 2. Hardware decode and encode (M): per-platform hwaccel decode with software fallback per stream and a decoder cap, Playback toggle; hardware H.264/H.265 presets that pick the first working encoder and fall back to x264/x265.
- [x] 3. Audio plugin manager UI (S): Tools > Audio Plugins with on/off switches, status, blocklist with reasons, Rescan, Rescan Selected, Retry Blocked, extra search folders and a scan log; background scan at startup; probes run in parallel.
- [x] 4. Transcription engine (M): whisper.cpp 1.9.5 built in, word timings, eight models from Tiny to Large v3 Turbo downloaded on first use, auto language detection and translation; Transcribe… in the media bin, transcripts saved in the project (undoable), media search by spoken words, SRT/VTT/TXT/JSON export; `montage-cli transcribe` and `models`. Speaker labels are still to do (the transcript format already has a speaker field).
- [x] 5. Caption tracks (M): caption lanes on the timeline (move, retime, double-click to edit), generation from transcripts that follows trims, speed and mutes, a Captions panel (edit, add, split, merge, delete, style, rename, language), SubRip/WebVTT import, SubRip/WebVTT/SCC (CEA-608 pop-on, checked with FFmpeg's 608 decoder) export, viewer CC toggle, burn-in and embedded subtitle streams (mov_text, SubRip, WebVTT) on export; `montage-cli captions` and `render --burn-captions --embed-captions`. Not done yet: roll-up and paint-on 608, CEA-708, and captions following ripple edits.
- [x] 6. VST3 hosting (M): MIT VST 3.8 SDK, crash-isolated probing, parameters, automation, two-blob state. Also done ahead of the list: crash-isolated scanning for CLAP/VST3/LV2/AU and CLAP hosting.
- [x] 7. Transcript panel (M):
  - Sequence mode: the cut's words with the current one highlighted; click to seek; search, including a partly typed last word; delete words as a ripple delete across all tracks and caption tracks; remove filler words; shorten pauses.
  - Source mode: select words to set In and Out, then insert or overwrite them into the timeline.
  - Uncertain words are dotted and filler words dimmed.
- [x] 8. Dialogue denoise and voice isolation (M):
  - **Noise Reduction**: spectral, with a noise print from the quietest tenth of the recording, gain smoothing over frequency and time, and adjustable reduction and sensitivity.
  - **Voice Isolation**: RNNoise (Xiph, BSD), with its model fetched and pinned at configure time and its 20 ms delay compensated.
  - Both process the whole source audio in the background with a cache, so there is no latency and seeking is free. Playback uses the original audio until the result is ready; exports wait for it.
  - Tested on real speech plus noise: SNR 12.8 → 19.1 dB, pauses 14 dB quieter (Noise Reduction) and 84 dB quieter (Voice Isolation), with speech level kept.
- [x] 9. Shape masks and HSL qualifier (M):
  - Every video filter can have an ellipse or rectangle mask (feather, expansion, rotation, opacity, invert; keyframeable) and/or an HSL qualifier (hue, saturation, luma, softness), with a Show Mask view.
  - Masks are in the clip's own frame, so they follow its transform.
  - On-screen handles in the Program monitor move and resize them, one undo step per drag.
  - Not done yet: bezier masks, and tracking (#16).
- [ ] 10. Plugin editor windows (L)
- [ ] 11. Track and bus effect chains, delay compensation, offline plugin rendering (L)
- [ ] 12. Audio Unit hosting on macOS (M)
- [ ] 13. OCIO colour management, ACES 2.0, HDR export (L)
- [ ] 14. Multicam (L)
- [ ] 15. GPU compositor on QRhi (XL)
- [ ] 16. Tracking and stabilisation (L)
- [ ] 17. LV2 hosting (M)
- [ ] 18. Speed ramps and optical-flow retiming (L)
- [ ] 19. Interchange round trip: OTIO import, EDL import, FCP7 XML and FCPXML (L)
- [ ] 20. AI object masks with SAM 2 (XL)
