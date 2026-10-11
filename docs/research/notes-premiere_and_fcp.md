# Adobe Premiere (Pro) and Apple Final Cut Pro: 2026 feature inventory (as of 7 Oct 2026)

Scope note: research done 7 Oct 2026. Adobe's helpx.adobe.com and petapixel.com returned HTTP 403 to the fetch tool, so Adobe items come from Adobe community announcements, Adobe/NVIDIA press, and trade press. Apple items come mainly from the official Final Cut Pro release notes (support.apple.com/en-us/102825), Apple Newsroom and trade press. Items I believe to be true from background knowledge but could not verify in this session are listed under "Gaps", marked "unverified", and not presented as cited facts.

Naming: Adobe dropped "Pro" from the product name with version 26.0 (Jan 2026). It is now "Adobe Premiere" on desktop, and there is a separate free "Premiere" app on iPhone. Apple now sells Final Cut Pro both as a one-time App Store purchase and inside the "Apple Creator Studio" subscription (from 28 Jan 2026).

## Q1. Headline features of the latest Premiere (25.x/26.x) and Final Cut Pro (11.x/12.x) releases up to October 2026

### Takeaway
Premiere's 2024–2026 run went: a rewritten wide-gamut colour-management system with an ACEScct working space and the new Properties panel (25.0, Oct 2024); Firefly-powered Generative Extend, on-device Media Intelligence search and caption translation (25.2, Apr 2025); the AI Object Mask plus faster-tracking shape masks and the rebrand to "Adobe Premiere" (26.0, Jan 2026); a dedicated Color Mode grading environment in public beta (26.2, NAB Apr 2026, won Best Software at NAB); and an agentic "Creative Agent" public beta (June 2026). Final Cut Pro went: Enhance Light and Color and Smooth Slo-Mo (10.8, Jun 2024); Magnetic Mask, Transcribe to Captions and spatial video (11.0, Nov 2024); Image Playground and adjustment clips (11.1, Mar 2025); Transcript Search, Visual Search and Beat Detection with the Creator Studio launch (12.0, Jan 2026); Generate Captions, Auto Mask, a rebuilt Match Color and Edit Detection (12.3, Jun 2026); and Cinematic-mode editing from Final Cut Camera (12.4, 29 Sep 2026).

### Cited Findings

**Adobe Premiere: release-by-release (newest first)**

- **Creative Agent (public beta, 18 Jun 2026):** a conversational assistant panel that can organise footage, build bins, generate transcripts, create stringouts and assemble rough cuts. It has two modes ("Always ask" or "Auto approve") and runs on daily complimentary beta credits that reset at midnight GMT. — [Adobe Community: "Adobe since NAB 2026: what editors should know"](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Premiere 26.3 (shipped after NAB; Adobe community post dated 24 Jun 2026):**
  - Global Audio Mute.
  - Single Word Captioning: word-by-word captions, each with its own timing, for the animated caption style.
  - Marker Search: filter markers by name or colour.
  - A/V Display Mode: show the timeline as audio-only, video-only or combined.
  - New GPU effects: Channel Blur, Gradient and Noise.
  - New transitions: 3D Spinback and 3D Slide.
  - Stock Panel Checkout: license Adobe Stock assets inside Premiere.
  - Object Masking refinements.
  - Source: [Adobe Community: Adobe since NAB 2026](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Premiere 26.2 (April 2026, shown at NAB 2026):**
  - Color Mode (Public Beta): Color workspace, Color Controls panel, Clip Grid for shot-to-shot grading, and layered Clip / Group / Sequence operations.
  - Effects and transitions powered by Film Impact: Channel Blur, Gradient, Noise; 3D Spinback and Slide.
  - Object Mask Sharp and Smooth edge modes.
  - Audio waveforms in the Source Monitor.
  - Global Audio Mute.
  - Sequence Index panel: a searchable, spreadsheet-style view of the timeline.
  - Faster media relinking, with automatic reconnection across drives and systems.
  - Marker search by name, comments and colour.
  - Content Credentials on export, including attribution info, linked accounts and AI-usage preferences.
  - Source: [Adobe Community: Welcome to Premiere 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)
  - **Conflict:** the post-NAB summary lists Global Audio Mute and the Channel Blur/Gradient/Noise effects as 26.3 features, while the 26.2 announcement lists them under 26.2. They probably shipped in 26.2. — [Adobe since NAB 2026](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489) vs [Welcome to Premiere 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)
- **Color Mode (beta) details:**
  - Won "Best Software at NAB 2026". Adobe describes it as "a full redesign of the color workflow inside Premiere", "not an update to Lumetri but a dedicated grading environment".
  - 32-bit colour processing and NVIDIA RTX GPU acceleration.
  - Panels: Color Monitor, Clip Grid, Color Controls, Comparison View, Video Scopes.
  - Adjustments, groups and sequence-level operations replace stacked Lumetri effects.
  - Sources: [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489); [NVIDIA blog: New Adobe Premiere Color Grading Mode Accelerated on NVIDIA GPUs](https://blogs.nvidia.com/blog/rtx-ai-garage-nab-adobe-premiere-color-mode/)
- **Premiere 26.0 (released 20 Jan 2026; rebranded "Adobe Premiere"):**
  - Headline: the AI-powered Object Mask. Hover and click to select a person or object, then track it through the shot. It was previewed as a beta at Adobe MAX in October 2025.
  - Redesigned Rectangle, Ellipse and Pen shape masks that track "up to 20× faster", with rounded-corner control and constrained straight lines.
  - Send media from Adobe Firefly Boards to desktop Premiere.
  - Create and adjust transitions directly on clips with handles.
  - GPU-accelerated thumbnail generation.
  - Premiere, After Effects, Audition and Media Encoder run natively on Windows on ARM (ARM64).
  - Sources: [ProVideo Coalition: New AI and masking tools in Premiere](https://www.provideocoalition.com/new-ai-and-masking-tools-in-premiere-plus-major-upgrade-to-after-e%ef%ac%80ects/); [PetaPixel: Rebranded Adobe Premiere 26 arrives with one-click object tracking](https://petapixel.com/2026/01/20/rebranded-adobe-premiere-26-arrives-with-one-click-object-tracking); [Adobe Community: Welcome to Adobe Premiere 26.0](https://community.adobe.com/announcements-727/welcome-to-adobe-premiere-26-0-1552341)
  - 26.0 also brought:
    - Connectivity with Firefly (including Boards), Frame.io V4 and Adobe Stock (52 million assets).
    - "Interactive audio fades, effortless file relinking, improved organization and naming schemes, faster performance, and new RAW support".
    - Source: [Newsshooter: What's new in Adobe Premiere Pro 26.0](https://www.newsshooter.com/2026/01/20/whats-new-in-adobe-premiere-pro-26-0/)
- **Premiere on iPhone (launched 30 Sep 2025, free, no ads):**
  - Unlimited multitrack timeline and 4K HDR editing.
  - Animated captions, speed ramps and background removal.
  - Generative stickers, background expansion and image-to-video.
  - Enhance Speech and Generative Sound Effects.
  - One-tap export to TikTok, YouTube Shorts and Instagram, with auto-resizing.
  - Paid upgrades add AI credits and cloud storage.
  - Sources: [AppleInsider](https://appleinsider.com/articles/25/09/30/adobe-premiere-launches-on-iphone-free-to-download-now); [TheWrap](https://www.thewrap.com/adobe-premiere-iphone-creators/)
- **Premiere Pro 25.2 (2 Apr 2025, NAB 2025):**
  - Generative Extend went GA. It is powered by the Firefly Video Model, extends video in 4K in landscape or vertical, and can also extend audio clips.
  - Media Intelligence and a new Search panel went GA: natural-language search, filterable by content, dialogue or metadata.
  - Caption Translation into 27 languages, working on Premiere-generated captions or imported .srt sidecars.
  - "Drag-and-drop color management".
  - Sources: [NewscastStudio](https://www.newscaststudio.com/2025/04/03/adobe-brings-generative-extend-media-intelligence-to-premiere-pro); [Tom's Guide](https://www.tomsguide.com/ai/premiere-pro-can-now-use-ai-to-extend-video-and-transcribe-into-27-languages); [Adobe helpx feature summary (April 2025)](https://helpx.adobe.com/au/premiere-pro/using/whats-new/2025-2.html) (title and snippet only, page returned 403)
- **Premiere Pro 25.0 (Adobe's helpx page titles it the "October 2024 release"; announced in beta mid-2024):**
  - Rewritten colour management that turns raw and log footage from "nearly every camera" into consistent images on import, without LUTs.
  - A new wide-gamut working colour space based on ACEScct, with tone mapping.
  - Six "set-it-and-forget-it" presets in Sequence Settings and Lumetri. Colour-management settings pass to After Effects over Dynamic Link.
  - New context-aware Properties panel. For the first time you can edit properties of multiple clips at once.
  - Sources: [CineD](https://www.cined.com/adobe-premiere-pro-and-after-effects-25-in-beta-new-color-management-improved-properties-panel-enhanced-3d-workflows-and-more/); [RedShark News](https://www.redsharknews.com/adobe-premiere-pro-25-adds-new-color-management-properties-panel-more); [Adobe helpx: feature summary (October 2024 release)](https://helpx.adobe.com/premiere-pro/using/whats-new/2025.html) (title only)
- **Audio overhaul (announced in beta 16 Jan 2024):**
  - Interactive fade handles on timeline clips.
  - AI Audio Category Tagging: auto-labels clips as dialogue, music, SFX or ambience, giving one-click access to the matching tools.
  - Redesigned effect badges.
  - Enhance Speech, first in Premiere beta in September 2023.
  - Sources: [Adobe Newsroom media alert](https://news.adobe.com/news/news-details/2024/media-alert-adobe-premiere-pro-innovations-make-audio-editing-faster-easier-and-more-intuitive); [British Cinematographer](https://britishcinematographer.co.uk/adobe-announces-new-audio-features-for-premiere-pro-ahead-of-sundance/)

**Apple Final Cut Pro (Mac): release-by-release (newest first; all from [Apple FCP release notes](https://support.apple.com/en-us/102825) unless noted)**

- **12.4 (29 Sep 2026):**
  - Edit video with Cinematic mode effects recorded in Final Cut Camera.
  - Faster multicam angle switching.
  - Improved audio waveform generation.
  - Fixes for title fonts, iPad project import, Animation Editor keyframe pasting and network library backup.
- **12.3 (30 Jun 2026):**
  - Generate Captions: on-device AI that transcribes audio and places subtitles on the timeline, with animated styles and control of font, colour and position. US English only. Needs an Apple silicon Mac on macOS 15.6+, or an M1/A16/A17 Pro iPad on iPadOS 26+.
  - Auto Mask: on-device AI that isolates skin, hair, sky, foliage or clothing without manual tracking. Works alongside Magnetic Mask. Mac only.
  - Match Color rebuilt to match against an editor-chosen reference frame.
  - Edit Detection: on-device analysis of rendered video that splits it back into its original clips.
  - Persistent two-up trim display, direct frame export to Pixelmator Pro, and a swap-clips shortcut.
  - Copy and paste complex edits across timelines.
  - HEVC proxies replace H.264 proxies.
  - Background rendering now off by default.
  - Better subtitle management: convert captions, and select or adjust multiple captions at once.
  - Sources: [Apple release notes](https://support.apple.com/en-us/102825); [Camera Jabber](https://camerajabber.com/photography-news/final-cut-pro-adds-ai-generated-captions-and-edit-detection-in-major-update/); [Larry Jordan: New Features in FCP 12.3](https://larryjordan.com/articles/new-features-in-apple-final-cut-pro-12-3/)
- **12.2 (9 Apr 2026):** faster startup when many Audio Units are installed; Magnetic Timeline tutorial; Beat Detection zoom fix.
- **12.0 (28 Jan 2026, launched with Apple Creator Studio):**
  - Transcript Search: exact-match or natural-language search of spoken words.
  - Visual Search: natural-language search for objects and actions.
  - Beat Detection: shows bars and beats in the timeline.
  - In-app guides and a new toolbar creation menu.
  - FCPXML updated to 1.14.
  - Sources: [Apple release notes](https://support.apple.com/en-us/102825); [Newsshooter FCP 12.0](https://www.newsshooter.com/2026/01/28/final-cut-pro-12-0/); [No Film School](https://nofilmschool.com/final-cut-pro-update-12-0)
- **11.2 (19 Sep 2025):**
  - Expanded ProRes RAW controls for iPhone footage.
  - Apple Log 2 LUT support.
  - FireWire support removed on macOS Tahoe.
- **11.1.1 (22 May 2025):** fixes for MXF playback and export segmentation.
- **11.1 (27 Mar 2025):**
  - Image Playground: create stylised images from a description.
  - Magnetic Mask performance improvements.
  - Adjustment clips.
  - Quantec QRS (Room Simulator) reverb.
  - Rename audio effects, reveal multicam/synced clip sources, and drag markers.
- **11.0 (13 Nov 2024):**
  - Magnetic Mask: an "AI-powered" tool to isolate people and objects without a green screen.
  - Transcribe to Captions.
  - Spatial video import and editing (Vision Pro / iPhone 15 Pro).
  - 90, 100 and 120 fps timelines.
  - Original clips auto-hidden after creating synced or multicam clips.
  - Picture in Picture and Callout effects; modular transitions.
  - Third-party **Media Extensions** (macOS Sequoia+).
- **10.8 (20 Jun 2024):**
  - Enhance Light and Color: a machine-learning auto correction.
  - Smooth Slo-Mo: AI frame interpolation on Apple silicon.
  - Rename colour corrections and effects; drag effects between clips.
  - Timeline Index filters and search by reel, scene, camera, custom metadata and effect name.
  - HDR tone-mapping for 360° projects.

**Final Cut Pro for iPad and companion apps**

- **FCP for iPad 3.0 (28 Jan 2026):**
  - Beat Detection, Transcript Search and Visual Search.
  - **Montage Maker (iPad only):** picks visual highlights and cuts them to the rhythm of a chosen song, with Auto Crop for vertical.
  - External monitor playback.
  - Sources: [Apple FCP for iPad "What's new"](https://support.apple.com/en-asia/guide/final-cut-pro-ipad/whats-new-dev1fcc8d09e/ipados); [Apple Newsroom: Introducing Apple Creator Studio](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)
- **FCP for iPad 2.0 (announced May 2024):** Live Multicam wirelessly records up to four angles from iPhones/iPads running the new **Final Cut Camera** app. Each camera can be controlled remotely (exposure, focus, zoom), and proxies are replaced with full-resolution files in the background. Final Cut Camera offers manual WB/ISO/shutter/focus, zebras, focus peaking and audio meters. — [Apple Newsroom May 2024](https://www.apple.com/newsroom/2024/05/final-cut-pro-transforms-video-creation-with-live-multicam-on-ipad-and-new-ai-features-on-mac/); [TV Technology](https://tvtechnology.com/news/apple-unveils-a-ipad-multicam-production-studio-solution)
- **Apple Creator Studio:**
  - $12.99/month or $129/year (students and educators $2.99/month or $29.99/year).
  - Includes Final Cut Pro, Logic Pro and Pixelmator Pro on Mac and iPad, plus Motion, Compressor and MainStage on Mac.
  - FCP is still sold as a one-time App Store purchase, but some premium content requires the subscription.
  - Sources: [Apple Newsroom (via Barchart)](https://www.barchart.com/story/news/37018794/apple-introduces-apple-creator-studio-an-inspiring-collection-of-the-most-powerful-creative-apps); [Newsshooter](https://www.newsshooter.com/2026/01/28/final-cut-pro-12-0/); [fcp.cafe](https://fcp.cafe/news/20260114/)
  - With 12.0 the suite moved to **Motion 6 and Compressor 5**. — [TidBITS](https://tidbits.com/watchlist/final-cut-pro-x-12-compressor-5-and-motion-6/)
  - Motion gained Magnetic Mask. — [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)
- **Logic Pro 12 (Logic-adjacent audio, Jan 2026):**
  - Synth Player: an AI electronic "session player" for chords and synth bass.
  - Chord ID: turns audio or MIDI into chord progressions.
  - Music Understanding (iPad): natural-language search in the Sound Browser.
  - Quick Swipe Comping on iPad.
  - New Sound Library.
  - Requires Apple silicon on Mac.
  - Source: [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)

### Inferences
- Both vendors have moved to a fast cadence: Premiere ships roughly every 2 months (26.0 in Jan, 26.2 in Apr, 26.3 in Jun), and Apple shipped four FCP point releases in 2026 (12.0, 12.2, 12.3, 12.4). Since 2025 both have put most of their headline feature work into AI.
- Both now aim at editors and short-form creators at once:
  - Premiere's iPhone app, Firefly Boards and Single Word Captioning target social/creator workflows.
  - Apple's Montage Maker, Beat Detection, animated Generate Captions and Creator Studio bundling do the same.
- Premiere's Color Mode is aimed at DaVinci Resolve's Color page (Clip Grid ≈ thumbnail timeline; Clip/Group/Sequence ≈ Resolve's clip/group/timeline grades). FCP's colour updates (Auto Mask, rebuilt Match Color, Enhance Light and Color) are assistive rather than a new grading environment.

### Gaps
- No usage data from either vendor (MAU, share of pro users) and no independent survey of how heavily professionals use each feature. The closest signals are Color Mode's NAB award (reported by Adobe itself) and Larry Jordan calling FCP 12.3 "very significant" ([Larry Jordan](https://larryjordan.com/articles/new-features-in-apple-final-cut-pro-12-3/)).
- Could not read Adobe's official helpx release notes (403), so exact dates for 25.3–25.6 and 26.1, and full 26.x changelogs, are unverified.
- No information found on whether Adobe MAX 2026 has happened or what it brought. MAX is usually mid/late October, so it is probably still upcoming as of 7 Oct 2026.
- Premiere desktop pricing was not verified in this session.

## Q2. AI features in each, and which run locally vs in the cloud

### Takeaway
Apple's AI editing features are all on-device on Apple silicon (Magnetic Mask, Transcript/Visual Search, Generate Captions, Auto Mask, Edit Detection, Enhance Light and Color, Smooth Slo-Mo, Beat Detection, Montage Maker). Most are US-English-only and some are Mac-only. Premiere is hybrid:
- **Local:** analysis and assist features (Media Intelligence search, transcription via a new Speechmatics on-device model, Object Mask, Remix, per community experts).
- **Cloud:** generative features (Generative Extend, Firefly-based generation, the Creative Agent), which run on Adobe's servers and consume generative credits.

### Cited Findings

**Premiere AI features**

- **Generative Extend (beta Oct 2024; GA in 25.2, Apr 2025):**
  - Firefly Video Model. Adds frames to the head or tail of video (4K, landscape or vertical) and extends audio clips. Marketed as "safe for commercial use".
  - Free for a limited time at launch, then requires a Firefly subscription and generative credits. Firefly plans start at $10/month for 2,000 credits.
  - Runs in the cloud.
  - Sources: [Tom's Guide](https://www.tomsguide.com/ai/premiere-pro-can-now-use-ai-to-extend-video-and-transcribe-into-27-languages); [NewscastStudio](https://www.newscaststudio.com/2025/04/03/adobe-brings-generative-extend-media-intelligence-to-premiere-pro); cloud processing per a community expert in [Adobe Community: when does Premiere process AI on device or in the cloud](https://community.adobe.com/questions-729/when-does-premiere-process-it-s-ai-tasks-on-device-or-in-the-cloud-1553097) (not Adobe staff)
- **Media Intelligence (GA in 25.2):**
  - An on-device model builds a semantic index of objects, locations, camera angles, spoken dialogue and metadata.
  - "All analysis happens locally", with no internet connection needed.
  - Source: [ProVideo Coalition: Unlocking efficiency with Media Intelligence](https://www.provideocoalition.com/unlocking-efficiency-with-media-intelligence-in-adobe-premiere-pro/)
- **Speech to Text / transcripts (foundation of text-based editing and auto captions):**
  - Adobe and Speechmatics shipped a new on-device STT model for Premiere. It is "within 5% relative to cloud accuracy" (measured over ~10M words) and processes 1 hour of audio in about 55 seconds, on Windows and Mac.
  - Sources: [Podnews press release](https://podnews.net/press-release/adobe-speechmatics-on-device); [TV Technology](https://tvtechnology.com/production/adobe-and-speechmatics-deliver-cloud-grade-on-device-speech-recognition-for-premiere)
  - The release date of this model was not captured.
- **Caption Translation (25.2):** translates into 27 languages. — [NewscastStudio](https://www.newscaststudio.com/2025/04/03/adobe-brings-generative-extend-media-intelligence-to-premiere-pro)
- **Single Word Captioning (26.3).** — [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Object Mask (beta at MAX Oct 2025; shipped in 26.0; Sharp/Smooth edges in 26.2):** AI identifies people and objects with a single click and tracks them. — [ProVideo Coalition](https://www.provideocoalition.com/new-ai-and-masking-tools-in-premiere-plus-major-upgrade-to-after-e%ef%ac%80ects/); [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)
- **Where Premiere AI runs (community answers, not Adobe staff):**
  - Community Legend R Neil Haugen: "remix, the bin searches, transcription and masking are all local computer operations".
  - Community Expert Ann Bens: generative extend is "done in the cloud".
  - Source: [Adobe Community thread](https://community.adobe.com/questions-729/when-does-premiere-process-it-s-ai-tasks-on-device-or-in-the-cloud-1553097)
- **Enhance Speech and Audio Category Tagging (AI; beta Sept 2023 / Jan 2024).** — [Adobe Newsroom](https://news.adobe.com/news/news-details/2024/media-alert-adobe-premiere-pro-innovations-make-audio-editing-faster-easier-and-more-intuitive)
- **Creative Agent (public beta, Jun 2026):**
  - An agentic assistant that can organise bins, transcribe, build stringouts and assemble rough cuts.
  - Metered by daily credits, which implies cloud processing.
  - Source: [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Color Mode is GPU-dependent:** NVIDIA RTX acceleration and 32-bit processing. — [NVIDIA blog](https://blogs.nvidia.com/blog/rtx-ai-garage-nab-adobe-premiere-color-mode/)
- **Premiere on iPhone generative features:** generative stickers, background expansion, image-to-video and Generative Sound Effects, plus Enhance Speech. Paid tiers add AI credits. — [AppleInsider](https://appleinsider.com/articles/25/09/30/adobe-premiere-launches-on-iphone-free-to-download-now); [TheWrap](https://www.thewrap.com/adobe-premiere-iphone-creators/)
- **Content Credentials export (26.2):** records AI-usage preferences, relevant for disclosing generative content. — [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)

**Final Cut Pro AI features (all on-device unless noted)**

- **Magnetic Mask** (FCP 11.0; improved in 11.1; added to Motion in 2026). — [Apple release notes](https://support.apple.com/en-us/102825); [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)
- **Transcribe to Captions** (11.0), superseded in practice by **Generate Captions** (12.3):
  - Generate Captions runs on-device, is US English only, and needs Apple silicon / macOS 15.6+.
  - Sources: [Apple release notes](https://support.apple.com/en-us/102825); [Camera Jabber](https://camerajabber.com/photography-news/final-cut-pro-adds-ai-generated-captions-and-edit-detection-in-major-update/)
- **Transcript Search and Visual Search** (12.0 / iPad 3.0):
  - Need an Apple silicon Mac on macOS 15.6+, or an M1+/A16/A17 Pro iPad on iPadOS 26+.
  - US English only.
  - Source: [fcp.cafe / Newsshooter coverage](https://www.newsshooter.com/2026/01/28/final-cut-pro-12-0/)
- **Beat Detection** (12.0) and **Montage Maker** (iPad 3.0 only, with Auto Crop to vertical). — [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)
- **Auto Mask** (12.3, on-device, Mac only), **Edit Detection** (12.3, on-device) and **Match Color** (rebuilt in 12.3). — [Camera Jabber](https://camerajabber.com/photography-news/final-cut-pro-adds-ai-generated-captions-and-edit-detection-in-major-update/)
- **Enhance Light and Color** (10.8, ML) and **Smooth Slo-Mo** (10.8, Apple silicon). — [Apple release notes](https://support.apple.com/en-us/102825)
- **Image Playground** (11.1, Apple Intelligence image generation). — [Apple release notes](https://support.apple.com/en-us/102825)
- Apple's Creator Studio announcement says AI features "run on-device where applicable" on Apple silicon, and that "some features utilize OpenAI's generative models with potential usage limits". The summary did not say which apps those are; they are likely the iWork/Freeform image features rather than FCP. — [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)

### Inferences
- **Apple's design constraint is privacy plus on-device processing.** Its AI features avoid per-use costs and credits, but are tied to Apple silicon, recent OS versions and (for language features) US English.
- **Adobe meters its generative features with credits.** Its analysis features have moved on-device: Media Intelligence, and transcription via Speechmatics.
- **Neither NLE has cloud-generative B-roll generation inside the timeline as a mainstream pro feature.** Adobe's Generative Extend is the nearest. Apple has no generative-video equivalent; it has nothing like Generative Extend.
- **The AI features with the most likely day-to-day pro use** are transcription, captions and text search, masking/tracking, Enhance Speech and audio cleanup. Generative Extend is used more for fixes than routinely. This is an inference; there is no usage data.

### Gaps
- Adobe has no official, consolidated on-device vs cloud table that I could access. The local claims for Remix and Object Mask come from community volunteers.
- **Unverified (background knowledge, not confirmed in this session):**
  - Premiere's Auto Reframe (introduced in Premiere 14.0, late 2019, Adobe Sensei, local).
  - Morph Cut (introduced 2015, local).
  - Scene Edit Detection (around 2020–21, local).
  - Remix (music retiming, around 2022, local).
  - Auto Color in Lumetri (Sensei, local).
  - Enhance Speech in Premiere: believed to have become GA in the 24.x cycle (around mid-2024). Its processing location (local vs cloud) was not confirmed.
  - Text-Based Editing: believed GA in Premiere 23.4/23.5 (2023).
- **FCP's older ML features (unverified):** Smart Conform (FCP 10.4.1, 2018/19, auto-reframe for social aspect ratios), the object tracker (10.6, 2021), Cinematic mode editing (10.6, 2021), Voice Isolation (10.6.x, 2022). Release numbers were not confirmed this session.
- Whether Image Playground in FCP runs on-device or via Private Cloud Compute was not confirmed.

## Q3. Colour management, multicam, proxies/hardware decoding, titles/graphics and audio (including plugin hosting)

### Takeaway
- **Colour:** Premiere has an ACEScct-based wide-gamut colour-management system (25.0), Lumetri, and now a dedicated 32-bit, GPU-accelerated Color Mode (beta, 26.2) with clip/group/sequence layers and a Clip Grid. FCP offers HDR/wide-gamut libraries, colour wheels, curves and boards, LUTs including Apple Log 2 (11.2), ProRes RAW controls, and AI assists (Enhance Light and Color, rebuilt Match Color, Auto Mask).
- **Audio:** Premiere hosts VST, VST3 and AU (macOS) plugins. FCP hosts only Audio Units.
- **Multicam:** FCP added Live Multicam on iPad and faster angle switching in 12.4.
- **Proxies:** FCP moved its proxies to HEVC in 12.3.

### Cited Findings

**Colour**

- **Premiere 25.0 colour management:**
  - Rewritten; log and raw footage auto-transformed on import without LUTs.
  - Wide-gamut working space based on ACEScct with tone mapping.
  - Six presets (Rec.709 legacy through wide-gamut ACEScct).
  - Colour settings shared with After Effects via Dynamic Link.
  - Sources: [CineD](https://www.cined.com/adobe-premiere-pro-and-after-effects-25-in-beta-new-color-management-improved-properties-panel-enhanced-3d-workflows-and-more/); [RedShark News](https://www.redsharknews.com/adobe-premiere-pro-25-adds-new-color-management-properties-panel-more)
- **Premiere 25.2:** "drag-and-drop color management". — [Tom's Guide](https://www.tomsguide.com/ai/premiere-pro-can-now-use-ai-to-extend-video-and-transcribe-into-27-languages)
- **Premiere Color Mode (beta, 26.2):**
  - 32-bit processing, NVIDIA RTX acceleration.
  - Color Monitor, Clip Grid, Color Controls, Comparison View, Video Scopes.
  - Clip / Group / Sequence operations.
  - Sources: [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489); [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825); [NVIDIA](https://blogs.nvidia.com/blog/rtx-ai-garage-nab-adobe-premiere-color-mode/)
- **Premiere masking:** AI Object Mask, plus shape masks that track up to 20× faster (26.0). — [ProVideo Coalition](https://www.provideocoalition.com/new-ai-and-masking-tools-in-premiere-plus-major-upgrade-to-after-e%ef%ac%80ects/)
- **FCP colour:**
  - Apple Log 2 LUT support and expanded ProRes RAW controls for iPhone footage (11.2).
  - Enhance Light and Color (10.8).
  - HDR tone-mapping for 360° (10.8).
  - Auto Mask for isolating skin, hair, sky and similar for corrections, plus rebuilt Match Color (12.3).
  - Fix for colour tinting in log-encoded proxies (12.3).
  - Sources: [Apple release notes](https://support.apple.com/en-us/102825); [Larry Jordan](https://larryjordan.com/articles/new-features-in-apple-final-cut-pro-12-3/)

**Multicam**

- **FCP Mac:**
  - Auto-hide originals after creating multicam/synced clips (11.0).
  - Reveal multicam/synced sources (11.1).
  - Better angle-switching responsiveness (12.4).
  - Source: [Apple release notes](https://support.apple.com/en-us/102825)
- **FCP for iPad:** Live Multicam with up to 4 Final Cut Camera devices (2.0, 2024). — [Apple Newsroom](https://www.apple.com/newsroom/2024/05/final-cut-pro-transforms-video-creation-with-live-multicam-on-ipad-and-new-ai-features-on-mac/)

**Proxies, decoding and performance**

- **FCP:**
  - HEVC proxy generation replaces H.264, with better compression and HDR support (12.3).
  - Background rendering defaults to Off (12.3).
  - Third-party Media Extensions for codecs/formats on macOS Sequoia+ (11.0).
  - FireWire removed on macOS Tahoe (11.2).
  - Sources: [Apple release notes](https://support.apple.com/en-us/102825); [Larry Jordan](https://larryjordan.com/articles/new-features-in-apple-final-cut-pro-12-3/)
- **Premiere:**
  - GPU-accelerated thumbnails, native Windows on ARM (26.0).
  - "New RAW support" (26.0).
  - Faster relinking (26.0 and 26.2).
  - Sources: [ProVideo Coalition](https://www.provideocoalition.com/new-ai-and-masking-tools-in-premiere-plus-major-upgrade-to-after-e%ef%ac%80ects/); [Newsshooter](https://www.newsshooter.com/2026/01/20/whats-new-in-adobe-premiere-pro-26-0/); [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)
- **Frame rates:** FCP added 90/100/120 fps timelines (11.0). — [Apple release notes](https://support.apple.com/en-us/102825)

**Titles, graphics and effects**

- **Premiere:**
  - Properties panel, with multi-clip editing, covers video, audio, text and graphics (25.0). — [RedShark News](https://www.redsharknews.com/adobe-premiere-pro-25-adds-new-color-management-properties-panel-more)
  - Film Impact-powered GPU effects (Channel Blur, Gradient, Noise) and 3D Spinback/Slide transitions (26.2/26.3).
  - Stock Panel Checkout (26.3).
  - Sources: [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825); [Adobe since NAB](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
  - Animated, word-by-word caption style via Single Word Captioning (26.3). — [Adobe since NAB](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
  - On-clip transition handles (26.0). — [ProVideo Coalition](https://www.provideocoalition.com/new-ai-and-masking-tools-in-premiere-plus-major-upgrade-to-after-e%ef%ac%80ects/)
- **FCP:**
  - Picture in Picture and Callout effects, modular transitions (11.0).
  - Adjustment clips (11.1).
  - Generate Captions with animated styles (12.3).
  - Direct frame export to Pixelmator Pro (12.3).
  - Magnetic Mask now also in Motion (2026).
  - Sources: [Apple release notes](https://support.apple.com/en-us/102825); [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)

**Audio**

- **Premiere:**
  - Interactive fade handles, AI Audio Category Tagging, Enhance Speech and redesigned badges (2024 audio overhaul). — [Adobe Newsroom](https://news.adobe.com/news/news-details/2024/media-alert-adobe-premiere-pro-innovations-make-audio-editing-faster-easier-and-more-intuitive)
  - Source Monitor waveforms and Global Audio Mute (26.2). — [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)
- **Premiere plugin hosting:** third-party audio effects in **VST, VST3 and Audio Units** (AU on macOS only). — [Adobe helpx: Third-party plug-ins](https://helpx.adobe.com/il_en/premiere/desktop/use-premiere-pro-with-other-apps/plug-ins.html) (from search snippet; page not fetched)
- **FCP audio:**
  - Quantec QRS reverb and renamable audio effects (11.1).
  - Faster startup with many Audio Units (12.2).
  - Improved waveform generation (12.4).
  - Source: [Apple release notes](https://support.apple.com/en-us/102825)
- **FCP plugin hosting:** **Audio Units only**, no native VST. — [Sound On Sound](https://www.soundonsound.com/techniques/final-cut-pro-vst-audio-units)
  - This source is old (it recommends the long-discontinued FXpansion VST-AU adapter). The AU-only stance is consistent with FCP 12.2's release note about "multiple Audio Units". — [Apple release notes](https://support.apple.com/en-us/102825)
- **Logic-adjacent:** Logic Pro 12 adds Synth Player and Chord ID. — [Apple Newsroom](https://www.apple.com/ph/newsroom/2026/01/introducing-apple-creator-studio-an-inspiring-collection-of-creative-apps)

### Inferences
- **On colour, Premiere is now the more ambitious of the two.** It has explicit ACES (ACEScct) working-space management and a Resolve-style grading mode. FCP's colour model is less explicit (no ACES/OCIO) and leans on automatic ML tools.
- **Plugin hosting is a clear split.** Premiere supports both VST3 and AU, so it is cross-platform. FCP is AU-only, macOS/iPadOS only.
- **For a newcomer NLE:**
  - VST3, plus AU on Mac, is the baseline for audio plugin hosting.
  - HEVC/ProRes proxies and hardware decoding of the main delivery codecs are table stakes.

### Gaps
- No source found for whether Premiere's new colour management (or Color Mode) supports OpenColorIO configs. ACEScct is confirmed; OCIO is not. FCP has no published ACES/OCIO support; that absence is unverified.
- **Unverified Essential Graphics / MOGRT details:**
  - Premiere's Essential Graphics panel and MOGRT templates from After Effects (since Premiere 2017).
  - Premiere's Essential Sound panel (since 2017).
  - FCP's Motion-built templates for titles, generators, effects and transitions published to FCP.
  - These are long-standing but no 2026 sources were captured.
- **Unverified hardware-decoding specifics:** Premiere's hardware decode of H.264/HEVC via Intel QuickSync, NVIDIA NVDEC and Apple VideoToolbox; ProRes RAW and BRAW support status. FCP's ProRes RAW and Apple silicon media engine.
- **FCP roles and the magnetic timeline:** core long-standing FCP concepts (roles for audio stems/subroles; magnetic timeline since FCP X 2011). Only the 12.2 Magnetic Timeline tutorial and 12.0 role-colour fixes are confirmed in this session's sources. — [Apple release notes](https://support.apple.com/en-us/102825)

## Q4. Interchange formats imported/exported (XML, FCPXML, AAF, OTIO, EDL)

### Takeaway
- **FCP:** interchange is built around FCPXML, now version 1.14 (FCP 12.0).
- **Premiere OTIO:** import/export has been in Premiere *beta* for over two years and, as of the latest community answers, has not shipped in the release build.
- **Everything else:** no 2026 primary sources were captured on Premiere's long-standing XML/AAF/EDL support or on FCP's reliance on third-party converters for AAF/EDL. Treat those as unverified.

### Cited Findings
- **FCP 12.0** updated FCPXML to version **1.14**. — [Apple release notes](https://support.apple.com/en-us/102825); [Newsshooter](https://www.newsshooter.com/2026/01/28/final-cut-pro-12-0/)
- **Premiere beta OTIO import/export** carries:
  - names, cutlist with clips, source in/out, durations, starting timecode, fps;
  - multiple tracks, track names, linear speed and sequence markers.
  - It is accessed via File > Export > OpenTimelineIO and File > Import.
  - Source: [Adobe Community: Now in Beta: OTIO import and export](https://community.adobe.com/t5/premiere-pro-beta-discussions/now-in-beta-otio-import-and-export/m-p/14985932)
- **OTIO is still beta only.** A community manager says: "This feature is still in beta. To try it out, please download the latest Beta version". A user complains it has been in beta "over 2 years". — [Adobe Community: OTIO export option missing](https://community.adobe.com/questions-729/opentimelineio-otio-export-option-missing-in-premiere-pro-s-export-menu-1418570)
- **Premiere 26.2 Content Credentials export:** C2PA-style provenance metadata on exported media. — [Adobe Community 26.2](https://community.adobe.com/announcements-727/welcome-to-premiere-26-2-1557825)
- **FCP iPad-to-Mac project import** exists (12.4 fixed crashes when importing iPad projects). — [Apple release notes](https://support.apple.com/en-us/102825)
- **FCP direct frame export to Pixelmator Pro** (12.3). — [Apple release notes](https://support.apple.com/en-us/102825)

### Inferences
- OTIO is still not a first-class shipping format in either major NLE. That leaves FCPXML (Apple), FCP7 XML / AAF / EDL (Premiere, Avid and Resolve) as the practical interchange set.
- A new NLE that supports OTIO plus FCPXML import would have an interchange advantage in some cases.

### Gaps
- **Unverified (background knowledge):**
  - Premiere imports/exports Final Cut Pro 7 XML, AAF (for Pro Tools/Avid), EDL (CMX3600) and OMF (legacy). It cannot natively open FCPXML; XtoCC was used historically.
  - FCP has no native AAF, EDL or OMF export. It relies on third-party tools: X2Pro Audio Convert (AAF), XtoCC and EDL-X (EDL), Producer's Best Friend (reports).
  - FCP has no native OTIO support.
  - None of these could be confirmed with 2026 sources in the tool budget.
- **Caption formats:** Premiere supports SRT, and FCP iTT/SRT/CEA-608. Only Premiere's SRT import for translation was confirmed ([NewscastStudio](https://www.newscaststudio.com/2025/04/03/adobe-brings-generative-extend-media-intelligence-to-premiere-pro)).

## Q5. Collaboration and cloud features (Frame.io integration, Team Projects, shared libraries)

### Takeaway
Premiere has deep, and deepening, cloud integration:
- Frame.io V4 built in, with zero-click sign-in and Firefly assets visible in Frame.io projects.
- Firefly Boards hand-off.
- Adobe Stock licensing inside the app.
- A credit-metered Creative Agent.

FCP is still local-first. It has no native multi-user cloud collaboration in the sources found. Libraries live on local or network storage (12.4 fixes network library backup), and collaboration comes from iPad↔Mac project transfer and third-party integrations.

### Cited Findings
- **Premiere 26.0:** "Seamless workflows across Adobe Firefly (including Firefly Boards), Frame.io V4, and integrated access to Adobe Stock's 52 million assets". — [Newsshooter](https://www.newsshooter.com/2026/01/20/whats-new-in-adobe-premiere-pro-26-0/)
- **Firefly Boards:** media can be sent to desktop Premiere. — [ProVideo Coalition](https://www.provideocoalition.com/new-ai-and-masking-tools-in-premiere-plus-major-upgrade-to-after-e%ef%ac%80ects/)
- **Frame.io after NAB 2026:** zero-click sign-in from Premiere, Firefly assets accessible in Frame.io projects, and Japanese localisation. — [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Stock Panel Checkout (26.3):** license Adobe Stock in-app. — [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Creative Agent (beta, Jun 2026):** cloud credits. — [Adobe Community](https://community.adobe.com/announcements-727/adobe-since-nab-2026-what-editors-should-know-1629489)
- **Premiere on iPhone:** paid tiers include cloud storage. — [AppleInsider](https://appleinsider.com/articles/25/09/30/adobe-premiere-launches-on-iphone-free-to-download-now)
- **FCP:**
  - 12.4 fixed the ability to back up network-hosted libraries.
  - 12.4 fixed crashes importing iPad projects.
  - Source: [Apple release notes](https://support.apple.com/en-us/102825)
- **FCP for iPad Live Multicam:** local wireless multi-device capture (up to 4 cameras), not cloud. — [Apple Newsroom](https://www.apple.com/newsroom/2024/05/final-cut-pro-transforms-video-creation-with-live-multicam-on-ipad-and-new-ai-features-on-mac/)
- **Apple Creator Studio** is a subscription bundle for the apps and premium content. The sources do not describe it as a collaboration or cloud project service. — [Newsshooter](https://www.newsshooter.com/2026/01/28/final-cut-pro-12-0/)

### Inferences
- **Collaboration is Adobe's structural edge:** Frame.io review/approval and camera-to-cloud, Creative Cloud libraries and Team Projects. Apple has not answered it natively.
- **A new NLE can stand out in two ways:**
  - Matching FCP's privacy-first local AI.
  - Offering open, self-hostable review links, rather than trying to match Adobe's cloud stack.

### Gaps
- **Unverified (background knowledge):**
  - Premiere Team Projects and Productions (shared multi-editor projects; Team Projects hosted via Creative Cloud).
  - Creative Cloud Libraries for shared graphics, LUTs and MOGRTs.
  - Frame.io panel in Premiere since 2022 (v22.x), with comments mapped to timeline markers.
  - Frame.io Camera-to-Cloud.
  - Frame.io's FCP workflow extension and FCPXML-based round trips.
  - No 2026 source confirmed whether any of these changed.
- No evidence found of Apple adding iCloud-based shared FCP libraries or real-time collaboration through October 2026.
