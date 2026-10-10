# Audio plugin scanning, validation and hosting (VST3, CLAP, LV2, Audio Units) for a C++20 / Qt 6 NLE

Research date: 7 October 2026. Every licence claim below was checked against the licence file itself, fetched raw from the repository on that date, unless marked otherwise. Context: Montage has no LICENSE file in its repository root (checked locally), so the licence of anything it links against still matters.

## 1. Licences and availability as of 2026 (SDKs and host helpers)

### Takeaway
All four formats can now be hosted under permissive licences. The VST3 SDK moved from its GPLv3/proprietary dual licence to **MIT** with VST 3.8.0 (29 October 2025). CLAP and its helpers are MIT. lilv and the LV2 stack are ISC. Audio Units are an Apple system framework. Avoid JUCE (AGPLv3 or a paid tier) and avoid copying code from GPL hosts (Carla, Ardour), but read them freely as design references.

### Cited Findings
**VST3 (Steinberg)**
- `vst3sdk/LICENSE.txt` is the MIT License, "Copyright (c) 2026, Steinberg Media Technologies GmbH". — [vst3sdk LICENSE.txt](https://github.com/steinbergmedia/vst3sdk/blob/master/LICENSE.txt)
- The sub-repositories a host actually compiles are each MIT with the same copyright line: `vst3_pluginterfaces`, `vst3_base` and `vst3_public_sdk`. — [pluginterfaces LICENSE.txt](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/LICENSE.txt), [base LICENSE.txt](https://github.com/steinbergmedia/vst3_base/blob/master/LICENSE.txt), [public_sdk LICENSE.txt](https://github.com/steinbergmedia/vst3_public_sdk/blob/master/LICENSE.txt)
- VSTGUI keeps its own 3-clause BSD-style licence, "(c) 2022, Steinberg Media Technologies". A host does not need VSTGUI. — [vstgui LICENSE](https://github.com/steinbergmedia/vstgui/blob/master/LICENSE)
- Steinberg's press release of 29 October 2025: "By releasing the VST 3.8 SDK, Steinberg transitions to the MIT Open Source License, marking a significant shift from the previous dual licensing model". The MIT licence "permits free use, modification, and distribution of software, as long as the original copyright and license text are retained … in both commercial and non-commercial software products". VST 3.8 also brings interface changes for MIDI 2.0 and a VSTGUI update. — [Steinberg press release PDF, 2025-10-29](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf)
- The same press release says the separate **ASIO SDK** is now "available under GPLv3 licensing, allowing both proprietary and open-source licensing options to be offered side by side". ASIO is a Windows audio-driver API, not a plugin format, and is not MIT. — [Steinberg press release PDF](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf)
- Steinberg's developer portal: "Since version 3.8 VST 3 is licensed under MIT license." It adds: "Trademark usage (e.g. 'VST' name or logo) is optional under MIT license, but if used, must comply with Steinberg's official trademark rules". The exceptions are "VSTGUI and mda files which are under BSD-like license". — [VST 3 Licensing (dev portal)](https://steinbergmedia.github.io/vst3_dev_portal/pages/VST+3+Licensing/Index.html)
- The 3.8.0 release notes add `IMidiLearn2` and `IMidiMapping2` and update VSTGUI to 4.15.0. — [Steinberg forum: VST 3.8.0 SDK Released](https://forums.steinberg.net/t/vst-3-8-0-sdk-released/1011988); also reported by [Sonicstate](https://sonicstate.com/news/2025/10/30/vst-3-now-available-under-mit-license)

**CLAP**
- `free-audio/clap` is MIT, "Copyright (c) 2021 Alexandre BIQUE". — [clap LICENSE](https://github.com/free-audio/clap/blob/main/LICENSE)
- The current header version is CLAP **1.2.10** (`CLAP_VERSION_MAJOR 1`, `MINOR 2`, `REVISION 10`). "Version 1.X.Y correspond to the release stage, API and ABI are stable". — [clap version.h](https://github.com/free-audio/clap/blob/main/include/clap/version.h)
- `clap-helpers` is MIT (Alexandre Bique, 2021). — [clap-helpers LICENSE](https://github.com/free-audio/clap-helpers/blob/main/LICENSE)
- `clap-wrapper` is MIT ("Copyright (c) 2022 defiantnerd"). — [clap-wrapper LICENSE](https://github.com/free-audio/clap-wrapper/blob/main/LICENSE)
- `free-audio/clap-host` is an MIT reference host. Its README says: "This repo serves as an example to demonstrate how to create a CLAP host" and "The host uses Qt for its GUI". Its CMakeLists uses `find_package(Qt6Core …)`, `Qt6Widgets`, RtAudio and RtMidi, and it ships CMake presets for vcpkg static and system dynamic builds. — [clap-host README](https://github.com/free-audio/clap-host/blob/main/README.md), [clap-host LICENSE](https://github.com/free-audio/clap-host/blob/main/LICENSE), [clap-host CMakeLists.txt](https://github.com/free-audio/clap-host/blob/main/CMakeLists.txt)

**LV2**
- lilv's `COPYING` reads "Copyright 2011-2026 David Robillard" and then the ISC permission text ("Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted, provided that the above copyright notice and this permission notice appear in all copies"). — [lilv COPYING](https://gitlab.com/lv2/lilv/-/blob/main/COPYING). Conflict: GitLab's repository badge labels it "BSD Zero Clause License" ([gitlab.com/lv2/lilv](https://gitlab.com/lv2/lilv)), but the file keeps the copyright-notice condition, which is ISC wording, not 0BSD.
- The latest lilv releases are **0.28.0** (stable, 8 June 2026), which added `lilv_state_get_bundle_path()` and fixed a crash loading plugin classes on Windows, and **0.28.1** (11 June 2026). — [lilv NEWS](https://gitlab.com/lv2/lilv/-/blob/main/NEWS)
- lilv builds with **Meson**. Its `meson.build` declares dependencies on `zix`, `serd`, `sord`, `lv2` and `sratom`. — [lilv meson.build](https://gitlab.com/lv2/lilv/-/blob/main/meson.build)
- suil, the LV2 plugin-UI embedding library, carries the same ISC-style text ("Copyright 2007-2025 David Robillard … Permission to use, copy, modify, and/or distribute this software for any purpose"). Its latest NEWS entry is 0.10.27. — [suil COPYING](https://gitlab.com/lv2/suil/-/blob/main/COPYING)

**Audio Units (Apple)**
- `AVAudioUnitComponentManager` (macOS 10.10+) "provides a way to search and query audio components that the system registers". It "has methods to find various information about the audio components without opening them", and it can search by tags, by `NSPredicate` or by `AudioComponentDescription`. — [Apple: AVAudioUnitComponentManager](https://developer.apple.com/documentation/avfaudio/avaudiounitcomponentmanager)
- `kAudioComponentInstantiation_LoadOutOfProcess` is available from macOS 10.11. — [Apple: loadOutOfProcess](https://developer.apple.com/documentation/audiotoolbox/audiocomponentinstantiationoptions/loadoutofprocess)

**Other host helpers**
- Tracktion's **choc** is ISC ("Copyright (c) 2025 Tracktion Corporation"). One caveat: `choc::ui::WebView` on Windows embeds Microsoft redistributable code under its own permissive licence. — [choc LICENSE.md](https://github.com/Tracktion/choc/blob/main/LICENSE.md)
- **Carla** "is open source and licensed under the GNU General Public License, version 2 or later". It hosts LADSPA, DSSI, LV2, VST2, VST3 and AU, and supports plugin bridges. — [Carla README](https://github.com/falkTX/Carla/blob/main/README.md)
- **Ardour**'s plugin manager, the source of the scanning design in section 3, is GPL code in `libs/ardour/plugin_manager.cc`. — [Ardour plugin_manager.cc](https://github.com/Ardour/ardour/blob/master/libs/ardour/plugin_manager.cc)
- **JUCE 8** is offered under AGPLv3 or a commercial EULA. Forum threads describe the tiers as Starter (free, up to $20k annual revenue), Indie (up to $300k, about $40/month) and Pro (no limit, about $175/month). A closed-source product that does not want AGPLv3 needs a JUCE licence. — [JUCE forum: Amendments to the JUCE EULA for JUCE 8](https://forum.juce.com/t/amendments-to-the-juce-end-user-licence-agreement-for-juce-8/61265); [JUCE forum: Revenue limits for JUCE tiers](https://forum.juce.com/t/revenue-limits-for-juce-tiers/61058/23). The tier figures come from search summaries of forum posts, not from the EULA itself.

### Inferences
- The old reason to put VST3 off ("wait until the licence allows it") is gone. The SDK parts a host needs (pluginterfaces, base, public.sdk hosting) are MIT, so VST3 can ship in a closed or permissively licensed Montage with only attribution. Add the Steinberg MIT notice to the app's third-party notices file. Do not use the "VST" logo unless you follow Steinberg's trademark rules.
- A no-JUCE, permissive stack: CLAP headers + clap-helpers (MIT), VST3 pluginterfaces/base/public.sdk (MIT), lilv + suil (ISC), Apple AudioToolbox/AVFAudio (system), and optionally choc (ISC) for small utilities. Use `clap-host` (MIT, Qt 6) as the reference implementation to read and borrow from.
- Treat Carla and Ardour as reading material only. Copying their code would bring GPL obligations into Montage, which has no declared licence.
- `clap-wrapper` turns CLAP plugins into VST3 or AUv2 plugins. It is a plugin-author tool and gives a host little.

### Gaps
- The serd, sord, sratom and zix `COPYING` files could not be read directly: GitLab served a Cloudflare challenge to the fetcher. They are by the same author and are believed to be ISC, but that is unverified here. Check them before shipping.
- Steinberg's "official trademark rules" document (logo use) was not fetched. Logo use is optional and is not needed to host plugins.
- The JUCE EULA text itself was not fetched; the tier figures are from forum or search summaries.
- The `clap-helpers` README returned 404 at the guessed path, so the exact list of host-side helpers (event lists, parameter queues, a plugin proxy) was not confirmed.
- VST2 was not researched. Its SDK is no longer offered by Steinberg; that comes from general knowledge, not a source checked here.

## 2. Standard plugin search paths per OS and environment overrides

### Takeaway
Each format defines its own user and system folders. CLAP and LV2 also define a PATH-style environment variable (`CLAP_PATH`, `LV2_PATH`). VST3 defines no environment variable; it uses fixed folders plus an application-local folder. Audio Units are found through the Component Manager, not by walking folders.

### Cited Findings
**VST3** (official, in priority order; "the first matching plugin is used"; "Links, Symbolic links or Shortcuts could be used from these predefined folders") — [VST3 Plug-in Locations](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Locations.html)
- Windows: `%LOCALAPPDATA%\Programs\Common\VST3\` (user) → `C:\Program Files\Common Files\VST3\` (global, native bitness) → `C:\Program Files (x86)\Common Files\VST3\` (32-bit on 64-bit Windows) → `$APPFOLDER\VST3\`
- macOS: `~/Library/Audio/Plug-ins/VST3/` → `/Library/Audio/Plug-ins/VST3/` → `/Network/Library/Audio/Plug-ins/VST3/` → `$APPFOLDER/Contents/VST3/`
- Linux: `$HOME/.vst3/` → `/usr/lib64/vst3/` → `/usr/lib/vst3/` → `/usr/local/lib64/vst3/` → `/usr/local/lib/vst3/` → `$APPFOLDER/vst3/`
- Ardour hard-codes these same lists: `~/Library/Audio/Plug-Ins/VST3:/Library/Audio/Plug-Ins/VST3:/Network/Library/Audio/Plug-ins/VST3` on macOS, `<Program Files>/Common Files/VST3` on Windows, and `~/.vst3:/usr/lib64/vst3:/usr/lib/vst3:/usr/local/lib64/vst3:/usr/local/lib/vst3` on Linux. — [Ardour plugin_manager.cc, `vst3_discover_from_path` calls](https://github.com/Ardour/ardour/blob/master/libs/ardour/plugin_manager.cc)
- The VST3 SDK's hosting module exposes `static PathList getModulePaths()`, which enumerates the standard locations. — [vst3_public_sdk hosting/module.h](https://github.com/steinbergmedia/vst3_public_sdk/blob/master/source/vst/hosting/module.h)

**CLAP** (from the normative comment in `entry.h`) — [clap entry.h](https://github.com/free-audio/clap/blob/main/include/clap/entry.h)
- Linux: `~/.clap`, `/usr/lib/clap`
- Windows: `%COMMONPROGRAMFILES%\CLAP`, `%LOCALAPPDATA%\Programs\Common\CLAP`
- macOS: `/Library/Audio/Plug-Ins/CLAP`, `~/Library/Audio/Plug-Ins/CLAP`
- "a CLAP host must query the environment for a CLAP_PATH variable, which is a list of directories formatted in the same manner as the host OS binary search path (PATH on Unix, separated by `:` and Path on Windows, separated by `;`)". Hosts search directories recursively for entries ending in `.clap`.

**LV2** — [LV2 Filesystem Hierarchy Standard](https://lv2plug.in/pages/filesystem-hierarchy-standard.html)
- Linux: user `$HOME/.lv2`; system `$PREFIX/lib/lv2` (default `/usr/local/lib/lv2`), with the user path searched first
- macOS: user `$HOME/Library/Audio/Plug-Ins/LV2`; system `/Library/Audio/Plug-Ins/LV2`
- Windows: user `%APPDATA%\LV2`; system `%COMMONPROGRAMFILES%\LV2`
- "the environment variable `LV2_PATH` is the search path for LV2 bundles. Like the `PATH` variable for programs, it is colon-delimited on Unix and OSX, semicolon-delimited on Windows, and searched from left to right."
- lilv 0.26.4 fixed "default LV2 path on cross-compiled Windows builds", so lilv computes these defaults itself. — [lilv NEWS](https://gitlab.com/lv2/lilv/-/blob/main/NEWS)

**Audio Units (macOS only)**
- Hosts find AUs through the system registry (`AVAudioUnitComponentManager` / `AudioComponentDescription` searches) "without opening them". — [Apple: AVAudioUnitComponentManager](https://developer.apple.com/documentation/avfaudio/avaudiounitcomponentmanager)

### Inferences
- Build one `PluginPaths` service. For each format, search in this order: the user's custom folders from Preferences, then the environment variable (`CLAP_PATH`, `LV2_PATH`; Montage could also honour a non-standard `VST3_PATH` but must label it as its own extension), then the official defaults listed above. De-duplicate by canonical path (`QFileInfo::canonicalFilePath`) so symlinked folders are scanned once.
- Many Linux distributions install to `/usr/lib/lv2` and `/usr/lib/clap` (and multiarch variants), not `/usr/local`. Leave LV2 discovery to lilv (`lilv_world_load_all` honours `LV2_PATH` and the platform defaults) rather than reimplementing it.
- AU components normally live in `~/Library/Audio/Plug-Ins/Components` and `/Library/Audio/Plug-Ins/Components`. Montage should still enumerate through the Component Manager, which also covers AUv3 app extensions that have no `.component` file.

### Gaps
- `VST3_PATH` is not an official Steinberg variable; no Steinberg document mentions it.
- Apple's own page listing the `Components` folder paths was not fetched; the paths above are general knowledge.

## 3. How market-leading NLEs and DAWs scan plugins: isolation, block lists, caches, rescans, timeouts

### Takeaway
Mature hosts run the scan out of process, one plugin bundle per helper-process launch, with a timeout. Before trying a plugin they mark it as suspect, so a crash leaves it blocked. They cache results per module and treat the cache as stale when the file's timestamp changes. They offer "scan new", "rescan selected" and "full reset" actions. Ardour is the best documented of these (its source is public). Bitwig goes furthest and also isolates plugins *while they run*. Apple moves AU loading out of process at the OS level from macOS 11.

### Cited Findings
**Ardour (source read directly)** — [plugin_manager.cc](https://github.com/Ardour/ardour/blob/master/libs/ardour/plugin_manager.cc), [vst3_scan.cc](https://github.com/Ardour/ardour/blob/master/libs/ardour/vst3_scan.cc)
- It uses separate scanner executables, one per format: `ardour-vst-scanner`, `ardour-vst3-scanner` and `ardour-au-scanner` (with `.exe` variants). If a scanner binary is missing, Ardour falls back to an "internal scan" in the host process.
- Block lists are per format and per architecture: `vst3_x64_blacklist.txt`, `vst3_x86_blacklist.txt`, `vst3_a64_blacklist.txt`, `vst2_x64_blacklist.txt`, `auv2_a64_blacklist.txt` and others. The user can clear them (`clear_vst3_blacklist`, `clear_au_blacklist`).
- Crash safety comes from blocking *first*: before launching the scanner, Ardour calls `vst3_blacklist(module_path)`. Only after the cache file parses does it call `vst3_whitelist(module_path)`. A crash or hang therefore leaves the plugin blocked.
- Timeout: the scanner runs under `ARDOUR::SystemExec`. The host polls every decisecond, with `timeout = 1 + Config->get_plugin_scan_timeout()` (in deciseconds). It calls `scanner.terminate()` on timeout or cancel. The UI offers "cancel one" and "cancel all" (`cancel_scan_timeout_one`, `cancel_scan_timeout_all`).
- Cache: one file per module, named by the SHA-1 of the module path with a `.v3i` extension, in `vst3_info_cache_dir()`. The cache is valid when the plugin's `st_mtime` is older than the cache file's `st_mtime`. The root `VST3Cache` node stores `version`, `bundle` and `module`. A cache file whose `module` does not match is treated as invalid and blocked.
- Fields cached per class (`VST3Info`): `uid`, `name`, `vendor`, `category`, `version`, `sdk-version`, `url`, `email`, `n_inputs`, `n_outputs`, `n_aux_inputs`, `n_aux_outputs`, `n_midi_inputs`, `n_midi_outputs`.
- A `cache_only` mode lists plugins from the cache without scanning and reports them as "New" or "Updated", so start-up stays fast.

**Bitwig Studio (runtime sandboxing)**
- There are five "Plug-in Hosting Modes": "Within Bitwig" (in the engine, least memory, one crash takes all audio down), "Together" (the default; plugins share one sandbox separate from the engine), "By manufacturer", "By plug-in" and "Individually" (the safest and the most memory). — [Bitwig user guide: Plug-in Handling and Options](https://bitwig.com/userguide/latest/vst_plug-in_handling_and_options); [Bitwig support: What is plug-in crash protection?](https://bitwig.com/support/technical_support/what-is-plug-in-crash-protection-26)
- A crashed plugin shows a notice in its UI, with "Reload Plug-in" and "Reload All Plug-ins". A project already loaded is not reloaded automatically when the hosting mode changes. — [Bitwig user guide](https://bitwig.com/userguide/latest/vst_plug-in_handling_and_options)

**REAPER**
- The scan cache is the plain-text `reaper-vstplugins64.ini`, rebuilt by "Clear cache/re-scan" (Preferences > Plug-ins > VST). Entries look like `pluginname.vst3=657CEBECA4E6D501,1326722673,pluginname (vendor)`. Blocked plugins stay blocked across "Clear cache/re-scan"; the fix is to delete their line from the ini by hand. — [Mixwave support: REAPER troubleshooting](https://support.mixwave.com/doc/reaper-troubleshooting) (third-party vendor documentation, not Cockos)
- REAPER bridges 32-bit plugins under 64-bit, and the bridge can be chosen per plugin ("Run As" > "Embed Bridged UI"). — [Mixwave support](https://support.mixwave.com/help/reaper-troubleshooting)

**Adobe Premiere Pro (Audio Plug-in Manager)**
- It opens from Preferences > Audio > Manage Audio Plug-ins, or from the Effects panel's flyout menu. "Scan for Plug-ins" loads new plugins from disk. "Rescan existing plug-ins" re-reads updated ones. A checkbox per plugin enables or disables it, and "Enable All" turns them all on. The "VST Plug-in Folders" list applies only to legacy VST2; users should not add the VST3 folder there. — [Adobe HelpX: Managing third-party audio plug-ins](https://helpx.adobe.com/media-encoder/desktop/encoding-quick-start-and-basics/adding-third-party-plugins.html) (returned 403 to a direct fetch; these details come from search snippets), [FXFactory: Enable audio plug-ins in Premiere Pro](https://support.fxfactory.com/article/164-enable-audio-plug-ins-in-premiere-pro), [ProVideo Coalition: Tame your audio effects](https://www.provideocoalition.com/tool-tip-tuesday-for-adobe-premiere-pro-tame-your-audio-effects/)
- Scanning still breaks in 2026. One Adobe community bug report is titled "premiere beta 26.5 (27.0) does not scan any vst3 or au audio plug-ins on macos". — [Adobe community](https://community.adobe.com/questions-734/premiere-beta-26-5-27-0-does-not-scan-any-vst3-or-au-audio-plug-ins-on-macos-1644421)

**Logic Pro / Final Cut Pro (Audio Units)**
- Logic validates every AU with Apple's AU validation tool (`auvaltool`). Plugins that fail are excluded "to prevent possible problems or crashes", and the Plug-in Manager shows "failed validation" or "crashed validation" in a Compatibility column. "Reset & Rescan Selection" re-runs validation for the selected plugins; "Full Audio Unit Reset" rescans every AU. — [Apple Support: If you can't find a recently installed plug-in for Logic Pro](https://support.apple.com/en-ng/122179); [Soundtoys: Plug-In Scanning in Logic Pro](https://support.soundtoys.com/article/61-plug-in-scanning-in-logic-pro); [Source Elements: Incompatible Audio Units found](https://support.source-elements.com/source-elements-error-messages/incompatible-audio-units-found-in-logic-pro)
- "Logic Pro and Final Cut Pro support most Audio Units v2 and Audio Units v3 plug-ins on Mac computers with Apple silicon"; Intel-only AUs need Rosetta. — [Apple Support 102082](https://support.apple.com/en-US/102082)
- From macOS 11, "the system loads audio units into a separate process that depends on the architecture or host preference. This increases stability and security by isolating audio units from their host app, and allows loading x86 audio units into an Apple silicon host app". The hosting processes are `AUHostingServiceXPC` (native) and `AUHostingServiceXPC_arrow` (Rosetta). Apple warns hosts not to "assume callbacks happen on the main thread", and says out-of-process AUs cannot share pointers through custom properties. — [Apple: Debugging Out-of-Process Audio Units on Apple Silicon](https://developer.apple.com/documentation/audiotoolbox/debugging-out-of-process-audio-units-on-apple-silicon)

**Ableton Live**
- The relevant preferences are "Use Audio Units" (macOS) and "Use VST3 Plug-In System Folders", plus a "Rescan" button. Option-click (macOS) or Alt-click (Windows) on Rescan runs a deep rescan that "forces Live to scan all installed plugins, including previously ignored or blacklisted ones". — [Sonnox support: Rescanning plug-ins in Ableton Live](https://support.sonnox.com/support/solutions/articles/22000201335-re-scanning-plug-ins-in-ableton-live)
- Ableton forum users report Live 12 freezing ("Application Not Responding") at start-up while scanning plugins. — [Ableton forum thread](https://forum.ableton.com/viewtopic.php?p=1827150)

**DaVinci Resolve / Fairlight**
- Resolve 17.4.2 fixed "rescanning on startup with crashed VST3 plugins" and a VST3 scanning problem on certain system languages. — [Newsshooter: DaVinci Resolve 17.4.2](https://www.newsshooter.com/2021/11/18/blackmagic-davinci-resolve-17-4-2/)
- Users report that when Resolve's scanner crashes it does not add the plugin to a block list and loops endlessly, with no list of plugins that scanned successfully. Users also report AU plugins crashing the Fairlight page. — Blackmagic Forum user reports, e.g. [Fairlight – multiple crashes plugins/automation](https://forum.blackmagicdesign.com/viewtopic.php?p=772583) and [Native Instruments community: Phasis AU crashed DaVinci Resolve 19.1.2](https://community.native-instruments.com/discussion/40877/phasis-au-crashed-davinci-resolve-19-1-2). These are anecdotal, from search snippets, and the thread each claim came from could not be confirmed.

### Inferences
**Recommended scanning architecture for Montage**, combining Ardour's write-ahead block list, Bitwig's isolation and Logic/Ableton's rescan actions:
1. **A `montage-plugin-scanner` helper executable**, built next to `montage` and `montage-cli`. It takes one argument (`--format clap|vst3|lv2|au --path <bundle>`, or an AU type/subtype/manufacturer triple) and writes JSON describing every class or plugin in that bundle to stdout. It links only the format SDKs and no Qt GUI (QtCore at most).
2. **The parent drives it with `QProcess`**: run N in parallel (for example `QThread::idealThreadCount()/2`), with a per-plugin timeout (a default near 30 s is an assumption to tune; Ardour makes it configurable), and `kill()` on timeout. A non-zero exit, a signal, or JSON that does not parse counts as a failure.
3. **A write-ahead block list**: write `{path, status:"scanning"}` to the cache before launching the helper, and replace it with `ok` or `failed` afterwards. If Montage itself dies mid-scan, the next launch finds `scanning` entries and marks them `failed` (Ardour's blacklist-then-whitelist idea).
4. **Cache key** = format + canonical bundle path + mtime + size (+ the plugin binary's mtime for bundles) + host architecture + scanner schema version. Ardour uses only path-hash + mtime, but adding size and arch cheaply avoids stale results after an x64 → arm64 change or a reinstall that keeps the same mtime. Store everything in one JSON or SQLite file under `QStandardPaths::AppDataLocation`, not one file per plugin.
5. **Fields to cache**: format, unique ID (VST3 CID, CLAP id, LV2 URI, AU type/subtype/manufacturer), name, vendor, version, category or features, SDK version, main and aux input/output channel counts, MIDI ports, whether it has an editor, and the latency reported at scan time (as a hint only; the real value is read again on activation). Also `status`, `error`, `scan_duration_ms` and `user_enabled` (a Premiere-style checkbox).
6. **UI**: Preferences > Audio Plug-ins with "Scan for new", "Rescan selected", "Full rescan (including blocked)", per-plugin enable checkboxes, a "show failed" filter and a scan log. Run the start-up scan in the background (cache-only first, like Ardour's `cache_only`), so a slow plugin never blocks launch. Ableton and Resolve both have user reports of exactly that kind of hang.
7. **Runtime isolation (later)**: a Bitwig-style out-of-process host is a large project (shared-memory audio buffers, IPC for parameters, reparenting the editor window across processes). For an NLE, scan-time isolation plus in-process hosting is a reasonable v1; put "sandbox mode" on the roadmap. On macOS, AUs already run out of process from macOS 11 when the system chooses to.

### Gaps
- None of the sources found document Resolve's or Premiere's internal scanning architecture (whether scanning runs out of process, the timeout values, the cache format).
- No official documentation was found for Ableton's scanner process model or cache format.
- Cockos does not document the `reaper-vstplugins64.ini` fields. It is widely assumed that the first hex field is a file timestamp and the second a plugin ID, but no primary source confirms that.
- Audacity, Qtractor and Zrythm were not researched within the tool-call budget.

## 4. Reading metadata cheaply without instantiating, and when instantiation is needed

### Takeaway
LV2 (TTL manifests through lilv), VST3 bundles that ship `moduleinfo.json`, and Audio Units (the Component Manager) can be listed **without running plugin code**. CLAP needs the shared library loaded and `clap_entry->init()` called, but descriptors come from the factory **without creating a plugin instance**. Channel layouts, parameters and accurate latency need an instance for VST3 and CLAP, and must always be re-read on activation.

### Cited Findings
- **VST3 `moduleinfo.json`** lives in the bundle's `Contents/Resources` folder. It was introduced in SDK 3.7.5 in `Contents` and moved to `Contents/Resources` in 3.7.8 for macOS code signing. It is optional JSON5 holding "the same information as the Module Factory, plus an optional list of compatible classes": Name, Version, Factory Info (vendor, URL, email, flags), and a Classes array (CID, category, vendor, SDKVersion, Sub Categories, snapshots) plus a Compatibility array. "the host does not need to load the component to know which classes the module provides". The SDK ships `moduleinfotool` and a `ModuleInfoLib` parser for hosts. — [VST3 dev portal: ModuleInfo-JSON](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/VST+Module+Architecture/ModuleInfo-JSON.html)
- The VST3 SDK hosting `Module` class provides `getModuleInfoPath(modulePath)`, `getSnapshots(modulePath)`, `validateBundleStructure(path, err)`, `getModulePaths()` and `create(path, err)`. — [vst3_public_sdk hosting/module.h](https://github.com/steinbergmedia/vst3_public_sdk/blob/master/source/vst/hosting/module.h)
- Ardour's cache records VST3 bus or channel counts (`n_inputs`, `n_aux_inputs`, `n_midi_inputs` …), information that is not in `moduleinfo.json`, so its scanner instantiates the component. — [Ardour vst3_scan.cc](https://github.com/Ardour/ardour/blob/master/libs/ardour/vst3_scan.cc)
- **CLAP**: `clap_entry.init()` "must be as fast as possible" for quick scanning and must not show a GUI or interact with the user. `get_factory()` is "[thread-safe]". `init`/`deinit` must not run concurrently with each other or with other CLAP calls. From CLAP 1.2.0, `init` can be called more than once, so plugins must code defensively. — [clap entry.h](https://github.com/free-audio/clap/blob/main/include/clap/entry.h)
- The `clap_plugin_descriptor` fields are `id` (reverse-URI, mandatory), `name` (mandatory), `vendor`, `url`, `manual_url`, `support_url`, `version`, `description` and a NULL-terminated `features` keyword array for host indexing. — [clap plugin.h](https://github.com/free-audio/clap/blob/main/include/clap/plugin.h)
- CLAP audio-port layout and latency need an instance: latency `get()` is `[main-thread & (being-activated | active)]`, so a plugin reports latency only once activated. — [clap latency.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/latency.h)
- **LV2** bundles are described by Turtle (TTL) manifests that lilv reads; the libraries are listed in section 1. — [lilv](https://gitlab.com/lv2/lilv)
- **AU**: `AVAudioUnitComponentManager` gets "information about the audio components without opening them". — [Apple: AVAudioUnitComponentManager](https://developer.apple.com/documentation/avfaudio/avaudiounitcomponentmanager)

### Inferences
- A two-tier scan:
  - **Tier 1, in process and safe**: lilv for LV2, `moduleinfo.json` for VST3 (when present), the Component Manager for AU. This needs no helper process and fills the browser immediately.
  - **Tier 2, out of process**: CLAP (dlopen + init + factory enumeration; optionally create and `init` a plugin and query `clap.audio-ports`), VST3 bundles without `moduleinfo.json`, and channel or latency probing for any format. Run it in `montage-plugin-scanner` with the timeout and block list from section 3.
- Even when `moduleinfo.json` exists, a full VST3 scan that reads bus info means instantiating `IComponent`. Do it lazily (the first time the user inserts the plugin), or in the helper during idle time.
- Validate on insert, not just at scan time. Any plugin's channel layout and latency can change after `activate`, so treat cached values as hints for UI filtering only.
- On macOS, running `auval` in the helper (as Logic does) is optional. A cheaper option is to instantiate in the helper and catch failures. Keep an "AU failed validation" status anyway, so the UI can say why a plugin is hidden.

### Gaps
- The ModuleInfoLib file names and API were not fetched. How many shipping plugins in 2026 actually include `moduleinfo.json` is unknown.
- No source was fetched on how lilv handles `rdfs:seeAlso` lazy loading or how much of each plugin's TTL `lilv_world_load_all` parses. Check this before relying on lilv for fast start-up with large LV2 collections.

## 5. Hosting essentials: buffers and sample rate, offline render, latency, automation, state, editor GUIs from Qt, threading

### Takeaway
Every format has the same lifecycle: configure (sample rate, maximum block size, realtime or offline) → activate → process on the audio thread → deactivate to reconfigure. Latency and port layout may change only across a deactivate/activate cycle. Editor windows are embedded by passing a native parent handle (HWND, NSView*, X11 Window) from a Qt container widget. Wayland has no CLAP embedding, so plan for XWayland or floating windows on Linux.

### Cited Findings
**Lifecycle, buffers and sample rate**
- CLAP `activate(sample_rate, min_frames, max_frames)`: "The process's sample rate will be constant and process's frame count will included in the [min, max] range … Once activated the latency and port configuration must remain constant, until deactivation". `activate` is `[main-thread & !active]`. `start_processing` and `process` are `[audio-thread & active]`. `reset()` "clears all buffers … kills all voices" and leaves parameter values unchanged. — [clap plugin.h](https://github.com/free-audio/clap/blob/main/include/clap/plugin.h)
- VST3 `ProcessSetup` holds `processMode`, `symbolicSampleSize` (`kSample32` / `kSample64`), `maxSamplesPerBlock` and the sample rate, passed in `IAudioProcessor::setupProcessing`. — [vst3 ivstaudioprocessor.h](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivstaudioprocessor.h)

**Offline (faster-than-realtime) export**
- In VST3, `kOffline` means "each process call could be faster than realtime or slower, higher quality than realtime". Switching between `kRealtime` and `kPrefetch` happens on the realtime thread, but "Switching between kRealtime (or kPrefetch) and kOffline requires that the host calls IAudioProcessor::setupProcessing". — [vst3 ivstaudioprocessor.h](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivstaudioprocessor.h)
- CLAP's render extension offers `CLAP_RENDER_REALTIME` (the default) and `CLAP_RENDER_OFFLINE` ("For processing without realtime pressure. The plugin may use more expensive algorithms"). The plugin can also report "a hard requirement to process in real-time", for example a proxy to hardware. Both calls are `[main-thread]`. — [clap ext/render.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/render.h)

**Latency and tail**
- CLAP: "The latency is only allowed to change during plugin->activate. If the plugin is activated, call host->request_restart()". — [clap ext/latency.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/latency.h)
- VST3 `IAudioProcessor` has `getLatencySamples` and `getTailSamples`, with constants for no tail and an infinite tail. — [vst3 ivstaudioprocessor.h](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/vst/ivstaudioprocessor.h)

**Parameters and automation**
- CLAP: "The host sees the plugin as an atomic entity; and acts as a controller on top of its parameters". The host reads values with `get_value()` on the main thread. Changes are sent as `CLAP_EVENT_PARAM_VALUE` events either during `process()` or during `params.flush()` when no audio is processing. User gestures are bracketed by `CLAP_EVENT_PARAM_GESTURE_BEGIN` and `CLAP_EVENT_PARAM_GESTURE_END`. Loading a preset may call `host_params->rescan()` and `host_latency->changed()`. Flags include `CLAP_PARAM_IS_AUTOMATABLE` and per-note and per-key variants. — [clap ext/params.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/params.h)

**State**
- CLAP's state extension saves and loads "both parameter values and non-parameter state" through streams, on `[main-thread]`. It is used "to persist a plugin's state between project reloads, when duplicating and copying plugin instances, and for host-side preset management". `CLAP_EXT_STATE_CONTEXT` tells the plugin whether a save is for a preset, a duplicate or a project. The plugin calls `mark_dirty()` when its state changes. — [clap ext/state.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/state.h)

**Editor GUIs**
- CLAP GUI sequence: `is_api_supported` → `create` → (floating: `set_transient`, `suggest_title`) or (embedded: `set_scale`, `can_resize`, `set_size` or `get_size`, `set_parent`) → `show`/`hide` → `destroy`. "The Embedding protocol is by far the most common, supported by all hosts to date". The window APIs are:
  - `win32` (physical pixels; embed via `SetParent`)
  - `cocoa` (logical size; "don't call clap_plugin_gui->set_scale()")
  - `x11` (physical pixels; embed via XEmbed)
  - `wayland`: "embed is currently not supported, use floating windows".
  - Plugin-initiated resizes go through `host_gui->request_resize()`. User drags go through `adjust_size()` then `set_size()`. — [clap ext/gui.h](https://github.com/free-audio/clap/blob/main/include/clap/ext/gui.h)
- The VST3 `IPlugView::attached(void* parent, FIDString type)` platform types are `kPlatformTypeHWND`, `kPlatformTypeNSView`, `kPlatformTypeX11EmbedWindowID` ("X11 Window supporting XEmbed"), `kPlatformTypeWaylandSurfaceID` (a `wl_surface` pointer), plus HIView and UIView. On macOS coordinates are logical units; on Windows and Linux X11 they are physical pixels. When the host resizes: if `canResize()` is true, it calls `checkSizeConstraint()`, resizes the window, then calls `onSize()`. Linux hosts must supply a run loop (`IRunLoop`, with `IEventHandler` and `ITimerHandler`). — [vst3 gui/iplugview.h](https://github.com/steinbergmedia/vst3_pluginterfaces/blob/master/gui/iplugview.h)
- In Qt, `QWindow::fromWinId(WId)` wraps a native handle as a "foreign window". `WId` is `NSView*` on macOS, `HWND` on Windows and `xcb_window_t` on X11. "Qt does not take (exclusive) ownership of the native window handle … the application is responsible for keeping the native window alive". For Qt Widgets apps, `QWidget::createWindowContainer()` makes a widget that layouts manage. Plain `QWindow::setParent` "leaves it up to the application developer to handle positioning, resizing" and is discouraged when an alternative exists. — [Qt 6.10: Window Embedding example](https://doc.qt.io/qt-6.10/qtdoc-demos-windowembedding-example.html)
- Apple, for out-of-process AUs: "don't assume callbacks happen on the main thread. When updating the user interface, asynchronously dispatch to the main queue". Custom free-floating dialogs should be added "as a subwindow to the main window". — [Apple: Debugging Out-of-Process Audio Units](https://developer.apple.com/documentation/audiotoolbox/debugging-out-of-process-audio-units-on-apple-silicon)

### Inferences
**Engine integration for an NLE**
- Wrap every format behind one `IAudioPluginInstance` interface (`activate(sr, maxBlock, offline)`, `process(AudioBlock&, ParamEvents&)`, `latencySamples()`, `tailSamples()`, `saveState()`/`loadState()`, `createEditor(QWidget* parent)`) so the timeline mixer treats it like the existing EQ, compressor and limiter effects.
- The NLE's export path is the main reason to support offline mode. Before an export, deactivate each instance, switch it to offline (CLAP `render.set(OFFLINE)`; VST3 `setupProcessing(kOffline)`), reactivate, render, then switch back. `montage-cli` headless renders should always use offline mode. Plugins that report a hard realtime requirement must be rendered in realtime, or the export must warn.
- **Latency compensation**: sum the reported latency per track's effect chain and shift that track's audio earlier by the difference to the slowest track (or, simpler and common in NLEs, compensate each clip's audio against picture so A/V sync holds). On export, pre-roll the latency, discard the first N output samples, and render `tailSamples()` past the out point or the sequence end, so reverb and delay tails are not cut off.

**Keyframes to automation**
- Montage already has linear, hold and smooth keyframes. For each audio block, evaluate the parameter curve and emit events: CLAP `PARAM_VALUE` events carry a sample `time` offset inside the block, and VST3 uses `IParameterChanges` point queues with sample offsets.
- For linear segments, one point per block boundary is enough. Smooth curves should be sub-sampled (for example every 32–64 samples; tune this). Hold keyframes emit one event at the keyframe sample.
- Convert normalised values: VST3 parameters are 0..1, while CLAP parameters use the plugin's own min/max from `get_info`.
- When the user turns a knob in the plugin's GUI, record the gesture begin/end as keyframes, reusing Montage's "drags merge into one undo step" logic.

**State in `.montage` JSON**
- Store `{format, id (CID/clap id/LV2 URI/AU triple), name, vendor, version, state: base64(blob), params: {id: value}}`.
- For VST3, save both `IComponent::getState` and `IEditController::getState` (two blobs). On load, restore with `setComponentState`.
- For LV2, use lilv's state API. lilv 0.28.0 added `lilv_state_get_bundle_path` and fixed state-saving data-loss bugs, so require 0.28 or later.
- Keep the plain parameter map as a fallback, so a project still opens, without the plugin's private state, when the plugin is a different version or missing. Show a "missing plugin" placeholder that keeps the blob and passes the audio through unprocessed, as relinking already does for media.

**Editors in Qt**
- Create a `QWidget` (or a top-level `QWindow`) as the container and take its native handle with `winId()` (which forces native-window creation). Pass it to CLAP `set_parent` / VST3 `attached`. On macOS, `winId()` is an `NSView*`.
- Use `QWindow::fromWinId` + `createWindowContainer` only when the plugin hands *you* a window, or for cross-process editors.
- Honour logical versus physical sizes: on Windows and X11 multiply by `devicePixelRatio()`; on Cocoa do not.
- On Linux:
  - Run plugin editors under the `xcb` platform plugin (XWayland). One option is to start Montage with `QT_QPA_PLATFORM=xcb` when plugin editors are enabled; otherwise fall back to CLAP floating windows.
  - Implement VST3 `IRunLoop` and CLAP `posix-fd-support` / `timer-support` with `QSocketNotifier` and `QTimer` on the GUI thread.

**Threading rules**
- `[main-thread]` means the Qt GUI thread.
- `[audio-thread]` means Montage's audio callback or export worker. Never allocate, lock or call Qt there; pass parameter changes through lock-free single-producer/single-consumer queues.
- Process exactly one audio thread per instance at a time.
- For export, use a separate instance or pause playback, because the same instance cannot be activated for realtime and offline at once.
- Implement CLAP's `thread-check` host extension so plugins can assert correctly.

### Gaps
- VST3 `IComponentHandler::restartComponent` flags (for example a latency change) were not fetched from the headers. Confirm the names in `ivsteditcontroller.h`.
- Whether VST3 Wayland embedding (`kPlatformTypeWaylandSurfaceID`) works in practice in 2026 hosts and plugins, and how it could pair with Qt's Wayland platform, was not researched.
- AUv3 hosting specifics (`AUAudioUnit` and `requestViewController` for the editor, out-of-process rendering costs) were not researched in depth. Only the out-of-process architecture and threading warning are sourced.
- No benchmark data was found on how often to sub-sample automation for plugin parameters in NLEs.

## 6. Minimal viable approach: implementation order and keeping the build optional in CMake

### Takeaway
With VST3 now MIT, the best coverage-to-effort order for a video editor is **VST3 first** (the format that Premiere, Resolve and Ableton all scan, on Windows and macOS), then **CLAP** (a small C ABI with an MIT, Qt 6 reference host to borrow from), then **AU** on macOS (needed only for AU-only plugins, and Apple already isolates them out of process), then **LV2** on Linux. Each format sits behind its own CMake option that defaults to OFF where the SDK is not found, so CI on all three OSes stays green.

### Cited Findings
- The VST3 SDK host-side pieces are MIT as of 3.8.0 (October 2025). — [vst3sdk LICENSE.txt](https://github.com/steinbergmedia/vst3sdk/blob/master/LICENSE.txt); [Steinberg press release](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf)
- Premiere's plugin manager treats VST3 (alongside legacy VST and AU) as the main format. — [ProVideo Coalition](https://www.provideocoalition.com/tool-tip-tuesday-for-adobe-premiere-pro-tame-your-audio-effects/). Ableton's guidance revolves around "Use VST3 Plug-In System Folders" and "Use Audio Units". — [Sonnox support](https://support.sonnox.com/support/solutions/articles/22000201335-re-scanning-plug-ins-in-ableton-live). Resolve release notes deal with VST3 scanning. — [Newsshooter](https://www.newsshooter.com/2021/11/18/blackmagic-davinci-resolve-17-4-2/)
- CLAP is a plain C header API (stable 1.x ABI, currently 1.2.10). — [clap version.h](https://github.com/free-audio/clap/blob/main/include/clap/version.h). An MIT Qt 6 + CMake reference host exists. — [clap-host](https://github.com/free-audio/clap-host/blob/main/README.md)
- lilv depends on zix, serd, sord, lv2 and sratom, and builds with Meson, not CMake. — [lilv meson.build](https://gitlab.com/lv2/lilv/-/blob/main/meson.build)
- Final Cut Pro and Logic accept AUv2 and AUv3 only, so Mac users' plugin libraries usually include AU builds. — [Apple Support 102082](https://support.apple.com/en-US/102082)
- Montage's top-level `CMakeLists.txt` currently has only two options, `MONTAGE_BUILD_APP` and `MONTAGE_BUILD_TESTS` (checked locally in `/home/user/montage/CMakeLists.txt`).

### Inferences
**Phased plan**
1. **Phase A, shared plumbing (format-neutral)**:
   - the `PluginPaths` service
   - the `PluginCatalog` cache (JSON or SQLite)
   - the `montage-plugin-scanner` helper with the QProcess pool, timeout and write-ahead block list
   - the Preferences > Audio Plug-ins UI
   - the `IAudioPluginInstance` interface wired into the existing per-clip and per-track audio effect chain
   - project-file serialisation with a missing-plugin placeholder.
   - Unit-test it with a fake format, so it needs no SDK in CI.
2. **Phase B, CLAP**. It is the fastest way to prove the plumbing end to end: header-only, a C ABI, MIT, and `clap-host` to crib from. Test with free CLAP plugins such as the `clap-plugins` examples, Surge XT or Dexed; availability is assumed, not verified here.
3. **Phase C, VST3**. It gives the largest catalogue on Windows and macOS. Vendor `vst3_pluginterfaces`, `vst3_base` and `vst3_public_sdk/source/vst/hosting` as a git submodule or FetchContent pinned to a v3.8.x tag. Compile only the hosting sources the app needs, rather than the SDK's full CMake build (which also builds examples, VSTGUI and the validator). Use `Module::getModuleInfoPath` for the fast scan path.
4. **Phase D, AU (macOS)**: AudioToolbox `AudioComponentFindNext`/`AudioComponentInstanceNew` for AUv2, and AVFAudio for AUv3 and its editors, behind `if(APPLE)`.
5. **Phase E, LV2 (Linux first)**. Use pkg-config to find a system lilv (≥ 0.28) and suil. Do not vendor lilv into the CMake build, because it is Meson-based.

**CMake shape** (keeps CI green; option names are suggestions):
```cmake
option(MONTAGE_PLUGINS        "Audio plugin hosting"                 ON)
option(MONTAGE_PLUGINS_CLAP   "CLAP hosting (header-only, fetched)" ON)
option(MONTAGE_PLUGINS_VST3   "VST3 hosting (MIT SDK, fetched)"     ON)
option(MONTAGE_PLUGINS_LV2    "LV2 hosting via system lilv/suil"    OFF)  # auto-ON on Linux if found
option(MONTAGE_PLUGINS_AU     "Audio Unit hosting (macOS only)"     ${APPLE})

if(MONTAGE_PLUGINS_CLAP)
  FetchContent_Declare(clap GIT_REPOSITORY https://github.com/free-audio/clap GIT_TAG 1.2.10)
  FetchContent_MakeAvailable(clap)              # interface target clap
  target_compile_definitions(montage_audio PRIVATE MONTAGE_HAS_CLAP=1)
endif()
if(MONTAGE_PLUGINS_LV2)
  find_package(PkgConfig)
  pkg_check_modules(LILV IMPORTED_TARGET lilv-0>=0.28)
  if(NOT LILV_FOUND)
    message(WARNING "lilv not found; LV2 disabled")
    set(MONTAGE_PLUGINS_LV2 OFF)
  endif()
endif()
if(MONTAGE_PLUGINS_AU AND NOT APPLE)
  set(MONTAGE_PLUGINS_AU OFF)
endif()
```
- Each backend lives in `src/audio/plugins/<format>/` and registers itself with a `PluginFormatRegistry` at start-up. The core compiles and its tests pass with every backend OFF.
- Add one CI matrix entry with all plugin options OFF, so the fallback keeps building.
- Ship a tiny MIT test plugin (for example a CLAP gain plugin built in-tree under `tests/`). The scanner, state round-trip and offline-render tests can then run on all three OSes without third-party binaries. Add a deliberately crashing or hanging test plugin to prove the timeout and block list work.
- Packaging: install `montage-plugin-scanner` beside the app (inside `Contents/MacOS` on macOS, so it inherits the bundle's code signature and entitlements). On macOS the hardened runtime needs the "disable library validation" entitlement, or third-party plugin dylibs will not load. This is general knowledge and not verified in this research.
- Defer: runtime sandboxing (Bitwig-style), MIDI instruments, sidechain buses, and VST3 MIDI 2.0 features. An NLE needs audio effects first.

### Gaps
- No 2026 market-share data was found on how many commercial plugins ship CLAP versus VST3 versus AU. The VST3-first ranking rests on what the market leaders expose, not on counts.
- The VST3 SDK's own CMake option names and the exact list of hosting source files (per-platform module loaders) were not verified. Confirm them against the v3.8.x tag before writing the CMake.
- The macOS hardened-runtime entitlement for loading third-party plugin code was not sourced in this research. Confirm it in Apple's entitlement documentation.
- Whether vcpkg or Conan package lilv 0.28 for Windows and macOS (avoiding Meson in CI) was not checked.
