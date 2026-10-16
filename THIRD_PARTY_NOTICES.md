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
| [Hammerspoon](https://github.com/Hammerspoon/hammerspoon) `hs.tangent` and [CommandPost](https://github.com/CommandPost/CommandPost): the Tangent Hub (TUBE) message ids and fields | `src/control/Tangent.cpp`, `.h` | MIT | [LICENSES/MIT-Hammerspoon.txt](LICENSES/MIT-Hammerspoon.txt), [LICENSES/MIT-CommandPost.txt](LICENSES/MIT-CommandPost.txt) |
| [misaki](https://github.com/hexgrad/misaki): English grapheme-to-phoneme rules | `src/core/G2p.cpp`, `.h` | Apache-2.0 | Copyright the misaki authors (hexgrad). Ported to C++. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [EdgeTAM](https://github.com/facebookresearch/EdgeTAM): learned constants of the memory bank | `src/media/EdgeTamConstants.inc` | Apache-2.0 | Copyright Meta Platforms, Inc. and affiliates. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [pyaaf2](https://github.com/markreidvfx/pyaaf2) 1.7.1: the AAF MetaDictionary | `src/core/AafMetaDictionary.inc` | MIT | [LICENSES/MIT-pyaaf2.txt](LICENSES/MIT-pyaaf2.txt) |
| [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) 4.26: the network graph (without weights) | `src/media/rife-4.26.onnx` | MIT | [LICENSES/MIT-Practical-RIFE.txt](LICENSES/MIT-Practical-RIFE.txt) |
| [Spleeter](https://github.com/deezer/spleeter): the separation network graphs (without weights) | `src/media/spleeter-*stems.onnx` | MIT | [LICENSES/MIT-spleeter.txt](LICENSES/MIT-spleeter.txt) |
| [Bandit v2](https://github.com/kwatcharasupat/bandit-v2): the dialogue, music and effects network graph (without weights), and the script that builds it, transcribed from the authors' code | `scripts/gen-bandit-graph.py`, `src/media/bandit-v2-eng.onnx` | Apache-2.0 | Copyright Karn N. Watcharasupat, Chih-Wei Wu and Alexander Lerch. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [MuseTalk](https://github.com/TMElyralab/MuseTalk) 1.5, with the autoencoder of [sd-vae-ft-mse](https://huggingface.co/stabilityai/sd-vae-ft-mse) and the encoder of [Whisper](https://github.com/openai/whisper): the lip sync network graphs (without weights), and the script that builds them, transcribed from the authors' code, the [diffusers](https://github.com/huggingface/diffusers) and [transformers](https://github.com/huggingface/transformers) model classes they use and [librosa](https://librosa.org)'s mel filter bank; and MuseTalk's sound window, face crop and blending | `scripts/gen-lipsync-graphs.py`, `src/media/lipsync-*.onnx`, `src/media/LipSync.cpp` | MIT; diffusers and transformers Apache-2.0; librosa ISC | Copyright 2024 Tencent Music Entertainment Group; 2022 OpenAI; The HuggingFace Team; 2013–2026 librosa development team. Modified (rewritten as ONNX graph construction and C++). [LICENSES/MIT-MuseTalk.txt](LICENSES/MIT-MuseTalk.txt), [LICENSES/MIT-whisper.txt](LICENSES/MIT-whisper.txt), [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt), [LICENSES/ISC-librosa.txt](LICENSES/ISC-librosa.txt) |
| [SCOWL](http://wordlist.aspell.net/) word lists | `src/resources/dict/` | SCOWL licence (permissive) | Copyright 2000-2018 Kevin Atkinson; the notice is kept at the top of each list |

## Libraries compiled into Montage

| Component | Licence | Licence file |
|---|---|---|
| [whisper.cpp and ggml](https://github.com/ggml-org/whisper.cpp) | MIT | the source's `LICENSE` (copied into the packages) |
| [RNNoise](https://github.com/xiph/rnnoise) and its model | BSD-3-Clause | the source's `COPYING` (copied into the packages) |
| [Lua](https://www.lua.org) 5.4 | MIT | [LICENSES/MIT-Lua.txt](LICENSES/MIT-Lua.txt) |
| [ThorVG](https://github.com/thorvg/thorvg), with RapidJSON (MIT) and JerryScript (Apache-2.0) | MIT, MIT, Apache-2.0 | the source's licence files (copied into the packages) |
| [OpenEXR](https://github.com/AcademySoftwareFoundation/openexr) 3.5 and [Imath](https://github.com/AcademySoftwareFoundation/Imath), with the [OpenJPH](https://github.com/aous72/OpenJPH) (BSD-2-Clause), [libdeflate](https://github.com/ebiggers/libdeflate) (MIT) and [zstd](https://github.com/facebook/zstd) (BSD-3-Clause) OpenEXR bundles | BSD-3-Clause, BSD-3-Clause, BSD-2-Clause, MIT, BSD-3-Clause | the sources' licence files (copied into the packages) |
| [bmx](https://github.com/ebu/bmx) 1.7, the EBU's MXF library, with its [libMXF and libMXF++](https://github.com/ebu/bmx/tree/main/deps) and [uriparser](https://github.com/uriparser/uriparser) 1.0, and the version stamp each compiles in from [cmake-git-version-tracking](https://github.com/andrew-hardin/cmake-git-version-tracking) | BSD-3-Clause (uriparser's library: BSD-3-Clause; cmake-git-version-tracking: MIT) | the sources' licence files (copied into the packages) |
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
its licence when it asks to download it. One is not under a software licence: Bandit v2's weights (dialogue, music and
effects) are CC BY-SA 4.0, so they are downloaded from the authors' Zenodo record as they are and never repackaged.
