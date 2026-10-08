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
- Masks on every video effect:
  - An ellipse or rectangle with feather, expansion, rotation, opacity and invert, all keyframeable. Drag it in the Program monitor to move it, or pull its handles to resize it.
  - An HSL qualifier selects by hue, saturation and luma with softness, for example to grade only skin or only the sky. Show Mask displays the selection.
  - **Object masks** (AI): choose the Object shape and click a person or thing in the Program monitor.
    - Alt-click marks what is not part of it, a drag draws a box around it, and Ctrl/Cmd-click removes a click.
    - ◀ Track / Track ▶ follow it through the clip, finding it again after it turns or is briefly hidden.
    - Feather, expansion, invert and Show Mask work as for the shapes, and the mask stays with the footage through trims and speed changes.
    - It runs locally with EdgeTAM, Meta's on-device model in the Segment Anything 2 family (Apache-2.0), on ONNX Runtime. The 65 MB model is downloaded on first use.
- Speed ramps and slow motion:
  - **Time Remapping** gives every clip a keyframeable speed curve (linear, hold or eased) inside its length. Picture and linked sound follow it together.
  - **Frame Sampling** sets how in-between frames are made in slow motion: Nearest Frame, Frame Blending, or Optical Flow, which moves pixels along their motion instead of cross-fading.
- Tracking and stabilisation (built in; no OpenCV):
  - **Stabilize** effect: analyses the clip's camera movement when added, then smooths it (or locks the shot). It can correct position, scale and rotation, and zooms to keep the edges hidden. Smoothness can be changed at any time without analysing again.
  - **Mask tracking**: ◀ Track / Track ▶ in any mask section follows what the mask covers from the playhead to the clip's start or end. It tracks position, scale and rotation, and writes keyframes.
  - Under the hood: pyramidal Lucas–Kanade on Shi–Tomasi corners with forward–backward checks, and RANSAC similarity fits.
- Gaussian blur, sharpen, vignette, mosaic, mirror, drop shadow
- **Adjustment layers** (Clip › New Adjustment Layer, or Generators in the Effects panel): the effects, opacity, transform and blend mode on the layer apply to everything on the tracks below it, for a grade or a look over many clips at once.
- Keyframes on every parameter: linear, hold or smooth, with previous/next navigation
- **Keyframes panel** (like the timeline in Premiere's Effect Controls): every animated parameter of the selected clip as a row of keys over the clip's length.
  - Click or drag a box to select keys, and drag them to retime (rows move together, without passing other keys).
  - Arrow keys nudge (Shift for 10 frames) and Delete removes. Right-click for Linear, Hold or Smooth, and double-click adds a key.
  - A click on the ruler or an empty lane moves the playhead.
- Volume and opacity lines on timeline clips (Sequence › Show Clip Volume / Show Clip Opacity):
  - Drag a line to raise or lower it. Ctrl/Cmd-click adds a keyframe; drag keyframes in time and value (Shift for value only).
  - Alt-click a keyframe to delete it, or right-click it for Linear, Hold or Smooth.
  - Volume is drawn on a perceptual scale up to +6 dB.
- Titles (font, outline, shadow, background box, alignment, tracking), colour mattes, gradients, SMPTE bars
- Transitions: cross dissolve, dip to black/white, wipe, push, slide, iris, cross zoom
- Scopes: waveform, RGB parade, vectorscope, histogram
- Colour management and HDR:
  - Media spaces come from the file's tags, and Interpret Colour overrides them: Rec.709, sRGB, Rec.2020, Display P3, Rec.2100 PQ and HLG, Sony S-Log3, ARRI LogC3 and LogC4, Panasonic V-Log, Canon Log 3, ACEScct.
  - Each sequence works in Rec.709, Rec.2020, P3, PQ or HLG. Clips are converted into it, with tone mapping between HDR and SDR and a display rendering for log footage.
  - HDR sequences are previewed tone mapped to SDR.
  - HDR exports are 10-bit and tagged, and PQ files carry HDR10 metadata. An HDR master can also be delivered as a tone-mapped SDR version.
  - Colour Space Transform effect: convert one clip between any two of these spaces.
  - OpenColorIO Transform effect (when built with OpenColorIO 2.1+):
    - Applies a studio's `.ocio` config, `$OCIO`, or a config built into OCIO (e.g. `ocio://studio-config-latest`).
    - Converts colour space to colour space, or to a display/view with an optional look; it can also run inverted.
    - The Inspector lists the config's spaces, displays, views and looks.
    - ACES 2.0 output transforms come with OCIO 2.5's built-in configs.
  - The camera log curves and gamuts match OpenColorIO's reference ACES transforms to 1e-4. They use the makers' published matrices to ACES.

**Multicam**
- Create Multicam Clip (media bin) makes angles from any number of cameras, synced by their sound, by timecode or by in points. Every source with sound gets an audio track.
- The Multicam panel shows every angle side by side:
  - click an angle or press 1–9 to switch;
  - while the program plays, each switch is a cut at the playhead, so a scene can be cut live;
  - Shift cuts while stopped;
  - audio can follow the video.
- Auto Switch cuts to whoever is speaking, a wide angle covers silence and cross-talk, and there is a shortest-shot length. Who speaks comes from either:
  - each person's microphone (each close-up listens to one, with a speaker margin);
  - the speaker labels of one transcribed recording of everyone (choose the angle that shows each person).
- On the timeline, clips are labelled with their angle. Multicam Angle and Multicam Audio (one source or all mixed) can be changed per clip, and Flatten Multicam swaps in the cameras' own clips.

**Audio**
- Clip gain and pan, both keyframeable
- **Auto Duck Music** (Clip menu): select music clips, tick the dialogue tracks, and the music dips under speech, fading down before it and back up after it. Speech is found from transcripts' words, or from loudness. The result is volume keyframes you can adjust on the clip's line.
- Track faders, pan, mute and solo in a mixer panel
- Insert effects on audio tracks, buses (submixes) and the master, including plugins. Use a strip's FX button to edit them in the Inspector, route tracks to buses from the strip's output menu, and add buses with + Bus. Track effects keep running past the last clip, so reverb and echo tails ring out.
- Plugin delay compensation: the latency plugins report is compensated on clips, tracks, buses and the master, so everything stays in sync with the picture and with other tracks, including straight after a seek.
- Render and Replace bakes an audio clip's effects into a new audio file to save CPU, and Restore Unrendered brings the original back (both on the clip's right-click menu).
- Peak meters with hold and clip indicators
- Effects:
  - EQ: 3-band and 5-band parametric EQ (low shelf, three bells, high shelf), high/low-pass.
  - Dynamics: compressor, limiter, de-esser (split-band), noise gate (threshold, range, attack, hold, release).
  - Space: reverb (Freeverb design) and delay.
  - Channel tools: fill both sides from the left or right microphone, mono, swap, and polarity invert.
- Third-party audio plugins: VST3, CLAP and LV2 plugins, and Audio Units on macOS, run as clip, track, bus and master effects. Their parameters are keyframeable, their settings are saved in the project, and their latency is compensated. LV2 hosting uses lilv when it is installed. The Effects browser lists plugins by vendor, and Tools › Audio Plugins shows every plugin found and its status.
- Plugin editors: the Editor button on a plugin effect opens the plugin's own interface in a window, as in a DAW. Turning its knobs makes ordinary undoable, keyframe-aware parameter edits. The plugin's settings are saved in the project. Undo and Inspector changes show up in the open editor.
- Plugin scanning works like a DAW's: new or changed plugins load in a separate helper process, so one that crashes or hangs is blocked instead of taking Montage down. Results are cached, so later launches are quick.
- Dialogue cleanup (Effects › Audio Filters › Restoration):
  - **Noise Reduction** learns the noise print from the quietest moments of the recording itself, then turns down hum, hiss and room tone by up to the amount you set.
  - **Voice Isolation** uses RNNoise, a neural network from Xiph, to keep speech and remove everything else, mixed with the original by Amount.
  - Both process the clip's whole source audio in the background and cache the result. Playback uses the original audio until the cleaned copy is ready (usually a few seconds); exports always wait for it.
- Crossfades: equal power or constant gain
- Loudness normalisation to -14, -16, -23 or -24 LUFS (ITU-R BS.1770 / EBU R128 gated measurement)
- Waveforms on the timeline

**Organising media**
- Bins, nested to any depth: drag media or bins onto a bin in the bin tree, and drop files on a bin to import into it. Deleting a bin keeps its media.
- Icon view, or a list view with sortable columns: rating, label, duration, type, resolution, frame rate, start timecode, codecs, keywords, usage in sequences, scene, shot, take, camera, camera model, description, comment, recording date, colour space, transcript, proxy, bin and file. Right-click the header to choose columns.
- Logging:
  - Ratings: press 1–5 for stars, 0 to clear, and X to reject.
  - Colour labels, keywords, and metadata fields, edited in the list (an edit applies to every selected row) or from the right-click menu.
  - The recording date and camera model are read from camera files.
- **Subclips**: mark In and Out in the Source monitor and choose Clip › Make Subclip (Ctrl+U), or right-click a Find Shots result.
  - A subclip is a bin item of its own, to rate, tag and find. It opens with In and Out around its range, and dragging it to the timeline places that range under its name.
  - Its Usage counts the clips that play part of it, and searches look only at the words spoken inside it.
- The search box finds names, keywords, metadata and what is said; "quoted text" finds a phrase. It searches inside the bin's bins too.
- **Smart bins** (saved searches) list the media matching their rules and stay up to date, for example "rating at least 4 and keyword includes interview". Rules cover every column, any text, and usage, so "used 0 times" finds what hasn't been cut in.

**Media and output**
- Imports anything FFmpeg reads
- Frame-accurate decoding that honours rotation, pixel aspect ratio, colour space and range
- Hardware decoding on by default (Playback › Hardware Decoding):
  - Devices: VideoToolbox on macOS; D3D11VA, D3D12VA, NVDEC or DXVA2 on Windows; VAAPI or NVDEC on Linux.
  - Any stream or device that isn't supported decodes in software instead, and at most 8 streams decode in hardware at once.
- Images as stills
- Proxy workflow: 960 px intra-frame proxies, toggled in the Program monitor
- Preview resolution: Full, 1/2, 1/4, 1/8
- **Render queue** (as in Media Encoder and Resolve): Add to Queue in the Export dialog. Queued exports render one after another in the background while you edit, each from the project as it was when queued. The Render Queue panel has Start, Stop, Retry, Remove, Clear Finished and Show File.
- Loudness for delivery (Export › Loudness, or `--loudness -14`): the whole mix is measured first (ITU-R BS.1770 / EBU R128) and set to -14, -16, -23 or -24 LUFS. A look-ahead limiter keeps true peaks under -1 or -2 dBTP.
- Export presets: H.264 (x264), H.265 (x265, 8- and 10-bit), hardware H.264/H.265 (VideoToolbox, NVENC, Quick Sync, AMF or Media Foundation, falling back to x264/x265), Apple ProRes 422 HQ/LT/4444 (with alpha), Avid DNxHR, VP9, AV1 (SVT-AV1), WAV and AAC; exports either the whole sequence or In–Out
- Still frame export
- Transcription that runs on your computer (whisper.cpp; nothing is uploaded):
  - Right-click clips in the media bin › Transcribe…, pick a model (Tiny to Large v3 Turbo, English-only or 99 languages) and a language, or translate to English. The model downloads once.
  - Transcripts are word-timed, saved in the project, and undoable. The media bin search finds clips by what is said in them.
  - Export Transcript… writes SubRip (`.srt`) or WebVTT (`.vtt`) captions, plain text, or JSON with word timings.
  - **Label speakers** (in the Transcribe dialog) also works out who speaks when, for any number of people or a number you give.
    - Every word is labelled. The Transcript panel names the speaker at each change; rename them with a right-click.
    - Plain-text exports are split by speaker, WebVTT carries voice tags, and captions break where the speaker changes.
    - It runs locally: pyannote segmentation 3.0 (MIT) and the CAM++ voice model (Apache-2.0) on ONNX Runtime, a 36 MB one-time download.
- **Find Shots**: search footage by what it shows ("a dog running on a beach", "close-up of hands").
  - Videos are indexed once, a frame every two seconds, and the index is saved with the project.
  - Results show a thumbnail and the moment; opening one marks it with In and Out in the Source monitor.
  - It runs locally: CLIP ViT-B/32 (OpenAI, MIT) on ONNX Runtime, a 190 MB one-time download.
  - On the command line: `montage-cli shots project.montage "a red car at night"`.
  - Right-click a result to save it as a subclip named after the search.
- **Auto-Tag Shots** (right-click videos in the media bin): adds keywords for what each shot shows, using the same index. The keywords are Close-up, Medium shot or Wide shot; Interior or Exterior; Day or Night; and People. They work in searches and smart bins, and a subclip is tagged from its own range.
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
- Interchange both ways with Premiere Pro, DaVinci Resolve, Final Cut Pro, Avid and Nuke:
  - Final Cut Pro 7 XML, FCPXML, OpenTimelineIO and CMX 3600 EDL.
  - Imported timelines find their media (missing files come in offline) and keep links, dissolves, speed changes, titles and markers.
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

Optional libraries are used when found: OpenColorIO (OCIO transforms), lilv (LV2 plugins) and ONNX Runtime (object masks). `setup-deps.sh` installs all three; ONNX Runtime goes to `/opt/onnxruntime` from Microsoft's release archive (point `ONNXRUNTIME_ROOT` at another copy).

On macOS: `brew install qtbase qtmultimedia ffmpeg opencolorio lilv onnxruntime ninja pkgconf`, configure with `-DCMAKE_PREFIX_PATH="$(brew --prefix)"`, then `scripts/package-macos.sh build` makes the DMG. On Windows, build in an MSYS2 UCRT64 shell with the `qt6-base`, `qt6-multimedia`, `qt6-tools`, `ffmpeg`, `opencolorio`, `lilv` and `onnxruntime` packages; `scripts/package-windows.sh build` makes the portable folder and zip, and `packaging/windows/montage.iss` (Inno Setup) the installer.

CMake options:
- `-DMONTAGE_BUILD_APP=OFF` builds only the engine and CLI, without Qt Widgets or Multimedia.
- `-DMONTAGE_BUILD_TESTS=OFF` skips the tests.
- `-DMONTAGE_REQUIRE_ONNXRUNTIME=ON` stops the configure step if ONNX Runtime is missing (release builds use it).

The object mask, speaker and visual search tests need their models: `scripts/fetch-models.sh ~/montage-models`, then set `MONTAGE_OBJECT_MODEL=~/montage-models/edgetam-video`, `MONTAGE_SPEAKER_MODEL=~/montage-models/speakers` and `MONTAGE_VISUAL_MODEL=~/montage-models/clip-vit-b32` (the app reads the same variables). The speech tests read `MONTAGE_TEST_WHISPER_MODEL`.

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
montage-cli xml cut.montage -o cut.xml                       # Final Cut Pro 7 XML (Premiere, Resolve)
montage-cli fcpxml cut.montage -o cut.fcpxml                 # FCPXML (Final Cut Pro)
montage-cli import edit.fcpxml -o edit.montage               # FCP XML, FCPXML, OTIO or EDL into a project
montage-cli presets
montage-cli models                                           # speech and object models, and where they go
montage-cli transcribe interview.mp4 --model base.en --srt interview.srt --vtt interview.vtt
montage-cli transcribe podcast.wav --speakers 2 --txt podcast.txt          # who said what
montage-cli captions cut.montage --transcribe base.en -o cut.scc --save   # caption a cut, keep the track
montage-cli render cut.montage -o cut.mp4 --burn-captions --embed-captions
montage-cli new -o hdr.montage --color-space rec2100pq --hdr-peak 1000 a.mov   # an HDR10 sequence
montage-cli render hdr.montage -o hdr10.mp4 --vcodec libx265                   # 10-bit HDR10 with metadata
montage-cli render hdr.montage -o sdr.mp4 --color-space rec709                 # tone-mapped SDR version
montage-cli colorspaces                                      # colour space ids
montage-cli mcp                                              # MCP server for AI agents (see below)
```

## AI agents (MCP)

`montage-cli mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server, so Claude and other MCP clients can edit with Montage. It works over stdio and speaks both the 2026-07-28 revision and the earlier initialize handshake (2024-11-05 to 2025-11-25).

Its tools work on `.montage` files by path. Each edit is saved at once, and the previous version is kept beside the project as `.bak` for `montage_undo`. The tools can:
- probe media, create projects and list a project's timeline;
- place media, split, remove (with ripple), move, trim and change the speed of clips;
- add titles, effects (including masked ones), transitions and markers;
- transcribe (with speaker labels) and find spoken phrases in the cut;
- find shots by description;
- log media (ratings, labels, keywords, metadata fields, bins), make subclips, auto-tag shots, and find media by text or smart-bin rules, optionally saving the rules as a smart bin;
- duck music under dialogue, and add adjustment layers;
- return a rendered frame as an image so the agent can check its work;
- render with any preset, with progress;
- export and import EDL, OTIO, FCP 7 XML and FCPXML.

```bash
claude mcp add montage -- montage-cli mcp      # Claude Code
```
Other clients take the same command in their MCP settings, e.g. `{"mcpServers": {"montage": {"command": "/Applications/Montage.app/Contents/MacOS/montage-cli", "args": ["mcp"]}}}`. Reopen a project in the app to see changes an agent made.

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

Phase 2 is ranked in [`docs/research/phase2-roadmap.md`](docs/research/phase2-roadmap.md) and tracked in [`PROGRESS.md`](PROGRESS.md). Next up: a GPU compositor, HDR monitoring, and more of Phase 3 (metadata, ratings and smart bins).
