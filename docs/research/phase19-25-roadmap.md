<!-- Approved by the owner on 27 November 2026. Phases 19 to 25 follow Phase 18 in this order. -->
# Seven phases carry Montage past Phase 18

## Executive summary

**Seven more phases (19 to 25, 105 items) close the gap that can be counted today.** After Phase 18 the repository still records about **80 open in-policy notes**, and the November 2026 competitor catalogues add about **15 features** Montage lacks. At 14 items per phase plus the standing "own leftovers" item, that is **six professional phases**. The seventh, **Phase 20, is reserved for the optional simple mode** another thread is designing. It sits on top of the full app and removes no professional depth. Its three dependency slots sit in Phase 19.

The phases are:
- 19: long threads and packaging
- 20: simple mode
- 21: colour and audio depth
- 22: VFX, 3D and motion graphics
- 23: editorial, media and captions
- 24: delivery, interchange and sound handover
- 25: live work, review, scale and real-hardware checks

"Rivalling the best" beyond that count is a judgement call: competitors ship yearly. The biggest remaining differences are owner decisions outside these phases: cloud collaboration, generated media, closed SDKs, Dolby formats and signed installers. Lifting them all adds about three to four phases.

I treated Final Cut Pro 13 and Premiere 27 as released; no item depends on them alone.

## Eighty open notes and fifteen competitor features make six professional phases

Montage is already unusually broad. In about 387,000 lines of C++ it covers editing, grading, compositing, DAW-style audio post, IMF/DCP mastering and ST 2110 broadcast IP. The inventory concludes that **the remaining gaps are narrow**: closed SDKs, cloud features, verification on real hardware and "last 10 %" depth ([inventory, from README and PROGRESS](../../PROGRESS.md)).

Phase 18 is itself a consolidation phase. Eleven of its 15 items continue older threads, and **items 13 to 15 cannot be fully checked in Linux CI**. Its predecessors left "Not done" notes in the same situation, so Phase 18 will too. Those leftovers are folded into Phase 19.

The count below sets open, in-policy notes from PROGRESS.md against features seen only in the 2026 competitor catalogues. Related notes are grouped into slots, so the columns do not add across.

| Area | Open notes in Montage after Phase 18 | Seen only in competitors | Slots (phase) |
|---|---|---|---|
| Long threads Phase 18 will leave (GPU effects, GPU handles, HDR on Linux, 3D titles, sound timing) | 5 | – | 5 (19) |
| Platform, performance, scale | 6 | 2 | 8 (19, 25) |
| Scripting, jobs, agents | 3 | 2 | 5 (19, 25) |
| Colour and HDR | 7 | – | 9 (21) |
| Audio post | 8 | 2 | 9 (21, 24) |
| VFX, 3D, motion graphics | 12 | 5 | 14 (22) |
| Editorial, media, camera | 14 | 1 | 12 (23) |
| Captions, transcripts, languages | 5 | – | 3 (23, 25) |
| Delivery and interchange | 11 | – | 10 (24) |
| Live, capture, review | 5 | – | 5 (25) |
| AI tools (in policy) | 1 | – | 1 (25) |
| Onboarding, templates, simple mode | 3 | 3 | 3 (19) + 15 (20) |
| Standing "own leftovers" | – | – | 6 |
| **Total** | **80** | **15** | **105** |

The competitor column is short because Montage already matches most 2026 headline features. Examples include a Baselight-8-style grain model, ISO 21496-1 gain-map HDR stills, MV-HEVC spatial video, a photosensitive-flash QC pass, and a Rebuild-LUT-as-Grade tool comparable to Chromogen ML ([README](../../README.md)). What remains on the competitor side is concentrated in a few places:
- 3D: splat relighting and export, field masks and non-destructive USD export ([CG Channel, Nuke 17.1](https://www.cgchannel.com/2026/08/foundry-releases-nuke-17-1/)).
- In-UI shader code ([CG Channel, Flame 2027.1](https://www.cgchannel.com/2026/07/autodesk-releases-flame-2027/)).
- Reference EQ and level matching ([Blackmagic](https://www.blackmagicdesign.com/products/davinciresolve/whatsnew)).
- Transform matching to an offline reference ([Digital Production, Baselight 7](https://digitalproduction.com/2026/01/23/baselight-v7-refines-the-colourists-toolkit/)).
- Plan-and-approve agents ([TechTimes, Adobe MAX](https://www.techtimes.com/articles/330572/20261112/adobe-max-2026-recap-firefly-gets-planning-mode-creative-cloud-goes-open-agentic-ai-takes-over.htm)).

**Where this stops being a count.** The tally has an error of about ±1 phase, for three reasons:
- **The inventory is incomplete.** It read long "Not done" notes only to about 600 characters.
- **Part of the catalogue rests on background knowledge** that the researchers did not re-verify.
- **The target moves.** Nuke 18 is in beta, Baselight 8 is due in 2027 and Adobe's MCP agent access is due in early 2027 ([CG Channel](https://www.cgchannel.com/2026/10/foundry-releases-nuke-18-0-in-open-beta/); [Digital Production](https://digitalproduction.com/2026/09/11/baselight-8-the-feature-list-is-the-easy-part/); [Creative Boom](https://www.creativeboom.com/news/adobe-max-2026-all-the-big-announcements/)).

Two parts of "rivalling" no phase can reach:
- **Policy exclusions:** shared-project bin locking, which Avid treats as central enough to block emulators ([NewscastStudio](https://www.newscaststudio.com/2026/08/27/avid-media-composer-20068/)), and cloud or generated media.
- **Credibility gained only in real facilities:** much of Montage is proven only against synthetic CI fixtures, its macOS build is not notarised and its Windows build is not signed ([README L472-506](../../README.md)).

My judgement is that seven phases make Montage a credible rival on features. Matching the leaders' reputation also needs the owner's decisions and real-world testing.

## Final Cut Pro 13 and Premiere 27 shipped; one researcher's search missed them

Two researchers cite dated primary sources:
- **Final Cut Pro 13.0** on 18 Nov 2026, with Apple's own release notes and a dated hands-on ([Apple](https://support.apple.com/en-us/102825); [AppleInsider](https://appleinsider.com/articles/26/11/25/hands-on-final-cut-pro-13-heavy-on-features-value-for-creator-studio-users)).
- **Premiere 27.0** on 10 Nov 2026 at MAX ([CineD](https://www.cined.com/adobe-premiere-27-0-released-color-mode-ai-assistant-panel-and-generative-audio-tools/); [PRONEWS](https://en.pronews.com/news/20261112145916659.html)).

Montage's own Phase 18 analysis also benchmarks both. The third researcher had a 20-call budget, and both releases were under three weeks old. That researcher found the MAX AI Assistant beta but not the version label.

**I relied on both versions existing.** No proposed item depends on them alone:
- Their main new features are already in Montage (keyframe easing, a CLI, a colour workspace).
- Or they are excluded (generated audio, credit-metered assistants).
- Or they are deferred for want of a usable model (crosstalk separation).

## One line per phase

| Phase | Theme | Main items |
|---|---|---|
| 19 | Finish the long threads and ship everywhere | GPU effects closing pass, Linux and Arm packages, embedded Python, hardware self-test, templates and first-run (simple-mode dependencies), GPU handles and HDR-on-Linux remainders |
| 20 | Simple mode (reserved) | Local suggestion runtime, verified model downloads, starter CC0 pack, beginner export presets, ten slots for the design thread |
| 21 | The colourist's and mixer's last tenth | Grade-graph splitters, mattes and per-node colour space, HDR trim views, gallery depth, dynamic and reference-matched EQ, fifth-order ambisonics |
| 22 | VFX, 3D and motion graphics depth | USD composition and export, lit and exported splats, shader-code node, zoom camera solves, 3D scene view, physically based materials |
| 23 | Editorial, media and captions depth | Transform matching, conform playback against the reference, script revisions, three more log curves, recovering more interrupted recordings, caption timing |
| 24 | Delivery, interchange and sound handover | Multi-segment IMF, Interop and stereo DCPs, more pulldown cadences, IAMF with picture, open DAW project exchange, per-instance plug-in hosts |
| 25 | Live work, review, scale and real-hardware proof | Render-farm services, multi-GPU, plan-and-approve assistant, project history, review on phones, ST 2110 kernel bypass |

**How the phases are ordered.** Phases are ranked by value × feasibility, and within each phase the items that CI can test completely come first. An item marked **†** has a part that Linux CI cannot check. No item needs an account, an API key or a closed SDK.

**Two exceptions to the ranking:**
- **Phase 20 is placed by owner priority.** The ranking would never select it, because no roadmap from Phase 2 to 18 has proposed onboarding.
- **Phase 25 comes last because of hardware.** It needs real hardware or people more than any other phase, which pushes it down under the method's rule.

**Approvals and flagged dependencies.** New CI builds (Arm, Linux packages) need S's approval under the project's gate. Dependencies whose licence needs checking are marked "flag".

## Fifteen items per phase

**Size key:** S ≤ 2 person-weeks, M 2–6, L 6–12. As with past phases, sizes show relative scope, not calendar time.

### Phase 19: finish the long threads and ship everywhere

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | Last effects on the GPU, closing pass (what Phase 18 #8 leaves; no CPU fallback) | L | [PROGRESS L2219, L2379](../../PROGRESS.md) |
| 2 | Linux packages built in CI (self-contained and sandboxed bundles) | M | [package.yml](../../.github/workflows/package.yml) |
| 3 | 3D titles, fourth part (what Phase 18 #7 leaves) | S | [PROGRESS L2339](../../PROGRESS.md) |
| 4 | Every analysis in the job store, resumable | S–M | [PROGRESS L2131, L2211](../../PROGRESS.md) |
| 5 | Embedded Python with per-script environments | M | [PROGRESS L1028](../../PROGRESS.md) |
| 6 | Script editor (highlighting, completion); Lua fixes (non-ASCII on Windows, non-blocking shell calls) | S–M | [PROGRESS L987, L1028](../../PROGRESS.md) |
| 7 | Project and sequence templates (simple-mode dependency) | S–M | [MainWindow.cpp L440](../../src/app/MainWindow.cpp) shows a status-bar welcome only |
| 8 | First-run window with a sample project of CC0 media (simple-mode dependency) | M | Bundled starter assets in 22 MB ([Shotcut 26.8](https://shotcut.org/blog/new-release-26.8.1/)) |
| 9 | Reserved for the simple-mode thread: plumbing for proposed edits (previewable, undoable, through the existing command and MCP layer), plus a dismissed-forever store in Preferences | M | Agent edits land as ordinary editable clips ([Eddie AI guide](https://www.heyeddie.ai/learn/premiere-pro/26.5.2/ai-features-in-premiere-pro)) |
| 10 | Arm builds for Windows and Linux (new CI, needs S's approval) | M | Premiere native on Windows on Arm ([Newsshooter](https://www.newsshooter.com/2026/01/20/whats-new-in-adobe-premiere-pro-26-0/)) |
| 11 | Hardware self-test that users run and file: 4K speed, HDR output, audio round trip, control surfaces | M | "Real time at 4K" unverified ([PROGRESS L1048, L1752](../../PROGRESS.md)) |
| 12 | Montage's own leftovers | S–M | Standing item since Phase 15 |
| 13† | HDR on Linux, fifth part (other desktop compositors, a real HDR display) | S | [PROGRESS L2139, L2422](../../PROGRESS.md) |
| 14† | GPU handles, third part (macOS and Windows paths; decoder frames kept on the GPU) | M | [PROGRESS L2000, L2428](../../PROGRESS.md) |
| 15† | Sound timing on real machines, second part (what Phase 18 #13 leaves) | S | Phase 18 #13 (branch `claude/project-thread-kfccn3`) |

### Phase 20: simple mode (reserved for the design thread)

**Constraint on every slot below:** the simple mode is an optional switch, an extra view over the same project and the same full professional app. It never replaces, hides by default, cuts or simplifies a professional feature. Switching back shows everything, unchanged.

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | Local language-model runtime for offline suggestions, if the design needs one (Montage ships no LLM; flag each model's weight licence) | M | [README L624](../../README.md) |
| 2 | Optional model downloads, hash-verified and licence-checked | S–M | Shotcut fixed its Whisper model verification ([Linux Adictos](https://en.linuxadictos.com/Shotcut-26.9-is-here!-New-design--auto-duck--and-security-improvements.html)) |
| 3 | CI harness for suggestions: fixture projects, fixed seeds | S–M | Method: CI-testable first |
| 4 | Starter asset pack (CC0 music, sound effects, titles, transitions) | S–M | [Shotcut 26.8](https://shotcut.org/blog/new-release-26.8.1/) |
| 5 | Beginner export presets on top of Export Versions | S | [README L181, L388-393](../../README.md) |
| 6–15 | Ten slots reserved for the simple-mode design (suggestion surface, regeneration, dismissal, the optional switch and so on, as that thread specifies), all layered on top of the full app | Set by the thread (budget about 10 × M) | Simple flows now start from auto-edit ([CapCut](https://www.capcut.com/help/how-to-use-auto-cut); [MacRumors](https://www.macrumors.com/2026/01/28/final-cut-pro-and-logic-pro-updated/)) |

### Phase 21: the colourist's and mixer's last tenth

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | Grade graph: splitter/combiner and compound nodes | M | [PROGRESS L1025](../../PROGRESS.md) |
| 2 | Grade graph: alpha outputs, external mattes, per-node colour space | M | Mattes rendered in place in Resolve 21 ([Newsshooter](https://www.newsshooter.com/2026/06/02/davinci-resolve-21-final-release/)) |
| 3 | HDR trims: master and target side by side, in nested sequences, with Export Versions | M | [PROGRESS L1065, L1216](../../PROGRESS.md) |
| 4 | EQ and level matching to a reference | M | [Blackmagic](https://www.blackmagicdesign.com/products/davinciresolve/whatsnew); [PROGRESS L1859](../../PROGRESS.md) |
| 5 | Dynamic EQ | S–M | [PROGRESS L1859](../../PROGRESS.md) |
| 6 | Gallery: multi-still split screen, HDR EXR stills, albums across projects, strip filters, lightbox reordering | M | [PROGRESS L1062](../../PROGRESS.md) |
| 7 | Node thumbnails and graphs for colour groups' pre/post grades | S–M | [PROGRESS L1025](../../PROGRESS.md) |
| 8 | Ambisonics to fifth order (Montage has third) | M | [Resolve manual ch. 184](https://docs.imaster.org/davinci-resolve-21-manual/part-13-fairlight/ch.-184-immersive-audio-workflows) |
| 9 | Exact Y′CbCr and range conversion, measured | S | "More Accurate" setting ([Digital Production](https://digitalproduction.com/2026/11/25/baselight-7-0-4-presets-transcoding-chromogen-ml/)) |
| 10 | Dragging primaries on the scope | S | [PROGRESS L1716](../../PROGRESS.md) |
| 11 | Dialogue match: true-stereo rooms, tails past the clip end | S | [PROGRESS L1102](../../PROGRESS.md) |
| 12 | Wow and flutter for speech-only sound; azimuth beyond two channels | S | [PROGRESS L1710](../../PROGRESS.md) |
| 13 | Montage's own leftovers | S–M | Standing item |
| 14† | Colour panels: maps for HDR palette, curves and warper; several modes; default maps | M | [PROGRESS L1060](../../PROGRESS.md) |
| 15† | Display calibration with a measuring probe (flag: measurement library licence) | M | [PROGRESS L1122](../../PROGRESS.md) |

### Phase 22: VFX, 3D and motion graphics depth

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | USD composition arcs (inherits, specializes, relocates) and non-destructive USD export | M | [PROGRESS L1554](../../PROGRESS.md); [CG Channel](https://www.cgchannel.com/2026/08/foundry-releases-nuke-17-1/) |
| 2 | Splats lit and shadowed by scene lights; splat export | M | [CG Channel, Nuke 17.1](https://www.cgchannel.com/2026/08/foundry-releases-nuke-17-1/) |
| 3 | Shader-code node in the composite graph | M | [CG Channel, Flame 2027.1](https://www.cgchannel.com/2026/07/autodesk-releases-flame-2027/) |
| 4 | Camera solve with zoom lenses and moving objects masked out | M | [PROGRESS L1035](../../PROGRESS.md) |
| 5 | 3D scene view for solves and 3D layers | M | [PROGRESS L1035](../../PROGRESS.md); [CG Channel, Nuke 17.0](https://www.cgchannel.com/2026/02/foundry-releases-nuke-17-0-nukex-17-0-nuke-studio-17-0/) |
| 6 | Open physically based materials on 3D text and models; clips as material sources | L | [Newsshooter, Fusion OpenPBR](https://www.newsshooter.com/2026/06/02/davinci-resolve-21-final-release/); [Typoanim, AE 27](https://www.typoanim.com/blog/after-effects-2027-new-features) |
| 7 | Keyer plate tracking and surface tracks inside the composite graph | S–M | [PROGRESS L1669, L1723](../../PROGRESS.md) |
| 8 | Restoration: planar-tracked patches, slanted scratches, two-frame dirt, marks carried through Stabilise, Lens and Reframe | M | [PROGRESS L1111](../../PROGRESS.md) |
| 9 | Faces over time: lids, gaze, steadied mesh; lip sync for every face, smoothed | M | [PROGRESS L1159-1162, L1186](../../PROGRESS.md) |
| 10 | Volume (field) masks in 3D | S–M | [CG Channel, Nuke 17.0](https://www.cgchannel.com/2026/02/foundry-releases-nuke-17-0-nukex-17-0-nuke-studio-17-0/) |
| 11 | Sky replacement with a steadied matte and water reflections | S–M | [PROGRESS L1138-1142](../../PROGRESS.md) |
| 12 | Vector animation files: packaged form, live text with installed fonts, shape-layer fidelity (keyed sizes, per-item opacity, all dashes, keyframeable gradient stops) | M | [PROGRESS L494-497, L1681](../../PROGRESS.md) |
| 13 | HTML broadcast graphics in real time on live outputs, with keyframed data | M | [PROGRESS L1038](../../PROGRESS.md); [RedShark](https://www.redsharknews.com/davinci-resolve-21-1-2-whats-new) |
| 14 | Layered vector artwork imported with editable text | M | [Typoanim, AE 27](https://www.typoanim.com/blog/after-effects-2027-new-features) |
| 15 | Montage's own leftovers | S–M | Standing item |

### Phase 23: editorial, media and captions depth

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | Transform matching to an offline reference | M | [Digital Production, Baselight 7](https://digitalproduction.com/2026/01/23/baselight-v7-refines-the-colourists-toolkit/) |
| 2 | Conform Check: reference plays in step with wipe or difference; take lookup by picture | M | [PROGRESS L1193](../../PROGRESS.md) |
| 3 | Script view: two-up from the script, revisions and colours, live layout while typing | M | [PROGRESS L1099](../../PROGRESS.md) |
| 4 | Nest In to Out keeps transitions and track settings | S | [PROGRESS L2117](../../PROGRESS.md) |
| 5 | Track groups that hold folders; switches per track | S–M | [PROGRESS L1898](../../PROGRESS.md) |
| 6 | Three more camera log curves (those with published formulas) | S | [PROGRESS L1791](../../PROGRESS.md) |
| 7 | Recovering more interrupted recordings (more MXF codecs, clip-wrapped files, timecode tracks) | M | [PROGRESS L1203](../../PROGRESS.md) |
| 8 | Captions follow shot changes inside clips; optional EBU-TT-D metadata; IMSC 1.3 | S–M | [PROGRESS L1703](../../PROGRESS.md) |
| 9 | Regenerated words aligned to their own result; transcript translation | S–M | [PROGRESS L464, L1196](../../PROGRESS.md) |
| 10 | Camera metadata: remaining raw-image opcode lists, lens-centre offsets, framing decision lists inside camera files, DPX header fields | S–M | [PROGRESS L1018, L1731, L1791](../../PROGRESS.md) |
| 11 | MLV: dual ISO, focus pixels, compressed files | S | [PROGRESS L1057](../../PROGRESS.md) |
| 12 | Timeline Sort; edit-keyboard shuttle trim and timecode keypad (keyboard part †) | S | [PROGRESS L1075](../../PROGRESS.md) |
| 13 | Montage's own leftovers | S–M | Standing item |
| 14† | Gyro data from more cameras, with lens distortion in the warp (needs sample files) | M | [PROGRESS L1108](../../PROGRESS.md) |
| 15† | Tethered stills capture (flag: camera-control library licence) | M | Tethering in Resolve 21's Photo page ([Newsshooter](https://www.newsshooter.com/2026/06/02/davinci-resolve-21-final-release/)) |

### Phase 24: delivery, interchange and sound handover

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | IMF with several segments | M | [PROGRESS L1207](../../PROGRESS.md) |
| 2 | Interop DCPs and 4K stereoscopic DCPs | M | [PROGRESS L1199](../../PROGRESS.md) |
| 3 | More pulldown cadences and interlaced deliveries; burn-ins and captions at the delivered rate | M | [PROGRESS L1211](../../PROGRESS.md) |
| 4 | IAMF with picture in the same file, Opus/AAC and demixing | M | [PROGRESS L1227](../../PROGRESS.md); Resolve lacks IAMF export ([Spatial9](https://spatial9.ai/blog/davinci-resolve-to-eclipsa-audio-workflow)) |
| 5 | Expressions and easings carried through interchange (keyframes in timeline exchange files) | S–M | [PROGRESS L1183-1185, L2366](../../PROGRESS.md) |
| 6 | ADM import in full: frame-wrapped and multi-track files, compressed S-ADM, every programme | S–M | [PROGRESS L1696](../../PROGRESS.md) |
| 7 | Open DAW project exchange, both ways | M | [Sound on Sound, Nuendo 14](https://www.soundonsound.com/news/steinberg-nuendo-14-arrives) |
| 8 | Camera RAW video in IMF; lossy HTJ2K with rate control | S–M | [PROGRESS L1116, L1855](../../PROGRESS.md) |
| 9 | Broadcaster ingest profiles for long-GOP joins | M | [PROGRESS L1908](../../PROGRESS.md) |
| 10 | HDR quality scoring and per-shot live ladders (flag: HDR scoring model availability) | M | [PROGRESS L2196, L2346](../../PROGRESS.md) |
| 11 | VVC encoding and xHE-AAC decoding (flag: encoder licence) | M | Phase 18 lower-priority list |
| 12 | ST 2094-60 metadata and ST 2120-1/2134 time labels | S | Phase 18 lower-priority list |
| 13 | One host per plug-in instance; sidechain keys for every plug-in format | M | [PROGRESS L939, L1875](../../PROGRESS.md) |
| 14 | Montage's own leftovers | S–M | Standing item |
| 15† | Several sound devices at once; sampler per-slot outputs and clock sources | M | [PROGRESS L1088, L2109](../../PROGRESS.md) |

### Phase 25: live work, review, scale and real-hardware proof

| # | Item | Size | Evidence |
|---|---|---|---|
| 1 | Render-farm nodes as services, with capability limits and job notifications | M | [PROGRESS L1031, L1814](../../PROGRESS.md); distributed transcodes ([Digital Production](https://digitalproduction.com/2026/09/11/baselight-8-the-feature-list-is-the-easy-part/)) |
| 2 | Shared, self-hosted job database for the farm on the local network | M | [PROGRESS L1031](../../PROGRESS.md) |
| 3 | Assistant plan-and-approve mode: the plan is shown, each step approved, one-click revert | S–M | [TechTimes](https://www.techtimes.com/articles/330572/20261112/adobe-max-2026-recap-firefly-gets-planning-mode-creative-cloud-goes-open-agentic-ai-takes-over.htm); [Descript changelog](https://descript.canny.io/changelog) |
| 4 | Project history: browse, compare and restore saved versions (check against the existing 5-minute snapshots first) | M | Snapshot compare in Resolve 21 ([Newsshooter](https://www.newsshooter.com/2026/06/02/davinci-resolve-21-final-release/)) |
| 5 | Depth for reframed, 360°, stereo and nested clips; depth on the GPU | M | [PROGRESS L1214](../../PROGRESS.md) |
| 6 | Review viewers shown following and brought back; client edits kept while following | S–M | Phase 18 lower-priority list |
| 7 | WHEP playback, Media over QUIC, MXL and ADM-OSC | M | Phase 18 lower-priority list |
| 8 | Montage's own leftovers | S–M | Standing item |
| 9† | Reference video output on Windows, in SD and as field-rendered interlace (same terms as the existing Linux/macOS output) | M | [PROGRESS L1021](../../PROGRESS.md) |
| 10† | Multi-GPU frame processing | L | [Resolve 21.1 feature PDF](https://documents.blackmagicdesign.com/SupportNotes/DaVinci_Resolve_Studio_21_Features.pdf?_v=1790578810000) |
| 11† | Vendor GPU render paths for video plug-ins | M | [PROGRESS L1774](../../PROGRESS.md) |
| 12† | 4K real-time budgets measured on real GPUs, as a regression gate | M | [PROGRESS L1752](../../PROGRESS.md) |
| 13† | Browser review on more browsers and on phones; 10-bit HEVC in browsers | S–M | [PROGRESS L1917, L2259](../../PROGRESS.md) |
| 14† | ST 2110 kernel-bypass sending, narrow pacing, interop with other makers' -22/-41 | L | [PROGRESS L1743, L2159](../../PROGRESS.md) |
| 15† | Interface translations reviewed by native speakers; drafts for new text | S | [README L75](../../README.md); [PROGRESS L2414](../../PROGRESS.md) |

## The simple-mode thread gets Phase 20 and three slots in Phase 19

**The beginner mode is reserved, not designed here.** It is an optional view switched on over the full professional app, which keeps all of its depth: no professional feature is removed, simplified or hidden by default for it, and nothing elsewhere in this roadmap is cut to make room. Today Montage has no templates, tour, sample media or simple mode. Its approachability work targets people switching from other editors ([inventory Q5](../../src/app/MainWindow.cpp)).

**Dependencies land first, in Phase 19:**
- Phase 18 #2 delivers the Preferences window, which will hold the "dismissed forever" record.
- Phase 19 #7 adds project and sequence templates.
- Phase 19 #8 adds the first-run window with CC0 media.
- Phase 19 #9 is a reserved slot for the plumbing that turns a suggestion into a previewable, undoable edit.

**Phase 20 holds the mode itself.** Five slots cover the infrastructure any design will need: an offline runtime, verified model downloads, a CI harness, starter assets and beginner export presets. Ten slots are left for the thread to fill.

**The evidence supports this shape.** Commercial simple modes now open on AI auto-edit ([CapCut](https://www.capcut.com/help/how-to-use-auto-cut)). Agent results land as ordinary editable timeline material ([Eddie AI guide](https://www.heyeddie.ai/learn/premiere-pro/26.5.2/ai-features-in-premiere-pro)). Both fit endlessly regenerable suggestions offered one to three at a time.

**Phase 20 can slide later without blocking anything else.** It sits second only because the ranking method would never surface it.

## Conclusion

**The gap that remains is shaped by policy and by verification, not by missing features.** Six professional phases finish the feature list. The last of them is mostly work that needs real GPUs, phones, capture cards and native speakers. That part will move only if the project gets access to hardware or community testers, not by writing more code in CI.

**A few owner decisions would matter more than any single phase.** The most important are signed installers, a self-hosted shared-project server and perhaps one closed SDK. Reserving Phase 20 explicitly is also the right safeguard: it is the first roadmap item that the project's own ranking method would never have chosen.
