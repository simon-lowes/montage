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
- [ ] Correctness review of edit operations, playback and export; fix findings

## Delivery
- [x] CI (GitHub Actions): build + tests
- [x] README with build and usage docs
