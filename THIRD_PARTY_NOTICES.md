# Third-party notices

Montage is free software under the GNU General Public License, version 3 or (at your option) any later version
([LICENSE](LICENSE)), with the additional permission in [LICENSE-EXCEPTION](LICENSE-EXCEPTION). It includes, or is
built with, the work of others, listed here with their licences.

Montage's packages (the macOS disk image, the Windows installer and zip) also carry a `licenses` folder: the licence
files of every library the package bundles, an index of them (`BUNDLED.txt`, with each library's version and where its
source comes from) and the exact FFmpeg configuration. In the app, Help › Licences… shows this file and opens that
folder. The source code for each release, including every bundled library's, is attached to the release on GitHub.

## Code and data ported into Montage's source

| Component | Where in Montage | Licence | Notice |
|---|---|---|---|
| [lensfun](https://github.com/lensfun/lensfun) library: guessing a lens's focal lengths and aperture from its name, and its Hermite interpolation | `src/core/LensProfile.cpp` | LGPL-3.0 | Copyright (C) 2007 Andrew Zabolotny and the lensfun contributors. [LICENSES/LGPL-3.0.txt](LICENSES/LGPL-3.0.txt) |
| [lensfun](https://github.com/lensfun/lensfun) database: lens profiles used in a test fixture | `tests/data/lensfun/test-lenses.xml` | CC BY-SA 3.0 | The lensfun database contributors. Not part of the packages |
| [blackmagic-misc](https://github.com/smunaut/blackmagic-misc): the Speed Editor's HID protocol and authentication | `src/control/SpeedEditor.cpp`, `.h` | Apache-2.0 | Copyright (C) 2021 Sylvain Munaut. Translated to C++. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [CommandPost](https://github.com/CommandPost/CommandPost) `hs.blackmagic`: the Speed Editor's key and light codes | `src/control/SpeedEditor.cpp`, `.h` | MIT | [LICENSES/MIT-CommandPost.txt](LICENSES/MIT-CommandPost.txt) |
| [misaki](https://github.com/hexgrad/misaki): English grapheme-to-phoneme rules | `src/core/G2p.cpp`, `.h` | Apache-2.0 | Copyright the misaki authors (hexgrad). Ported to C++. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [EdgeTAM](https://github.com/facebookresearch/EdgeTAM): learned constants of the memory bank | `src/media/EdgeTamConstants.inc` | Apache-2.0 | Copyright Meta Platforms, Inc. and affiliates. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [pyaaf2](https://github.com/markreidvfx/pyaaf2) 1.7.1: the AAF MetaDictionary | `src/core/AafMetaDictionary.inc` | MIT | [LICENSES/MIT-pyaaf2.txt](LICENSES/MIT-pyaaf2.txt) |
| [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) 4.26: the network graph (without weights) | `src/media/rife-4.26.onnx` | MIT | [LICENSES/MIT-Practical-RIFE.txt](LICENSES/MIT-Practical-RIFE.txt) |
| [Spleeter](https://github.com/deezer/spleeter): the separation network graphs (without weights) | `src/media/spleeter-*stems.onnx` | MIT | [LICENSES/MIT-spleeter.txt](LICENSES/MIT-spleeter.txt) |
| [SCOWL](http://wordlist.aspell.net/) word lists | `src/resources/dict/` | SCOWL licence (permissive) | Copyright 2000-2018 Kevin Atkinson; the notice is kept at the top of each list |

## Libraries compiled into Montage

| Component | Licence | Licence file |
|---|---|---|
| [whisper.cpp and ggml](https://github.com/ggml-org/whisper.cpp) | MIT | the source's `LICENSE` (copied into the packages) |
| [RNNoise](https://github.com/xiph/rnnoise) and its model | BSD-3-Clause | the source's `COPYING` (copied into the packages) |
| [Lua](https://www.lua.org) 5.4 | MIT | [LICENSES/MIT-Lua.txt](LICENSES/MIT-Lua.txt) |
| [ThorVG](https://github.com/thorvg/thorvg), with RapidJSON (MIT) and JerryScript (Apache-2.0) | MIT, MIT, Apache-2.0 | the source's licence files (copied into the packages) |
| [c2pa-rs](https://github.com/contentauth/c2pa-rs) (`c2pa-c-ffi`) and the Rust crates it uses | MIT or Apache-2.0; each crate's own | `c2pa` in the packages' `licenses` folder, one folder per crate |
| [VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) (interfaces and host helpers) | MIT | [third_party/vst3sdk/LICENSE.txt](third_party/vst3sdk/LICENSE.txt) |
| [CLAP](https://github.com/free-audio/clap) headers | MIT | [third_party/clap/LICENSE](third_party/clap/LICENSE) |
| [OpenFX](https://github.com/AcademySoftwareFoundation/openfx) headers | BSD-3-Clause | [third_party/openfx/LICENSE](third_party/openfx/LICENSE) |
| Blackmagic DeckLink SDK 12.0 headers | Blackmagic Design's header licence | [third_party/decklink/LICENSE](third_party/decklink/LICENSE) |
| NDI SDK 6 headers | MIT (these files only) | [third_party/ndi/LICENSE](third_party/ndi/LICENSE) |

## Libraries bundled in the packages

The packages bundle these shared libraries and their dependencies. Each package's `licenses/BUNDLED.txt` lists every
one with its exact version, licence and source.

- [FFmpeg](https://ffmpeg.org), built by `scripts/build-ffmpeg.sh` with `--enable-gpl` (and never `--enable-nonfree`),
  which makes this build GPL-2.0-or-later; with [x264](https://www.videolan.org/developers/x264.html) (GPL-2.0-or-later),
  [x265](https://www.x265.org) (GPL-2.0-or-later), libvpx (BSD-3-Clause), SVT-AV1 (BSD-3-Clause-Clear), dav1d
  (BSD-2-Clause), Opus (BSD-3-Clause), OpenJPEG (BSD-2-Clause), SRT (MPL-2.0) and librist (BSD-2-Clause).
- [Qt](https://www.qt.io) 6 (LGPL-3.0): Core, Gui, Widgets, Network, Concurrent, Xml and Multimedia. You may replace
  the bundled Qt libraries with your own build of the same version.
- [OpenColorIO](https://opencolorio.org) (BSD-3-Clause), [ONNX Runtime](https://onnxruntime.ai) (MIT, with its own
  third-party notices), [LibRaw](https://www.libraw.org) (LGPL-2.1 or CDDL-1.0), [lilv](https://drobilla.net/software/lilv)
  (ISC), [libmysofa](https://github.com/hoene/libmysofa) (BSD-3-Clause) and [OpenJPEG](https://www.openjpeg.org)
  (BSD-2-Clause).

## Not included

Montage does not include the Blackmagic Desktop Video driver, the NDI Runtime or the Blackmagic RAW SDK; it uses them
when they are installed. NDI® is a registered trademark of Vizrt NDI AB ([ndi.video](https://ndi.video)). VST® is a
registered trademark of Steinberg Media Technologies GmbH.

AI models are not included either: Montage downloads each one when a feature first needs it, and names the model and
its licence when it asks to download it.
