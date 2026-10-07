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
- Third-party audio plugins: VST3 and CLAP plugins run as clip effects. Their parameters are keyframeable and their settings are saved in the project. The Effects browser lists them by vendor, and Tools › Audio Plugins shows every plugin found and its status. LV2 and Audio Unit plugins are listed; hosting them is on the roadmap.
- Plugin scanning works like a DAW's: new or changed plugins load in a separate helper process, so one that crashes or hangs is blocked instead of taking Montage down. Results are cached, so later launches are quick.
- Crossfades: equal power or constant gain
- Loudness normalisation to -14, -16, -23 or -24 LUFS (ITU-R BS.1770 / EBU R128 gated measurement)
- Waveforms on the timeline

**Media and output**
- Imports anything FFmpeg reads
- Frame-accurate decoding that honours rotation, pixel aspect ratio, colour space and range
- Hardware decoding on by default (Playback › Hardware Decoding):
  - Devices: VideoToolbox on macOS; D3D11VA, D3D12VA, NVDEC or DXVA2 on Windows; VAAPI or NVDEC on Linux.
  - Any stream or device that isn't supported decodes in software instead, and at most 8 streams decode in hardware at once.
- Images as stills
- Proxy workflow: 960 px intra-frame proxies, toggled in the Program monitor
- Preview resolution: Full, 1/2, 1/4, 1/8
- Export presets: H.264 (x264), H.265 (x265, 8- and 10-bit), hardware H.264/H.265 (VideoToolbox, NVENC, Quick Sync, AMF or Media Foundation, falling back to x264/x265), Apple ProRes 422 HQ/LT/4444 (with alpha), Avid DNxHR, VP9, AV1 (SVT-AV1), WAV and AAC; exports either the whole sequence or In–Out
- Still frame export
- Transcription that runs on your computer (whisper.cpp; nothing is uploaded):
  - Right-click clips in the media bin › Transcribe…, pick a model (Tiny to Large v3 Turbo, English-only or 99 languages) and a language, or translate to English. The model downloads once.
  - Transcripts are word-timed, saved in the project, and undoable. The media bin search finds clips by what is said in them.
  - Export Transcript… writes SubRip (`.srt`) or WebVTT (`.vtt`) captions, plain text, or JSON with word timings.
- Edit by transcript (Transcript panel):
  - **Sequence mode** shows what the cut says, with the word under the playhead highlighted. Click a word to go there. Select words and press Delete to cut them out of every track; the gap closes and the captions move with it.
  - Remove Fillers cuts um, uh, er and similar words in one step. Shorten Pauses trims silences between words to a length you choose.
  - Find searches the words; the last word may be partly typed.
  - **Source mode** shows the Source monitor clip's transcript. Select words to set In and Out, then Insert or Overwrite them into the timeline.
- Captions:
  - Caption tracks on each sequence, shown as lanes above the video tracks. Drag captions to move them, drag their edges to retime them, and double-click one to edit its text in the Captions panel.
  - Generate captions from transcripts: words are placed where each clip plays them, following trims, speed changes and muted tracks, and are split into readable captions (42 characters per line, 2 lines and 7 seconds at most by default).
  - Edit, add, split, merge and delete captions in the Captions panel. You can also set the font, size, colours, background box, outline and position.
  - Import SubRip or WebVTT files. Export SubRip, WebVTT or Scenarist SCC (CEA-608 broadcast captions).
  - The CC button shows captions in the Program monitor. When exporting, captions can be burned into the picture, embedded as a subtitle track (mov_text in MP4/MOV, SubRip in MKV, WebVTT in WebM, with the track's language), or both.
- Interchange: CMX 3600 EDL and OpenTimelineIO (`.otio`) export, for finishing in Resolve, Premiere, Avid or Nuke
- Project files are readable JSON (`.montage`) and relink moved media through relative paths
- Crash safety:
  - Unsaved work is saved to a recovery copy about a second after each edit. After a crash or forced quit, the next launch offers to recover it.
  - If audio plugins were in use, it offers to start in safe mode with plugins disabled.
  - Every five minutes of editing, a timestamped snapshot is kept, the newest 20 per project (File › Open Auto-Save Snapshot).
- `montage-cli` for headless rendering and automation

## Installing

The [Package workflow](.github/workflows/package.yml) builds installers on every push and pull request; download them from the run's **Artifacts**. Pushing a version tag (`git tag v0.1.0 && git push origin v0.1.0`), or running the Package workflow from the Actions tab with a release tag, also publishes them as a GitHub Release.

- **macOS** (Apple Silicon, macOS 15 or later): `Montage-<version>-macos-arm64.dmg`. Open it and drag Montage to Applications. The app is not notarised yet, so the first launch is blocked: open System Settings › Privacy & Security and choose **Open Anyway**.
- **Windows** (x64, Windows 10 or later): `Montage-<version>-windows-x64-setup.exe` installs Montage, adds it to the Start menu and opens `.montage` projects on double-click. `Montage-<version>-windows-x64.zip` is the same app without installing. The installer is not code-signed yet, so SmartScreen warns: choose **More info › Run anyway**.

Both include `montage-cli`: inside `Montage.app/Contents/MacOS/` on macOS, next to `montage.exe` on Windows.

## Building

Ubuntu 24.04 or Debian with Qt ≥ 6.4 and FFmpeg ≥ 6:

```bash
./scripts/setup-deps.sh                 # installs compilers, Qt 6, FFmpeg dev packages
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/src/app/montage                 # the editor
./build/src/montage-cli --help          # the command-line tool
```

On macOS: `brew install qtbase qtmultimedia ffmpeg ninja pkgconf`, configure with `-DCMAKE_PREFIX_PATH="$(brew --prefix)"`, then `scripts/package-macos.sh build` makes the DMG. On Windows, build in an MSYS2 UCRT64 shell with the `qt6-base`, `qt6-multimedia`, `qt6-tools` and `ffmpeg` packages; `scripts/package-windows.sh build` makes the portable folder and zip, and `packaging/windows/montage.iss` (Inno Setup) the installer.

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
montage-cli models                                           # speech models and where they go
montage-cli transcribe interview.mp4 --model base.en --srt interview.srt --vtt interview.vtt
montage-cli captions cut.montage --transcribe base.en -o cut.scc --save   # caption a cut, keep the track
montage-cli render cut.montage -o cut.mp4 --burn-captions --embed-captions
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

Phase 2 is ranked in [`docs/research/phase2-roadmap.md`](docs/research/phase2-roadmap.md) and tracked in [`PROGRESS.md`](PROGRESS.md). Next up: dialogue denoise, masks, plugin editor windows, track and bus effect chains, Audio Unit and LV2 hosting, OCIO/ACES and HDR, multicam, a GPU compositor, tracking, optical-flow retiming and FCP XML interchange.
