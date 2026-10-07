# NLE User Priorities: What Professional and Prosumer Editors Rely On, Complain About, and Switch Over (2024–2026)

Research date: 7 October 2026. Evidence labels used throughout:
- **[QUANT]** = quantitative data (survey, vendor-reported count, or market figure)
- **[VENDOR]** = vendor-reported claim (marketing; directionally useful, not independent)
- **[ANECDOTE]** = individual editor or commenter opinion
- **[OLDER]** = data from before 2024
- **[SNIPPET]** = taken from a search-engine summary because the full page blocked fetching (403) or could not be checked. Treat as lower confidence.

Access note: reddit.com (r/editors, r/VideoEditing, r/davinciresolve, r/premiere, r/kdenlive) could not be reached at all. The search tool and the fetch tool both refuse that domain. Community sentiment therefore comes from Hacker News, trade press (ProVideo Coalition, Digital Production, RedShark, PetaPixel), project forums (KDE Discuss, Shotcut forum) and user-review sites. This is a real gap; see the Gaps sections.

---

## 1. NLE market share and usage 2024–2026, and why editors switch

### Takeaway
No independent, methodologically sound NLE market-share survey for 2024–2026 turned up. The usable signals are segment-specific. Avid Media Composer still dominates top-end film editing (4 of 5 Best Film Editing nominees at the 2026 Oscars). Premiere Pro dominates independent and festival film (about 60% of Sundance 2025 films, Adobe-reported). DaVinci Resolve is the main beneficiary of switching. Creators move to it for cost (free tier, or a one-time $299 Studio licence against a Creative Cloud subscription), for stability, and for all-in-one colour and audio. CapCut has the largest casual and short-form base (about 300M+ MAU, reported by aggregators). Switching away from Premiere is driven mainly by crashes and bugs, subscription cost, and a feeling that Adobe ships showy AI while core problems go unfixed.

### Cited Findings
**Headline "market share" figures (low reliability)**
- [QUANT, low reliability] Aggregator statistics pages repeat "Premiere Pro 35%, Final Cut Pro 25%, DaVinci Resolve 15%" for professional video editing. No methodology, sample or primary source is given. Do not use these as hard numbers. — [ElectroIQ](https://electroiq.com/stats/video-editing-statistics/); see also [ALM Corp](https://almcorp.com/blog/video-editing-statistics/), [SendShort](https://sendshort.ai/statistics/video-editing-software/)
- [QUANT, aggregator] Video-editing software market estimated at $3.54B (2025) → $3.75B (2026) → $4.99B (2031), as repeated on aggregator pages (the underlying analyst firm is not named on the snippet). — [SendShort](https://sendshort.ai/statistics/video-editing-software/); [ALM Corp](https://almcorp.com/blog/video-editing-statistics/)

**High-end film and TV (Avid stronghold)**
- [QUANT, VENDOR] 2026 Academy Awards: 4 of 5 Best Film Editing nominees cut on Avid Media Composer, including the winner Andy Jurgensen (*One Battle After Another*). 8 of 10 Best Picture nominees used Avid tools. 87% of winning productions used Avid editing or sound tools. Every Best Sound nominee used Pro Tools. — [Avid press release via BroadcastBeat](https://broadcastbeat.com/news/celebrating-the-greatest-creators-one-battle-after-another-and-f1-among-majority-of-oscar-wins-created-using-avid) (original: [avid.com press room](https://www.avid.com/press-room/2026/03/one-battle-after-another-and-f1-among-majority-of-oscar-wins-created-using-avid), returned 403)
- [VENDOR, SNIPPET] *Anora* (2025 Best Picture and Best Film Editing winner) was edited with Premiere Pro. Adobe's April 2025 release says so. — [Adobe press release, 2 Apr 2025 (BusinessWire)](https://www.businesswire.com/news/home/20250402472642/en)
- [ANECDOTE] Avid's grip on shared-storage TV and film work depends on **bin locking**. A storage vendor calls it "a practically mandatory function of any professional editing application". Bins are separate files, so locking is per bin, while Premiere locks a whole project in Productions. — [ELEMENTS (storage vendor) blog](https://elements.tv/blog/bin-locking-overview-and-troubleshooting-in-avid-media-composer/); [RedShark News, 25 Aug 2026](https://www.redsharknews.com/avid-media-composer-2026-8-binning-bin-locking)
- [QUANT/VENDOR pricing] Media Composer 2026.8 (Aug 2026) drops bin locking on third-party storage that emulates NEXIS. To keep the feature, users must buy Avid NEXIS or move from Ultimate ($540/yr) to Enterprise ($900/yr). Hedge (maker of the Mimiq NEXIS emulator) called it "a walled garden disguised as a licensing update", and editors reacted with anger. Premiere Productions works on any shared storage. Resolve's collaboration is "storage-agnostic and folded into the standard, free edition." — [RedShark News, 25 Aug 2026](https://www.redsharknews.com/avid-media-composer-2026-8-binning-bin-locking)

**Independent and festival film (Premiere stronghold)**
- [QUANT, VENDOR] Sundance 2026: 85% of film entrants used Adobe Creative Cloud apps (Premiere, Frame.io, After Effects, Photoshop, Substance 3D), according to "the annual Sundance Institute survey." No Premiere-only figure was given. — [Adobe newsroom, 20 Jan 2026](https://news.adobe.com/news/2026/01/sundance-filmmakers-choose-adobe)
- [QUANT, VENDOR, SNIPPET] Sundance 2025: about 85% of entrants used Adobe CC, and Premiere Pro was used on about 60% of films. — [Adobe blog, 22 Jan 2025](https://blog.adobe.com/en/publish/2025/01/22/sundance-film-festival-2025-supporting-all-creators-filmmakers-and-artists); [Adobe media alert, Jan 2025](https://news.adobe.com/news/2025/1/adobe-delivers-new-innovations-and-ai-tools-for-video-pros-and-invests-6-million-in-filmmaker-community). ProVideo Coalition ran the headline "Premiere Pro used on 61% of festival films" (year not verified). — [ProVideo Coalition](https://www.provideocoalition.com/sundance-adobe-premiere-pro-used-on-61-of-festival-films/)
- [OLDER, ANECDOTE] A 2020 No Film School round-up of Sundance films found Premiere, Avid, FCP X and Resolve in use, with no aggregate stats. It concluded filmmakers chose "what you're comfortable with". — [No Film School, 23 Jan 2020](https://nofilmschool.com/editing-tools-films-sundance)

**Resolve and CapCut user base**
- [QUANT, VENDOR, OLDER] DaVinci Resolve had "more than 2 million" users of the free version alone by January 2019. Resolve runs on macOS, Windows, Linux and iPadOS. — [Wikipedia: DaVinci Resolve](https://en.wikipedia.org/wiki/DaVinci_Resolve)
- [QUANT, unverified] A claim that Resolve grew to "over 5.4 million users by 2023" turned up in search results, but no primary Blackmagic source confirmed it. — search result linked to [erkansaka.net (blog), Jun 2026](https://erkansaka.net/2026/06/01/davinci-resolve-adobe-video-editing-dominance/)
- [QUANT, aggregator] CapCut is reported at 323M MAU in 2025. CapCut's own 2025 year-end report did not disclose MAU. — [Splice blog, Feb 2026](https://spliceapp.com/blog/most-popular-video-editing-app-usa); [NetInfluencer, Dec 2025](https://www.netinfluencer.com/capcut-sees-surge-in-everyday-creator-use-in-2025-as-ai-tools-drive-editing-adoption/). [OLDER] An earlier milestone article reported 200M+ MAU. — [Lindsey Gamble](https://lindseygamble.com/blog/capcut-bytedances-video-editing-app-surpasses-200-million-monthly-active-users)

**Why editors switch (mainly Premiere → Resolve)**
- [ANECDOTE, trade press] Alexander Richter, "Goodbye Adobe Premiere" (Sep 2025), gave these reasons for leaving Premiere: (1) bugs, such as audio waveforms not lining up with the actual audio even after clearing caches; (2) cost: $6,598.80 for Creative Cloud over 10 years against $299 one-time for Resolve Studio; (3) AI features that are "half-baked" and prominently displayed while essential functions languish; (4) "death by a thousand cuts" stagnation. Pulls toward Resolve: integrated colour, Fairlight audio and compositing with no round-tripping, grading he rates above Lumetri, and a 4K project that opened in 21 s against 38 s in Premiere. His counterpoint: Resolve showed "hiccups and stutters" with 4K "much earlier" than Premiere, and has a learning curve. — [Digital Production, 1 Sep 2025](https://digitalproduction.com/2025/09/01/goodbye-adobe-premiere/)
- [ANECDOTE, trade press] Jana Johnston (Mar 2026) wrote she was "tired of getting slowed down by a constantly crashing software and not being able to render without artefacts." She praised Resolve's Live Save ("I never fear that something is gone!"), its multi-user collaboration ("everybody can work with the same project file… even at the same time, in the same timeline"), having all of post in one app ("no translation issues between software"), and metadata/smart bins. What switchers have to adjust to: database-style project libraries instead of project files, and losing Dynamic Link to Audition/After Effects/Media Encoder. — [Digital Production, 18 Mar 2026](https://digitalproduction.com/2026/03/18/getting-your-bearings-switching-from-premiere-pro-to-davinci-resolve/)
- [ANECDOTE] Prominent creators (for example Peter McKinnon) have publicly cited stability as their reason for moving from Premiere to Resolve. A rival-tool blog also notes Resolve's free tier, Linux and Windows-on-ARM builds, and scene-cut detection. — [EasyEdit blog](https://easyedit.pro/blog/da-vinci-resolve-vs-premiere-pro-why-editors-switch-to-da-vinci)
- [OLDER] The "editors switching to Resolve in droves" story was already running in April 2023. — [Slashdot, Apr 2023](https://news.slashdot.org/story/23/04/19/2245214/why-video-editors-are-switching-to-davinci-resolve-in-droves)
- [ANECDOTE] Resolve's free tier on Linux lacks H.264 support, a known friction point for Linux users. — [Hacker News "State of Kdenlive" thread, 2026](https://news.ycombinator.com/item?id=47815118) (comment by vladde)

### Inferences
- The market splits by segment, and a new editor should not chase "overall share". Avid holds collaborative long-form film and TV (bin locking, shared storage, trim). Premiere holds indie, agency and corporate work through the Creative Cloud ecosystem. Resolve takes cost-sensitive and quality-focused switchers. CapCut holds short-form and casual creators. An open-source editor's realistic first beachheads are (a) Linux users and creators priced out of subscriptions, and (b) people leaving Premiere because of crashes or subscriptions who find Resolve too heavy.
- Switching reasons point to stability and data safety as the primary purchase driver, ahead of features. Both trade-press switch stories lead with crashes, render artefacts or sync bugs, and praise autosave or Live Save.
- Avid's 2026.8 bin-locking change and Resolve's free, storage-agnostic collaboration suggest that **per-bin locking on commodity shared storage** is an open opportunity. Small facilities are actively annoyed with Avid right now (inference from the RedShark coverage).
- Cost is a recurring motive, but Resolve already offers a free tier. "Free" alone does not set an open-source editor apart from Resolve. Openness (Linux parity, no codec gating, scripting, extensibility, no vendor lock-in) needs to be part of the pitch.

### Gaps
- No independent 2024–2026 market-share survey with methodology was found (Statista figures are paywalled; aggregator figures are unsourced). No Frame.io, Production Expert or "Editors' survey" results for 2024–2026 surfaced in searches.
- Reddit polls and well-upvoted switching threads (r/editors, r/premiere, r/davinciresolve) could not be reached: both the fetch and search tools block the domain.
- An informal "StoryBlocks/OpusClip" claim that 4 of 5 interviewed YouTubers named Resolve as their main editor appeared in a 2026 roundup snippet but could not be traced to its original source.
- No data on Final Cut Pro, Vegas, Kdenlive or Shotcut user counts for 2024–2026 was found. No numbers were found for how many editors actually switched (only anecdotes).

---

## 2. Which features editors cite most as essential or missing

### Takeaway
Across trade press, switching stories and open-source complaints, the most cited "must work" items are **stability and crash-safe saving**, **smooth playback and performance** (GPU decode, high-resolution and 10-bit, proxies), and **collaboration and media management** (bin locking, search, smart bins). Next come **integrated colour** (with colour management for log footage) and **serious audio**, then **captions/transcription**, now shipped by every incumbent. Multicam, advanced trimming and HDR are expected in professional tools and are visibly missing or on the roadmap in open-source editors.

### Cited Findings
**Stability and data safety**
- [ANECDOTE] Constant crashes and render artefacts are cited as the reason for leaving Premiere. Resolve's Live Save is praised as a key comfort feature. — [Digital Production, Mar 2026](https://digitalproduction.com/2026/03/18/getting-your-bearings-switching-from-premiere-pro-to-davinci-resolve/)
- [ANECDOTE] Premiere audio/waveform sync bugs persisted even after clearing the cache. — [Digital Production, Sep 2025](https://digitalproduction.com/2025/09/01/goodbye-adobe-premiere/)
- [ANECDOTE] Kdenlive users report crashes and corrupted backup saves ("will crash and destroy your work"). Others report years without crashes. — [HN, 2026](https://news.ycombinator.com/item?id=47815118)

**Performance and playback**
- [ANECDOTE] Comparing load times and 4K playback is how switchers evaluate tools (Resolve opened a 4K project in 21 s against 38 s for Premiere, but stuttered earlier on 4K playback). — [Digital Production, Sep 2025](https://digitalproduction.com/2025/09/01/goodbye-adobe-premiere/)
- [ANECDOTE] Kdenlive users complain there is no smooth 2× playback with good audio (something Resolve does), no GPU acceleration, and performance regressions in large projects. — [HN, 2026](https://news.ycombinator.com/item?id=47815118)
- [VENDOR roadmap] Kdenlive's own short-term (26.12) roadmap puts GPU decoding ("important for playback"), GPU effects and transitions, 10/12-bit colour, OpenFX and advanced trimming tools at the top. — [Kdenlive roadmap](https://kdenlive.org/roadmap/)

**Collaboration and media management**
- [ANECDOTE/VENDOR] Bin locking is described as "practically mandatory" for professional editing. Per-bin locking (Avid) is valued over whole-project locking (Premiere Productions). — [ELEMENTS](https://elements.tv/blog/bin-locking-overview-and-troubleshooting-in-avid-media-composer/); [RedShark, Aug 2026](https://www.redsharknews.com/avid-media-composer-2026-8-binning-bin-locking)
- [ANECDOTE] Resolve's multi-user collaboration in the same timeline, and its metadata and smart bins, are cited as reasons to stay on Resolve. — [Digital Production, Mar 2026](https://digitalproduction.com/2026/03/18/getting-your-bearings-switching-from-premiere-pro-to-davinci-resolve/)
- [VENDOR, SNIPPET] Adobe made **Media Intelligence** (search footage by objects, locations, camera angle, shoot date, camera type) its headline Premiere Pro feature in April 2025. It is pitched as finding clips "from terabytes of footage in seconds". — [Adobe press release, 2 Apr 2025](https://www.businesswire.com/news/home/20250402472642/en)
- [VENDOR] The 2026 Best Editing Oscar winner singled out Avid's automatic transcription: "When you need to find just a word or a phrase you can search and find it in all the different takes." — [Avid via BroadcastBeat, Mar 2026](https://broadcastbeat.com/news/celebrating-the-greatest-creators-one-battle-after-another-and-f1-among-majority-of-oscar-wins-created-using-avid)

**Colour**
- [ANECDOTE] Resolve's grading quality ("more realistic" skin tones than Lumetri) is a pull factor for switchers. — [Digital Production, Sep 2025](https://digitalproduction.com/2025/09/01/goodbye-adobe-premiere/)
- [VENDOR, SNIPPET] Premiere Color Management became generally available in April 2025. It converts raw and log footage "from nearly every camera" to HDR or SDR on import. — [Adobe press release, 2 Apr 2025](https://www.businesswire.com/news/home/20250402472642/en)
- [ANECDOTE] Missing HDR handling was flagged as a Kdenlive gap. Shotcut 26.6 (June 2026) added HDR preview. — [HN, 2026](https://news.ycombinator.com/item?id=47815118); [UbuntuHandbook, Jun 2026](https://ubuntuhandbook.org/index.php/2026/06/shotcut-26-6-released-openfx-vst2-filters-support-hdr-preview/)

**Audio**
- [ANECDOTE] Fairlight inside Resolve "eliminates the need for separate Adobe Audition work", and is cited as a switching pull. — [Digital Production, Sep 2025](https://digitalproduction.com/2025/09/01/goodbye-adobe-premiere/)
- [VENDOR roadmap] Kdenlive lists "Audio routing and channel mapping (submix busses)" and LV2/VST hosting as mid-term roadmap items. That implies these are current gaps. — [Kdenlive roadmap](https://kdenlive.org/roadmap/)

**Captions and subtitles**
- [VENDOR] Every major incumbent now ships automatic captions. Final Cut Pro 11 (Nov 2024) added "Transcribe to Captions". — [Apple Newsroom, Nov 2024](https://www.apple.com/ae/newsroom/2024/11/final-cut-pro-11-begins-a-new-chapter-for-video-editing-on-mac). Premiere added AI **Caption Translation** in 27 languages in April 2025. — [Adobe press release (SNIPPET)](https://www.businesswire.com/news/home/20250402472642/en). Resolve 20 (May 2025) added AI Animated Subtitles. — [Wikipedia: DaVinci Resolve](https://en.wikipedia.org/wiki/DaVinci_Resolve)
- [QUANT-ish, VENDOR] Auto captions were among the top features in CapCut's 2025 year-end report, alongside AutoCut, voice filter, text-to-speech, effects, overlay and extract-audio. No per-feature counts were given. — [NetInfluencer, 31 Dec 2025](https://www.netinfluencer.com/capcut-sees-surge-in-everyday-creator-use-in-2025-as-ai-tools-drive-editing-adoption/)
- [REGULATORY] The European Accessibility Act has been enforceable since 28 June 2025 and requires captions on covered audiovisual content (micro-enterprises are exempt). This pushes captions toward "required" for corporate and broadcast deliverables. — [Wistia](https://wistia.com/learn/marketing/european-accessibility-act); [Kaltura](https://corp.kaltura.com/?p=219298)

**Multicam, trimming, titles, vertical**
- [VENDOR] Resolve 20 added "AI Multicam SmartSwitch" (automatic angle switching). — [Wikipedia: DaVinci Resolve](https://en.wikipedia.org/wiki/DaVinci_Resolve)
- [ANECDOTE, review sites] OpenShot lacks multicam editing, which comparison reviews call a critical limit for professional work. — [VideoProc: OpenShot vs Shotcut](https://www.videoproc.com/video-editor/openshot-vs-shotcut.htm)
- [VENDOR roadmap] Kdenlive lists "advanced trimming tools", "advanced external monitor" (with multi-camera) and "slow playback speed" as not yet delivered. — [Kdenlive roadmap](https://kdenlive.org/roadmap/)
- [ANECDOTE] Kdenlive's title creator was criticised as unintuitive. Basic crop and scale needed YouTube tutorials, unlike Camtasia's direct manipulation. — [HN, 2026](https://news.ycombinator.com/item?id=47815118) (nickjj)
- [VENDOR, SNIPPET] Premiere's Generative Extend gained 4K, **vertical orientation** and audio extension in April 2025. This shows vertical delivery is a first-class concern for Adobe. — [Adobe press release, 2 Apr 2025](https://www.businesswire.com/news/home/20250402472642/en)

### Inferences (evidence-weighted feature ranking for a new open-source NLE)
The ranking combines (a) how often each item appears in switching stories and complaints, (b) whether every incumbent has shipped it in 2024–2026 (a sign of "table stakes"), and (c) whether open-source editors are visibly criticised for lacking it. It is a synthesis, not survey data.

| Tier | Feature | Evidence basis |
|---|---|---|
| **Table stakes (absence = non-starter)** | Stability, plus crash-safe autosave/recovery (Live Save-style) | Top switching reason (both DP articles). Main Kdenlive complaint (HN). |
| | Smooth real-time playback: GPU/hardware decode, 4K/10-bit, proxies, smooth 2× and JKL shuttle | Switchers benchmark it. Kdenlive's top roadmap item. HN complaints. |
| | Core timeline editing and trimming (ripple, roll, slip, slide, keyboard-driven) | Kdenlive roadmap lists advanced trim as missing. (Pro-editor emphasis on trim is well known, but no 2024–26 survey quantifies it; see Gaps.) |
| | Captions/subtitles: auto-transcribe, edit, burn-in, SRT/VTT export | Shipped by FCP, Premiere, Resolve and CapCut in 2024–25. EAA in force June 2025. A top CapCut feature. |
| | Basic colour correction with scopes, plus colour management for log/raw | Premiere made colour management GA in 2025. Resolve's colour pulls switchers. |
| | Multitrack audio with mixing, keyframes and noise reduction | Fairlight is cited as a pull. Kdenlive lacks busses and routing. |
| | Social/vertical export presets | Adobe treats vertical as first-class. CapCut's audience is short-form. (Quantitative usage data not found.) |
| **Strong differentiators (pro segments)** | Collaboration: per-bin locking on commodity shared storage, multi-user projects | Avid 2026.8 backlash. Resolve offers it free. Premiere locks the whole project. |
| | Media management: transcript and phrase search, metadata, smart bins, content search | Oscar winner's quote on transcription search. Adobe Media Intelligence. Resolve smart bins praised. |
| | Multicam (with auto angle switching as a bonus) | Missing in OpenShot. Resolve 20 adds AI SmartSwitch. Kdenlive multicam monitor on roadmap. |
| | Plugin hosting: OFX video, VST3/AU/LV2 audio (see Q5) | Shotcut and Kdenlive are both rushing to add it in 2026. |
| | HDR and 10/12-bit pipeline, OCIO | Kdenlive gap. Shotcut 26.6 added HDR preview. Kdenlive has OCIO on its long-term roadmap. |
| | One-app integration (edit + colour + audio + compositing) | Resolve's core pitch. Switchers cite "no translation issues". |
| **Lower priority / segment-specific** | Motion graphics templates, scripting, hardware control surfaces, distributed rendering | Kdenlive roadmap items. Little 2024–26 user-demand evidence found. |

### Gaps
- No 2024–2026 survey ranks features quantitatively among professional editors. The ranking above is synthesised from qualitative evidence.
- No data on use of motion graphics templates (MOGRTs), auto-reframe or keyboard-trim modes. Evidence on how much trimming matters to editors (Avid trim mode and similar) could not be sourced for 2024–2026. It is commonly asserted, but no citable source was found here.
- Adobe's community "Ideas" board (top-voted Premiere requests) did not render when fetched. The Blackmagic forum's feature-request threads were not reached.

---

## 3. Which AI features editors actually use, and which they dismiss

### Takeaway
"Utility AI" is what editors adopt: transcription (search, captions, text-based editing), speech enhancement and noise reduction, masking and object selection, translation, and stem splitting. It is widely used and praised, including by an Oscar-winning editor. Generative extend is the one generative feature with a favourable professional reception, and it is used narrowly (a few extra frames). Generative B-roll, characters and wholesale generated content are dismissed by professionals as unreliable, full of artefacts and legally risky. There is also visible backlash against vendors promoting AI while core stability goes unfixed.

### Cited Findings
**Adoption numbers**
- [QUANT, SNIPPET] Wyzowl's 2026 survey reports that 63% of video marketers have used AI tools to create or edit marketing videos, up from 51% the year before. Another source citing Wyzowl gives 75% (conflicting; the Wyzowl page returned 403, so this could not be checked). — [Wyzowl Video Marketing Statistics 2026](https://wyzowl.com/video-marketing-statistics-2020); conflicting figure via [SocialPilot](https://www.socialpilot.co/blog/video-marketing-statistic)
- [QUANT, SNIPPET] A 2025 study of UK journalists found 49% use AI monthly for transcription and captioning. — [CNTI: AI transcription and translation in journalism](https://cnti.org/reports/ai-transcription-and-translation-in-journalism/)
- [QUANT-ish, VENDOR] CapCut's 2025 year-end report ranks auto captions, AutoCut (automatic assembly), voice filter and text-to-speech among its top features (no counts given). — [NetInfluencer, Dec 2025](https://www.netinfluencer.com/capcut-sees-surge-in-everyday-creator-use-in-2025-as-ai-tools-drive-editing-adoption/)

**Adopted and valued (utility AI)**
- [VENDOR, ANECDOTE] Transcription and phrase search: *One Battle After Another* editor Andy Jurgensen (2026 Oscar) said, "One thing I really got into on this movie was Avid's automatic transcribing tool… search and find it in all the different takes." — [Avid via BroadcastBeat](https://broadcastbeat.com/news/celebrating-the-greatest-creators-one-battle-after-another-and-f1-among-majority-of-oscar-wins-created-using-avid)
- [ANECDOTE, trade press] ProVideo Coalition roundtable (30 Dec 2025):
  - Scott Simmons: AI "tools are those things that make the editors work faster and more efficient", meaning they remove tedium rather than generate content.
  - Iain Anderson: recognition and classification ("utility AI") works well, for example Magic Mask-style selection and stem splitting.
  - Nick Lear: Topaz Video AI's Starlight model is "a massive step up" for restoring poor footage.
  - Jeff Foster: ElevenLabs voiceover is "working in regular corporate client productions now". — [PVC roundtable](https://www.provideocoalition.com/the-next-level-of-ai-cameras-remote-workflows-and-more-for-2026-a-pvc-roundtable-discussion/)
- [ANECDOTE, SNIPPET] PVC commentary summed it up as "Some tools work great—noise reduction, transcription, translation. Most are problematic—unreliable, lacking controls, creating new headaches." — [PVC roundtable](https://www.provideocoalition.com/the-next-level-of-ai-cameras-remote-workflows-and-more-for-2026-a-pvc-roundtable-discussion/) / [PVC: AI Tools 2025 (Jeff Foster)](https://www.provideocoalition.com/ai-tools-video-animation-advances-for-2025/) (the exact article for this quote was not confirmed)
- [VENDOR] Feature convergence across incumbents (a signal of demand):
  - FCP 11 (Nov 2024): Magnetic Mask and Transcribe to Captions. — [Apple Newsroom](https://www.apple.com/ae/newsroom/2024/11/final-cut-pro-11-begins-a-new-chapter-for-video-editing-on-mac)
  - Resolve 19 (Apr 2024): IntelliTrack, UltraNR. Resolve 20 (May 2025): IntelliScript, Animated Subtitles, Multicam SmartSwitch, Audio Assistant. — [Wikipedia](https://en.wikipedia.org/wiki/DaVinci_Resolve)
  - Premiere (Apr 2025): Media Intelligence, Caption Translation, Generative Extend GA. — [Adobe PR (SNIPPET)](https://www.businesswire.com/news/home/20250402472642/en)
  - Adobe's Sundance 2026 release leads with Object Selection and Mask. — [Adobe newsroom, Jan 2026](https://news.adobe.com/news/2026/01/sundance-filmmakers-choose-adobe)

**Generative extend (mostly positive, narrow use)**
- [ANECDOTE, trade press] PetaPixel (Feb 2025): Generative Extend is "surprisingly good and actually useful". It solves "my clip is just barely not long enough" for transitions or B-roll cover. — [PetaPixel, 12 Feb 2025](https://petapixel.com/2025/02/12/premiere-pros-ai-generative-extend-is-surprisingly-good-and-actually-useful/)
- [ANECDOTE] Reviewers call it one of the first generative video tools that is "truly useful" for professional post. — [Creative Bloq](https://www.creativebloq.com/entertainment/film-video/video-editors-are-already-impressed-by-adobes-ai-video-editing-tool-in-premiere-pro); [Videomaker review](https://www.videomaker.com/reviews/software/adobe-generative-extend-tool-review-a-tool-poised-to-change-how-we-edit-forever/)
- [VENDOR, SNIPPET] Limits at beta: a maximum of about 2 s, 1080p only, log footage not supported, works best on static or slow shots. — [ProVideo Coalition: Burning Questions](https://www.provideocoalition.com/burning-questions-about-generative-extend-in-premiere-pro-beta/); [Adobe Help](https://helpx.adobe.com/premiere-pro/using/generative-extend.html). The April 2025 GA added 4K, vertical and audio. — [Adobe PR](https://www.businesswire.com/news/home/20250402472642/en)

**Dismissed or distrusted**
- [ANECDOTE, trade press] Oliver Peters: generative video "is mostly substandard and full of artifacts, including random AI hallucinations… requires multiple attempts through different pieces of software." Jeff Foster: replacement B-roll and major characters are "still not ready for primetime." Iain Anderson: legal and reputational risks remain "even if you manage to create something believable." — [PVC roundtable, Dec 2025](https://www.provideocoalition.com/the-next-level-of-ai-cameras-remote-workflows-and-more-for-2026-a-pvc-roundtable-discussion/)
- [ANECDOTE] Backlash: Adobe's AI features are called "half-baked" and prominently displayed while essential functions languish. This was cited as one reason for leaving Premiere. — [Digital Production, Sep 2025](https://digitalproduction.com/2025/09/01/goodbye-adobe-premiere/)

### Inferences
- **Adopt first (high use, low controversy):** speech-to-text transcription feeding (a) caption generation and export, (b) transcript and phrase search across takes, and (c) text-based rough-cut editing; speech enhancement and voice isolation; noise reduction; caption translation. They run offline-capable (for example Whisper-class models), which suits an open-source editor's privacy pitch.
- **Second wave (valued by pros, more engineering):** AI masking and object tracking (Magic Mask, Magnetic Mask, Object Mask), scene-cut detection, auto multicam switching, stem separation.
- **Optional or later:** generative extend (well received, but needs a large video model and is limited to a few frames). Generative B-roll, avatars and object generation are dismissed by pros, so they are a low priority for a professional-focused open-source editor.
- Positioning matters. Editors resent AI that is pushed ahead of stability. Shipping AI as opt-in, local, non-destructive utilities ("make me faster") lines up with stated pro preferences.

### Gaps
- No vendor-published usage share per AI feature (for example, the percentage of Premiere users who use Enhance Speech or Generative Extend) was found.
- No 2024–2026 survey specifically of professional editors' AI feature use (as opposed to marketers or journalists) was found.
- Community sentiment on auto reframe, object removal and Resolve's IntelliScript text-based editing could not be gathered, because Reddit was inaccessible.

---

## 4. Common complaints about open-source editors (Kdenlive, Shotcut, OpenShot, Olive)

### Takeaway
The blockers to professional adoption are consistent: **stability and project corruption**, **performance** (no GPU acceleration, weak playback of high-resolution, high-bit-depth or HDR footage, slowdowns in large projects), **missing professional features** (multicam in OpenShot; advanced trim, audio busses and routing, plugin hosting, colour management in Kdenlive), and **UX friction** (titles, basic transforms). In 2026 both Kdenlive/MLT and Shotcut are racing to add OpenFX and VST/LV2 support plus GPU/HDR pipelines. That confirms these are recognised gaps. Olive stalled and is being rewritten.

### Cited Findings
**Kdenlive**
- [ANECDOTE] HN "State of Kdenlive" (2026) complaints:
  - Crashes and corruption of backup saves (users BodyCulture, yesimahuman, pubby).
  - Unintuitive title creator, and basic cropping and scaling that needs tutorials (nickjj).
  - No smooth 2× playback.
  - No GPU acceleration, and performance regressions in big projects (marginalia_nu).
  - No HDR (ekianjo).
  - Distro packages less stable than AppImage or Flatpak.
  Praise: a "sweet spot" between iMovie and Resolve, with a lighter feel than Resolve. — [Hacker News](https://news.ycombinator.com/item?id=47815118); [related comment](https://news.ycombinator.com/item?id=47815642)
- [ANECDOTE, user reviews, undated] One professional user expects "3–4 crashes" in a 4–5 hour working day. Others call it a "hobbyist project", while some report it has been stable on Windows for about 2 years. — [VideoHelp user reviews](https://www.videohelp.com/software/Kdenlive/reviews); [Blue Fox Consultant](https://www.bluefoxconsultant.com/en/blog/blue-fox-articles-2/kdenlive-in-professional-settings-the-open-source-alternative-to-the-giants-of-editing-139)
- [VENDOR roadmap] Not yet delivered:
  - Short term (26.12): 10/12-bit colour; GPU decoding (implemented, needs MLT tweaks); GPU effects and transitions; OpenFX; advanced trimming.
  - Mid term: LV2/VST (backend ready, state saving and UI need work); audio routing and submix busses; AI effects; multicam external monitor; slow-motion playback; Python scripting.
  - Long term: OpenColorIO; hardware control surfaces; distributed rendering; node compositing.
  - Done: OpenTimelineIO (25.04). — [Kdenlive roadmap](https://kdenlive.org/roadmap/)
- [VENDOR] OpenFX support was merged into core MLT for the 7.38 release. Early limits: no draw-suite plugins, no OpenGL plugins, no push-button parameters. — [KDE Discuss: OpenFX integrated into MLT](https://discuss.kde.org/t/openfx-integrated-into-mlt-kdenlive/46203); [KDE Discuss: OpenFX dev log](https://discuss.kde.org/t/basic-openfx-support-development-log/39851); [MLT PR #1186](https://github.com/mltframework/mlt/pull/1186)

**Shotcut**
- [VENDOR] Shotcut 26.6 (June 2026) added experimental OpenFX, VST2 and LV2 filter support: no embedded plugin UIs, no instruments, limited testing. It also added a "safe mode" that restarts without external plugins if Shotcut crashes within 30 s of launch, plus HDR preview and Vulkan display on Linux. — [Shotcut forum: External Plugin Support](https://forum.shotcut.org/t/external-plugin-support-openfx-vst2-and-lv2/51741); [UbuntuHandbook, Jun 2026](https://ubuntuhandbook.org/index.php/2026/06/shotcut-26-6-released-openfx-vst2-filters-support-hdr-preview/); [AlternativeTo, Jun 2026](https://alternativeto.net/news/2026/6/shotcut-26-6-brings-hdr-enhancements-initial-plugin-support-and-vulkan-display-on-linux/); [Phoronix](https://www.phoronix.com/news/Shotcut-26.6-Beta)
- [ANECDOTE] HN commenters find Shotcut simpler and sometimes more stable than Kdenlive, but less feature-rich. — [HN, 2026](https://news.ycombinator.com/item?id=47815118)

**OpenShot**
- [ANECDOTE, review sites] No multicam. Hardware acceleration is experimental and only works for some formats (MP4/H.264), with rendering mostly on the CPU. Weaker than Premiere and FCP in colour, audio post and long-form performance. — [VideoProc: OpenShot vs Shotcut](https://www.videoproc.com/video-editor/openshot-vs-shotcut.htm); [VideoProc: OpenShot review](https://www.videoproc.com/video-editor/openshot-review.htm)

**Olive**
- [ANECDOTE/dev update] After a development pause, Olive's developer posted a March 2025 update. The plan: rethink the project, rewrite the node editor from C++ to C#, base the rendering engine on Godot, make it modular, and aim for a stable release. As of that update it is still not stable. — [Olive 2025-03 update (Disroot Scribe mirror)](https://scribe.disroot.org/post/2429686)

### Inferences
- The dominant reason professionals reject open-source NLEs is **trust**: crashes, corrupted projects, and unpredictable playback. Feature checklists come second. An open-source editor that leads with crash-safety (atomic saves, versioned autosave, safe-mode plugin isolation, robust recovery) and predictable real-time playback would answer the most frequent complaint directly.
- The competitive bar among open-source editors is moving in 2026: OpenFX and VST/LV2 hosting, GPU decode, 10-bit and HDR. A new entrant without these will compare poorly even with Shotcut and Kdenlive within a release or two.
- Shotcut's "safe mode" shows a lesson from the field: third-party plugins are a major crash source, so plugin sandboxing or out-of-process hosting is worth designing in from the start.
- Linux is under-served by the commercial leaders (Premiere and FCP have no Linux builds; Resolve's free Linux tier lacks H.264 and has VST gaps). That is a natural niche for a polished open-source editor.

### Gaps
- No quantitative data on open-source NLE user numbers or professional adoption rates was found.
- r/kdenlive and r/VideoEditing threads could not be reached (Reddit is blocked).
- Olive's Wikipedia page returned 404. Olive's status after March 2025 was not verified.

---

## 5. Expectations for audio plugin support (VST3/AU) and third-party effects (OFX)

### Takeaway
Professional NLEs are expected to host third-party audio plugins (VST3 on Windows and macOS, AU on macOS) through a plugin manager that scans and enables plugins. Resolve also hosts OFX video effects, giving it access to the Boris FX, Red Giant and RE:Vision ecosystems. Premiere and FCP use proprietary effect APIs instead of OFX. In 2026 Kdenlive/MLT and Shotcut both started adding OFX and VST/LV2 support. Shotcut shipped it with safe-mode crash isolation, which signals that plugin hosting is now expected even of open-source editors, and that stability risk is the main design concern.

### Cited Findings
- [REFERENCE] OpenFX hosts include DaVinci Resolve (since v10), Vegas Pro (since v10), Vegas Movie Studio Platinum, Fusion, Nuke and Natron (open source). Premiere Pro, Final Cut Pro, Media Composer, Kdenlive, Shotcut and Olive are not listed as hosts (Avid DS, now discontinued, supported it). Plugin vendors listed include Red Giant, GenArts (Sapphire), RE:Vision Effects, NewBlueFX and Digital Film Tools. — [Wikipedia: OpenFX (API)](https://en.wikipedia.org/wiki/OpenFX_(API))
- [REFERENCE] DaVinci Resolve supports OpenFX, VST and AU plugins. — [Wikipedia: DaVinci Resolve](https://en.wikipedia.org/wiki/DaVinci_Resolve)
- [SNIPPET] Resolve 17.4 (25 Oct 2021) added VST3 support in Fairlight on macOS and Windows [OLDER]. Users had requested VST3 on the Blackmagic forum when only VST2 was supported. — [Mix Online](https://www.mixonline.com/?p=113932); [Blackmagic Forum: VST3 for Resolve](https://forum.blackmagicdesign.com/viewtopic.php?p=650771)
- [SNIPPET] A third-party community project exists to bring VST plugins to Resolve on Linux. That suggests native Linux audio plugin support in Resolve is limited (inferred from the project's existence; not verified in Blackmagic documentation). — [VSTForResolveLinux changelog](https://raw.githubusercontent.com/JaySNL/VSTForResolveLinux/main/CHANGELOG.md)
- [SNIPPET] Premiere Pro supports third-party VST3 plugins, plus AU on macOS. An Audio Plug-in Manager (Preferences > Audio, or the Audio Track Mixer menu) scans for plugins and enables or disables them. — [FxFactory support: Enable audio plug-ins in Premiere Pro](https://support.fxfactory.com/article/164-enable-audio-plug-ins-in-premiere-pro); [PremiumBeat](https://www.premiumbeat.com/blog/new-features-and-enhancements-in-premiere-pro-cc); [Adobe: Managing third-party audio plug-ins](https://helpx.adobe.com/media-encoder/desktop/encoding-quick-start-and-basics/adding-third-party-plugins.html). The Premiere plugin SDK documents its own (non-OFX) effect plugin types. — [Premiere Pro Plug-in SDK guide](https://premiere-plugin-sdk-guide.readthedocs.io/intro/premiere-pro-plugin-types.html)
- [SNIPPET] Audio Units is Apple's Core Audio plugin format, used by Logic Pro and Final Cut Pro. — [Tella glossary](https://www.tella.com/definition/audio-plugins)
- [SNIPPET, ANECDOTE] Some plugins only expose certain channel configurations in VST3 (for example Exponential Audio Stratus 3D and Symphony 3D), so VST3 matters for surround and immersive mixing. — [Blackmagic Forum: VST3 for Resolve](https://forum.blackmagicdesign.com/viewtopic.php?p=706567)
- [VENDOR] Shotcut 26.6 looks for plugins in the OS's standard folders (it bundles none). Support is VST2, LV2 and OpenFX filters, with no plugin UIs except a special-cased Valhalla Supermassive and no instruments. A safe mode skips external plugins after a crash at startup. — [Shotcut forum](https://forum.shotcut.org/t/external-plugin-support-openfx-vst2-and-lv2/51741); [UbuntuHandbook](https://ubuntuhandbook.org/index.php/2026/06/shotcut-26-6-released-openfx-vst2-filters-support-hdr-preview/)
- [VENDOR] Kdenlive's LV2/VST backend is "ready" but state saving and UI still need work. OpenFX is in MLT 7.38 with early limitations. — [Kdenlive roadmap](https://kdenlive.org/roadmap/); [KDE Discuss](https://discuss.kde.org/t/openfx-integrated-into-mlt-kdenlive/46203)

### Inferences
- **Audio:** VST3 (all OSes), AU (macOS) and LV2 (Linux) hosting, with a scan/validate/enable plugin manager and **plugin state saved in the project**, is the expected baseline. Plugin UIs (editor windows) matter. Shotcut's lack of embedded UIs is one of its stated limitations, and Kdenlive's blocker is "state saving and UI". VST3 specifically matters for multichannel and immersive work.
- **Video effects:** OFX is the only cross-vendor open standard. Hosting it opens the Boris FX, Red Giant, RE:Vision, NewBlue and similar catalogues already shipped for Resolve and Vegas, and it is the realistic route for an open-source editor (Premiere's and FCP's APIs are proprietary). Full value needs the OFX draw suite (on-screen controls) and GPU/OpenGL rendering paths, which are exactly the gaps the MLT implementation still has.
- **Robustness:** isolate plugins (out-of-process hosting or a safe mode with a blocklist), because third-party plugins are a leading crash source, and stability is editors' top concern (Q2 and Q4).

### Gaps
- No survey data was found on what share of editors depend on third-party audio or OFX plugins.
- Exact Premiere version history for VST3, and Resolve's current Linux plugin support, could not be checked in primary docs (Adobe and Toolfarm pages returned 403).
- No 2024–2026 community evidence was found on whether CLAP (a newer audio plugin format) is requested in NLEs.
- VST2 licensing for new hosts (widely said to be closed by Steinberg) was not checked against a source here. Worth confirming before VST2 is planned alongside VST3/LV2.
