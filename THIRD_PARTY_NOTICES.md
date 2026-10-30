# Third-party notices

Montage is free software under the GNU General Public License, version 3 or (at your option) any later version
([LICENSE](LICENSE)), with the additional permission in [LICENSE-EXCEPTION](LICENSE-EXCEPTION). It includes, or is
built with, the work of others, listed here with their licences.

Montage's packages (the macOS disk image, the Windows installer and zip) also carry a `licenses` folder: the licence
files of every library the package bundles, an index of them (`BUNDLED.txt`, with each library's version and where its
source comes from) and the exact FFmpeg configuration. In the app, Help › Licences… shows this file and opens that
folder. The source code for each release, including every bundled library's, is attached to the release on GitHub.

## Licensed technology

This product includes Gain Map technology under license by Adobe. Montage reads and writes HDR stills with gain maps
(`src/media/GainMap.cpp`: JPEG with a gain map, its metadata as Adobe's gain map XMP and as ISO 21496-1) under the
licence Adobe offers with its Gain Map specification; its terms are Adobe's, as published with the specification.

## Code and data ported into Montage's source

| Component | Where in Montage | Licence | Notice |
|---|---|---|---|
| [lensfun](https://github.com/lensfun/lensfun) library: guessing a lens's focal lengths and aperture from its name, and its Hermite interpolation | `src/core/LensProfile.cpp` | LGPL-3.0 | Copyright (C) 2007 Andrew Zabolotny and the lensfun contributors. [LICENSES/LGPL-3.0.txt](LICENSES/LGPL-3.0.txt) |
| [lensfun](https://github.com/lensfun/lensfun) database: lens profiles used in a test fixture | `tests/data/lensfun/test-lenses.xml` | CC BY-SA 3.0 | The lensfun database contributors. Not part of the packages |
| [llama2.c](https://github.com/karpathy/llama2.c) stories260K: a tiny language model used to test the language model plumbing (as llama.cpp's own tests do) | `tests/data/llm/stories260K.gguf` | MIT | Copyright (c) 2023 Andrej. [tests/data/llm/LICENSE](tests/data/llm/LICENSE). Not part of the packages |
| [blackmagic-misc](https://github.com/smunaut/blackmagic-misc): the Speed Editor's HID protocol and authentication | `src/control/SpeedEditor.cpp`, `.h` | Apache-2.0 | Copyright (C) 2021 Sylvain Munaut. Translated to C++. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [CommandPost](https://github.com/CommandPost/CommandPost) `hs.blackmagic`: the Speed Editor's key and light codes | `src/control/SpeedEditor.cpp`, `.h` | MIT | [LICENSES/MIT-CommandPost.txt](LICENSES/MIT-CommandPost.txt) |
| [Hammerspoon](https://github.com/Hammerspoon/hammerspoon) `hs.tangent` and [CommandPost](https://github.com/CommandPost/CommandPost): the Tangent Hub (TUBE) message ids and fields | `src/control/Tangent.cpp`, `.h` | MIT | [LICENSES/MIT-Hammerspoon.txt](LICENSES/MIT-Hammerspoon.txt), [LICENSES/MIT-CommandPost.txt](LICENSES/MIT-CommandPost.txt) |
| [misaki](https://github.com/hexgrad/misaki): English grapheme-to-phoneme rules | `src/core/G2p.cpp`, `.h` | Apache-2.0 | Copyright the misaki authors (hexgrad). Ported to C++. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [EdgeTAM](https://github.com/facebookresearch/EdgeTAM): learned constants of the memory bank | `src/media/EdgeTamConstants.inc` | Apache-2.0 | Copyright Meta Platforms, Inc. and affiliates. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [pyaaf2](https://github.com/markreidvfx/pyaaf2) 1.7.1: the AAF MetaDictionary | `src/core/AafMetaDictionary.inc` | MIT | [LICENSES/MIT-pyaaf2.txt](LICENSES/MIT-pyaaf2.txt) |
| [pyavb](https://github.com/markreidvfx/pyavb) 1.4.0: its reader of Media Composer bins (the file's object table and the layout of each class read), rewritten in C++ | `src/core/AvidBin.cpp`, `.h` | MIT | Copyright (c) 2022 Mark Reid. [LICENSES/MIT-pyavb.txt](LICENSES/MIT-pyavb.txt) |
| [pyavb](https://github.com/markreidvfx/pyavb): its sample bin, used in a test fixture | `tests/data/avb/test_file_01.avb` | MIT | Copyright (c) 2022 Mark Reid. [LICENSES/MIT-pyavb.txt](LICENSES/MIT-pyavb.txt). Not part of the packages |
| [Oswald](https://github.com/googlefonts/OswaldFont): the variable font (weight axis 200 to 700, with named instances) from Google Fonts, used in a test fixture | `tests/data/fonts/Oswald-wght.ttf` | SIL OFL 1.1 | Copyright 2016 The Oswald Project Authors. [LICENSES/OFL-1.1-Oswald.txt](LICENSES/OFL-1.1-Oswald.txt). Not part of the packages |
| [Practical-RIFE](https://github.com/hzwer/Practical-RIFE) 4.26: the network graph (without weights) | `src/media/rife-4.26.onnx` | MIT | [LICENSES/MIT-Practical-RIFE.txt](LICENSES/MIT-Practical-RIFE.txt) |
| [Spleeter](https://github.com/deezer/spleeter): the separation network graphs (without weights) | `src/media/spleeter-*stems.onnx` | MIT | [LICENSES/MIT-spleeter.txt](LICENSES/MIT-spleeter.txt) |
| [Bandit v2](https://github.com/kwatcharasupat/bandit-v2): the dialogue, music and effects network graph (without weights), and the script that builds it, transcribed from the authors' code | `scripts/gen-bandit-graph.py`, `src/media/bandit-v2-eng.onnx` | Apache-2.0 | Copyright Karn N. Watcharasupat, Chih-Wei Wu and Alexander Lerch. [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [AP-BWE](https://github.com/yxlu-0102/AP-BWE): the speech bandwidth extension network graph (without weights), and the script that builds it, transcribed from the authors' code | `scripts/gen-apbwe-graph.py`, `src/media/apbwe.onnx` | MIT | Copyright (c) 2023 Ye-Xin Lu. Modified (rewritten as ONNX graph construction). [LICENSES/MIT-AP-BWE.txt](LICENSES/MIT-AP-BWE.txt) |
| [Video Depth Anything](https://github.com/DepthAnything/Video-Depth-Anything) Small, with its [DINOv2](https://github.com/facebookresearch/dinov2) encoder and the temporal attention of [AnimateDiff](https://github.com/guoyww/AnimateDiff): the steady video depth network graphs (without weights), and the script that builds them, transcribed from the authors' code; and its windowed inference (overlap, key frames, scale and shift alignment) | `scripts/gen-vda-graph.py`, `src/media/vda-small-*.onnx`, `src/media/VideoDepth.cpp` | Apache-2.0 | Copyright 2025 ByteDance; Meta Platforms, Inc. and affiliates; the AnimateDiff authors. Modified (rewritten as ONNX graph construction and C++). [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [MuseTalk](https://github.com/TMElyralab/MuseTalk) 1.5, with the autoencoder of [sd-vae-ft-mse](https://huggingface.co/stabilityai/sd-vae-ft-mse) and the encoder of [Whisper](https://github.com/openai/whisper): the lip sync network graphs (without weights), and the script that builds them, transcribed from the authors' code, the [diffusers](https://github.com/huggingface/diffusers) and [transformers](https://github.com/huggingface/transformers) model classes they use and [librosa](https://librosa.org)'s mel filter bank; and MuseTalk's sound window, face crop and blending | `scripts/gen-lipsync-graphs.py`, `src/media/lipsync-*.onnx`, `src/media/LipSync.cpp` | MIT; diffusers and transformers Apache-2.0; librosa ISC | Copyright 2024 Tencent Music Entertainment Group; 2022 OpenAI; The HuggingFace Team; 2013–2026 librosa development team. Modified (rewritten as ONNX graph construction and C++). [LICENSES/MIT-MuseTalk.txt](LICENSES/MIT-MuseTalk.txt), [LICENSES/MIT-whisper.txt](LICENSES/MIT-whisper.txt), [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt), [LICENSES/ISC-librosa.txt](LICENSES/ISC-librosa.txt) |
| [Liberation Sans](https://github.com/liberationfonts/liberation-fonts) 2.1.5 (Regular): the font a DCP's subtitles are drawn in, carried in its timed text track file | `src/resources/fonts/LiberationSans-Regular.ttf` | SIL OFL 1.1 | Copyright (c) 2010 Google Corporation with Reserved Font Arimo, Tinos and Cousine; Copyright (c) 2012 Red Hat, Inc. with Reserved Font Name Liberation. Unmodified. [LICENSES/OFL-1.1-Liberation.txt](LICENSES/OFL-1.1-Liberation.txt) |
| [libomtnet](https://github.com/openmediatransport/libomtnet): the Open Media Transport protocol as it implements it (frames, commands, the sender's and receiver's exchanges, names, DNS-SD and the discovery server's XML), rewritten in C++ | `src/media/Omt.cpp`, `.h`, `src/media/OmtDiscovery.cpp` | MIT | Copyright (c) 2025 Open Media Transport Contributors. [LICENSES/MIT-libomtnet.txt](LICENSES/MIT-libomtnet.txt) |
| [spz](https://github.com/nianticlabs/spz): the SPZ file format for Gaussian splats as its library reads and writes it (the header, the packing of each attribute, the smallest-three rotations and the harmonics' quantisation), rewritten in C++ | `src/media/Splats.cpp` | MIT | Copyright (c) 2024 Niantic Labs; 2025 Adobe Inc. [LICENSES/MIT-spz.txt](LICENSES/MIT-spz.txt) |
| [libiamf](https://github.com/AOMediaCodec/libiamf) 1.1.0: the reference renderer's ambisonic decoders to stereo (coefficients) | `src/render/Iamf.cpp` | BSD-3-Clause-Clear | Copyright (c) 2022, Alliance for Open Media. [LICENSES/BSD-3-Clause-Clear-libiamf.txt](LICENSES/BSD-3-Clause-Clear-libiamf.txt) |
| [Universal Category System](https://universalcategorysystem.com) v8.2.1: its categories, subcategories, CatIDs, explanations and synonyms, and the script that converts its list | `scripts/gen-ucs.py`, `src/core/UcsCategories.inc` | Public domain | A public domain initiative by Tim Nielsen, Justin Drury and Kai Paquin |
| [OPUS-MT](https://github.com/Helsinki-NLP/Opus-MT) models: first machine drafts of the interface's translations, marked unfinished until a translator checks them | `translations/montage_*.ts` | CC-BY-4.0 (the models) | Jörg Tiedemann and Santhosh Thottingal, OPUS-MT, University of Helsinki. Drafts made with `montage-cli draft-interface` |
| [SCOWL](http://wordlist.aspell.net/) word lists | `src/resources/dict/` | SCOWL licence (permissive) | Copyright 2000-2018 Kevin Atkinson; the notice is kept at the top of each list |

## Libraries compiled into Montage

| Component | Licence | Licence file |
|---|---|---|
| [whisper.cpp and ggml](https://github.com/ggml-org/whisper.cpp) | MIT | the source's `LICENSE` (copied into the packages) |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) (on whisper.cpp's ggml) | MIT | the source's `LICENSE` (copied into the packages) |
| [RNNoise](https://github.com/xiph/rnnoise) and its model | BSD-3-Clause | the source's `COPYING` (copied into the packages) |
| [SQLite](https://www.sqlite.org) 3.50 (with FTS5), for the sound library's index | Public domain | none needed ([sqlite.org/copyright.html](https://www.sqlite.org/copyright.html)) |
| [Lua](https://www.lua.org) 5.4 | MIT | [LICENSES/MIT-Lua.txt](LICENSES/MIT-Lua.txt) |
| [libvmx](https://github.com/openmediatransport/libvmx) (commit 31724bb), Open Media Transport's VMX codec, with [sse2neon](https://github.com/DLTcollab/sse2neon) on ARM | MIT, MIT | the source's `LICENSE.txt` (copied into the packages); [LICENSES/MIT-sse2neon.txt](LICENSES/MIT-sse2neon.txt) |
| [x265](https://www.x265.org) 4.2, built with its multiview encoder for spatial video (MV-HEVC) export | GPL-2.0-or-later | the source's `COPYING` (copied into the packages; the release sources carry its tarball) |
| [ThorVG](https://github.com/thorvg/thorvg), with RapidJSON (MIT) and JerryScript (Apache-2.0) | MIT, MIT, Apache-2.0 | the source's licence files (copied into the packages) |
| [OpenEXR](https://github.com/AcademySoftwareFoundation/openexr) 3.5 and [Imath](https://github.com/AcademySoftwareFoundation/Imath), with the [OpenJPH](https://github.com/aous72/OpenJPH) (BSD-2-Clause), [libdeflate](https://github.com/ebiggers/libdeflate) (MIT) and [zstd](https://github.com/facebook/zstd) (BSD-3-Clause) OpenEXR bundles | BSD-3-Clause, BSD-3-Clause, BSD-2-Clause, MIT, BSD-3-Clause | the sources' licence files (copied into the packages) |
| [bmx](https://github.com/ebu/bmx) 1.7, the EBU's MXF library, with its [libMXF and libMXF++](https://github.com/ebu/bmx/tree/main/deps) and [uriparser](https://github.com/uriparser/uriparser) 1.0, and the version stamp each compiles in from [cmake-git-version-tracking](https://github.com/andrew-hardin/cmake-git-version-tracking) | BSD-3-Clause (uriparser's library: BSD-3-Clause; cmake-git-version-tracking: MIT) | the sources' licence files (copied into the packages) |
| [c2pa-rs](https://github.com/contentauth/c2pa-rs) (`c2pa-c-ffi`) and the Rust crates it uses | MIT or Apache-2.0; each crate's own | `c2pa` in the packages' `licenses` folder, one folder per crate |
| The ASC's [FDL library](https://github.com/ascmitc/fdl) 1.0.2 (`libfdl_core`, with the FDL JSON schemas), which reads, checks and transforms Framing Decision Lists, and [jsoncons](https://github.com/danielaparker/jsoncons) 1.5.0, its JSON parser and schema validator. Copyright 2024-present American Society Of Cinematographers; Copyright (c) 2013-2020 Daniel Parker | Apache-2.0, BSL-1.0 | the sources' licence files and the FDL library's `NOTICE` (copied into the packages); [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) |
| [VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) (interfaces and host helpers) | MIT | [third_party/vst3sdk/LICENSE.txt](third_party/vst3sdk/LICENSE.txt) |
| [cgltf](https://github.com/jkuhlmann/cgltf) 1.14, which reads glTF 2.0 models | MIT | [third_party/cgltf/LICENSE](third_party/cgltf/LICENSE) |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) 2.0.0rc13, which reads OBJ models | MIT (its optional earcut triangulator, ISC, is not compiled in) | [third_party/tinyobjloader/LICENSE](third_party/tinyobjloader/LICENSE) |
| [CLAP](https://github.com/free-audio/clap) headers | MIT | [third_party/clap/LICENSE](third_party/clap/LICENSE) |
| Celemony's [ARA Audio Random Access SDK](https://github.com/Celemony/ARA_SDK) 2.3.0: the API headers (with the VST3 and CLAP companion headers) and the ARA Library's host dispatch, for hosting ARA plug-ins. ARA Audio Random Access is a trademark of Celemony Software GmbH | Apache-2.0 | [third_party/ara/LICENSE.txt](third_party/ara/LICENSE.txt) |
| [OpenFX](https://github.com/AcademySoftwareFoundation/openfx) headers | BSD-3-Clause | [third_party/openfx/LICENSE](third_party/openfx/LICENSE) |
| Blackmagic DeckLink SDK 12.0 headers | Blackmagic Design's header licence | [third_party/decklink/LICENSE](third_party/decklink/LICENSE) |
| NDI SDK 6 headers | MIT (these files only) | [third_party/ndi/LICENSE](third_party/ndi/LICENSE) |

## Libraries bundled in the packages

The packages bundle these shared libraries and their dependencies. Each package's `licenses/BUNDLED.txt` lists every
one with its exact version, licence and source.

- [FFmpeg](https://ffmpeg.org), built by `scripts/build-ffmpeg.sh` with `--enable-gpl` (and never `--enable-nonfree`),
  which makes this build GPL-2.0-or-later; with [x264](https://www.videolan.org/developers/x264.html) (GPL-2.0-or-later),
  [x265](https://www.x265.org) (GPL-2.0-or-later), libvpx (BSD-3-Clause), SVT-AV1 (BSD-3-Clause-Clear), dav1d
  (BSD-2-Clause), Opus (BSD-3-Clause), OpenJPEG (BSD-2-Clause), SRT (MPL-2.0), librist (BSD-2-Clause) and
  [OpenAPV](https://github.com/AcademySoftwareFoundation/openapv) 0.2.1.3 (BSD-3-Clause, Copyright (c) 2022 Samsung
  Electronics Co., Ltd.; linked into libavcodec for APV encoding, its `LICENSE` copied into the packages' `ffmpeg`
  licences folder).
- [Qt](https://www.qt.io) 6 (LGPL-3.0): Core, Gui, Widgets, Network, Concurrent, Xml and Multimedia. You may replace
  the bundled Qt libraries with your own build of the same version.
- [OpenColorIO](https://opencolorio.org) (BSD-3-Clause), [ONNX Runtime](https://onnxruntime.ai) (MIT, with its own
  third-party notices), [LibRaw](https://www.libraw.org) (LGPL-2.1 or CDDL-1.0), [lilv](https://drobilla.net/software/lilv)
  (ISC), [libmysofa](https://github.com/hoene/libmysofa) (BSD-3-Clause), [OpenJPEG](https://www.openjpeg.org)
  (BSD-2-Clause) and [OpenSSL](https://www.openssl.org) 3 (Apache-2.0; encrypted DCPs, signatures and KDMs).

## Not included

Montage does not include the Blackmagic Desktop Video driver, the NDI Runtime or the Blackmagic RAW SDK; it uses them
when they are installed. NDI® is a registered trademark of Vizrt NDI AB ([ndi.video](https://ndi.video)). VST® is a
registered trademark of Steinberg Media Technologies GmbH.

AI models are not included either: Montage downloads each one when a feature first needs it, and names the model and
its licence when it asks to download it. One is not under a software licence: Bandit v2's weights (dialogue, music and
effects) are CC BY-SA 4.0, so they are downloaded from the authors' Zenodo record as they are and never repackaged.
