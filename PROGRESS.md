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
- [ ] Unit tests for the engine (ctest)
- [ ] Scene-cut detection

## Application (Qt 6 Widgets)
- [ ] Main window with dockable panels, dark theme
- [ ] Media bin with import (dialog + drag & drop), thumbnails
- [ ] Source monitor with In/Out and insert / overwrite
- [ ] Program monitor with playback (audio-clocked), J/K/L, scrubbing, playback resolution
- [ ] Timeline: tracks, clips with thumbnails/waveforms, tools (select, razor, ripple, roll, slip, slide), snapping, zoom, markers, transitions
- [ ] Inspector with keyframes for every effect parameter
- [ ] Effects & transitions browser
- [ ] Scopes: waveform, RGB parade, vectorscope, histogram
- [ ] Audio meters and track mixer
- [ ] Export dialog with presets and progress
- [ ] Save / open / autosave, sequence settings
- [ ] Keyboard shortcuts matching industry conventions

## Delivery
- [ ] CI (GitHub Actions): build + tests
- [ ] README with build and usage docs
