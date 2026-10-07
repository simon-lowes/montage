# Montage

A professional non-linear video editor written in **C++20** with **Qt 6** and **FFmpeg**.

Montage follows the same technology choices as the editors it is modelled on. Adobe Premiere Pro,
DaVinci Resolve, Avid Media Composer, Lightworks and Vegas Pro are written in C++, and the open-source
professional editors (Kdenlive, Shotcut, Olive) use C++ with Qt for the interface and FFmpeg for media.
Final Cut Pro is the exception: it is written in Objective-C/Swift on Apple-only frameworks.

## Features

**Editing**
- Multi-track timeline: unlimited video and audio tracks, with lock, sync-lock, mute, solo, hide and track targeting
- Tools: Selection (V), Razor (C), Ripple (B), Roll (N), Slip (Y), Slide (U), Hand (H)
- Insert, overwrite, lift, extract, ripple delete, close gap, nudge, duplicate, copy / paste / paste-insert
- Three-point editing from the Source monitor (I / O marks, `,` insert, `.` overwrite, drag from the viewer)
- Synchronise clips from separate cameras and recorders by audio waveform (cross-correlation, sub-frame accurate)
- Linked audio/video, linking and unlinking, snapping, markers, match frame, compound (nested) clips
- Constant speed changes and reverse, with optional ripple
- Snapshot undo/redo for every operation; drags and slider edits merge into one undo step
- Scene-cut detection that cuts a clip at every shot change

**Effects and colour**
- Transform on every clip: position, scale, rotation, anchor, crop, opacity, flip, and fit/fill/stretch
- 13 blend modes
- Primary colour correction: lift, gamma and gain per channel, plus exposure, contrast, pivot, saturation, temperature, tint and offset
- One-click Auto Colour (grey-world balance and level stretch)
- Curves with presets, HSL/vibrance, `.cube` 3D/1D LUTs, black & white, invert
- Chroma key with spill suppression, and luma key
- Gaussian blur, sharpen, vignette, mosaic, mirror, drop shadow
- Keyframes on every parameter: linear, hold or smooth, with previous/next navigation
- Titles (font, outline, shadow, background box, alignment, tracking), colour mattes, gradients, SMPTE bars
- Transitions: cross dissolve, dip to black/white, wipe, push, slide, iris, cross zoom
- Scopes: waveform, RGB parade, vectorscope, histogram

**Audio**
- Clip gain and pan, both keyframeable
- Track faders, pan, mute and solo in a mixer panel
- Peak meters with hold and clip indicators
- Effects: 3-band EQ, compressor, limiter, high/low-pass, delay
- Crossfades: equal power or constant gain
- Loudness normalisation to -14, -16, -23 or -24 LUFS (ITU-R BS.1770 / EBU R128 gated measurement)
- Waveforms on the timeline

**Media and output**
- Imports anything FFmpeg reads
- Frame-accurate decoding that honours rotation, pixel aspect ratio, colour space and range
- Images as stills
- Proxy workflow: 960 px intra-frame proxies, toggled in the Program monitor
- Preview resolution: Full, 1/2, 1/4, 1/8
- Export presets: H.264 (x264), H.265 (x265, 8- and 10-bit), Apple ProRes 422 HQ/LT/4444 (with alpha), Avid DNxHR, VP9, AV1 (SVT-AV1), WAV and AAC; exports either the whole sequence or In–Out
- Still frame export
- Interchange: CMX 3600 EDL and OpenTimelineIO (`.otio`) export, for finishing in Resolve, Premiere, Avid or Nuke
- Project files are readable JSON (`.montage`). They relink moved media through relative paths and autosave every 2 minutes.
- `montage-cli` for headless rendering and automation

## Building

Ubuntu 24.04 or Debian with Qt ≥ 6.4 and FFmpeg ≥ 6:

```bash
./scripts/setup-deps.sh                 # installs compilers, Qt 6, FFmpeg dev packages
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/app/montage                 # the editor
./build/src/cli/montage-cli --help      # the command-line tool
```

CMake options:
- `-DMONTAGE_BUILD_APP=OFF` builds only the engine and CLI, without Qt Widgets or Multimedia.
- `-DMONTAGE_BUILD_TESTS=OFF` skips the tests.

## Using the editor

1. **Import** with `Ctrl+I`, or drag files or folders onto the Media panel or the timeline.
2. **Pick shots.** Double-click a clip to load it in the Source monitor, mark `I` and `O`, then press `,` to insert or `.` to overwrite at the playhead on the targeted tracks. The track buttons in the header set the target.
3. **Cut** with the timeline tools. Drag clip edges to trim, use `B` for ripple trim, `N` to roll an edit point, and `Y`/`U` to slip or slide. `Ctrl+K` adds an edit at the playhead.
4. **Add effects** from the Effects panel. Drag them onto clips, or select a clip and double-click the effect. Tune every parameter in the Inspector; the ◷ button animates a parameter with keyframes.
5. **Mix** in the Audio Mixer, and check levels on the meters and colour on the Scopes.
6. **Export** with `Ctrl+M`.

The first clip placed in an empty sequence sets the sequence's frame size and rate.

### Shortcuts (Premiere-style; F1 lists them all)

| Action | Key | Action | Key |
|---|---|---|---|
| Play / pause | Space | Shuttle back / stop / forward | J / K / L |
| Step frame | ← / → | Previous / next edit | ↑ / ↓ |
| Mark In / Out | I / O | Go to In / Out | Shift+I / Shift+O |
| Insert / overwrite | , / . | Lift / extract | ; / ' |
| Add edit | Ctrl+K | Add edit (all tracks) | Ctrl+Shift+K |
| Delete / ripple delete | Del / Shift+Del | Default transition | Ctrl+D |
| Audio crossfade | Ctrl+Shift+D | Speed / duration | Ctrl+R |
| Marker | M | Match frame | F |
| Snapping | S | Zoom in / out / fit | = / - / \ |
| Nudge clip | Alt+← / Alt+→ | New title | Ctrl+T |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z | Export | Ctrl+M |

## Command line

```bash
montage-cli probe clip.mov                                   # inspect media
montage-cli new -o cut.montage a.mov b.mov music.wav         # assemble a rough cut
montage-cli info cut.montage                                 # list tracks and clips
montage-cli render cut.montage -o cut.mp4 --preset "H.264 - YouTube / Vimeo"
montage-cli render cut.montage -o master.mov --preset "Apple ProRes 422 HQ" --in 00:00:10:00 --out 00:01:00:00
montage-cli frame cut.montage --at 00:00:05:12 -o poster.png
montage-cli scenes interview.mp4 --sensitivity 0.6           # list shot changes
montage-cli proxy a.mov -o a_proxy.mp4 --width 960
montage-cli loudness mix.wav                                 # integrated LUFS and peak
montage-cli edl cut.montage -o cut.edl                       # CMX 3600 EDL
montage-cli otio cut.montage -o cut.otio                     # OpenTimelineIO
montage-cli presets
```

## Architecture

```
src/core     Project model (value types), keyframes, effect catalogue, edit operations,
             snapshot undo, timecode, JSON project I/O.  No GUI dependencies.
src/media    FFmpeg probing, frame-accurate video decoding, audio decoding and peaks,
             decoder pool and LRU frame cache, scene detection, proxy generation.
src/render   CPU compositor (float RGBA, multi-threaded), effects and transitions,
             title rendering, stateful audio mixer, FFmpeg exporter.
src/app      Qt Widgets application: editor state, playback controller (render thread +
             audio sink), timeline, monitors, inspector, media bin, effects browser,
             scopes, mixer, meters, dialogs.
src/cli      montage-cli.
tests        Qt Test suites: core, render, media, and offscreen GUI integration tests.
```

Notes on the design:
- **Edits** are pure functions over a value-type `Project` (`core/EditOps.h`). Undo stores snapshots, so every edit is undoable without per-command inverse code. Interactive drags re-apply the operation to the state captured when the drag began, so the result depends only on how far the pointer has moved.
- **Rendering** uses 32-bit float, premultiplied RGBA throughout. Filters run in clip space before the fixed transform, as in Premiere. Decodes happen at the size the clip occupies on screen, which keeps preview cheap.
- **Playback** renders the newest requested frame on a worker thread and drops late frames. Paused frames always render at full quality.

## Roadmap

These come after the MVP:
- GPU compositor (Vulkan/OpenGL compute)
- ACES/OCIO colour management and HDR
- Multicam
- Speech-to-text captions (whisper.cpp)
- Speed ramps and optical-flow retiming
- Tracking and stabilisation
- OTIO import and FCP XML / AAF interchange
- Hardware encode and decode (NVENC/VAAPI/VideoToolbox)
- OpenFX plug-ins
