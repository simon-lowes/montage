# Implementation technologies for professional NLE features in a C++20 / Qt 6 / FFmpeg editor (as of October 2026)

Scope note: research done 7 October 2026. Licences were checked against the projects' own LICENSE/COPYING files or source headers where possible (raw files fetched from GitHub, googlesource and FFmpeg master). Montage today (from its own repo, not web-sourced): `CMakeLists.txt` requires `Qt6 6.4` (Core, Gui, Concurrent, Widgets, Multimedia), C++20; `src/core/Interchange.cpp` writes `.otio` JSON by hand (no OTIO library); there is no FFmpeg hwaccel code and no QRhi/OpenGL code in `src/`. The compositor is CPU float RGBA. Export presets already use GPL x264/x265, so Montage's distributed binaries already fall under the GPL.

## 1. GPU compositing and effects (Qt RHI, backends, compute, lessons from Olive / Kdenlive-MLT-movit / Shotcut / Blender)

### Takeaway
QRhi is the only cross-API GPU layer that ships with Qt. It has been usable by applications since Qt 6.6, but with "limited compatibility guarantees" (private-module linkage, possible source breaks in minor releases). It covers Vulkan, Metal, D3D11, D3D12 and OpenGL, offers compute where the API supports it, and has HDR swapchains. The open-source editors' GPU attempts mostly failed on stability: movit in MLT/Kdenlive/Shotcut was hidden or removed for years, and Olive is still alpha. Shotcut's late-2025 answer was a linear-light 10-bit CPU path with GPU as an experimental option, and Blender 5.0 brought GPU compositor node trees into the VSE. Montage should keep its CPU float path as the reference and fallback, and build a QRhi render graph with a float16/float32 working format beside it. That means raising the Qt floor from 6.4 to at least 6.6 (6.8 LTS is the sensible target).

### Cited Findings
- QRhi status: "The QRhi family of classes in the Qt Gui module, including QShader and QShaderDescription, offer limited compatibility guarantees. There are no source or binary compatibility guarantees for these classes". Apps link `Qt::GuiPrivate` and `#include <rhi/qrhi.h>`. This is available since Qt 6.6. — [Qt docs: QRhi](https://doc.qt.io/qt-6/qrhi.html)
- QRhi backends are OpenGL 2.1 / GLES 2.0+, Direct3D 11.2+, Direct3D 12 (Windows 10 1703+, SM 5.0+, needs ID3D12Device2, feature level 11_0 by default), Metal 1.2+, Vulkan 1.0+, and Null (no graphics calls). The `Implementation` enum is `{ Null, Vulkan, OpenGLES2, D3D11, D3D12, Metal }`. — [Qt docs: QRhi](https://doc.qt.io/qt-6/qrhi.html)
- Compute: the `QRhi::Compute` feature flag "Indicates that compute shaders, image load/store, and storage buffers are supported. OpenGL older than 4.3 and OpenGL ES older than 3.1 have no compute support." — [Qt docs: QRhi](https://doc.qt.io/qt-6/qrhi.html)
- Shaders: all shaders are written once (Vulkan-style GLSL), compiled to SPIR-V and packed into `QShader` instances. The `qsb` tool is part of the Qt Shader Tools module. `QRhiTexture::createFrom()` wraps existing native textures. — [Qt docs: QRhi](https://doc.qt.io/qt-6/qrhi.html)
- Runtime shader compilation: `QShaderBaker` (since Qt 6.6, `Qt::ShaderToolsPrivate`, `#include <rhi/qshaderbaker.h>`) has the same limited-compatibility warning. It "only handles the SPIR-V and human-readable source targets". Compiling to DXBC or MetalLib is done only by the offline `qsb` tool. — [Qt docs: QShaderBaker](https://doc.qt.io/qt-6/qshaderbaker.html)
- HDR swapchains (`QRhiSwapChain::Format`):
  - `HDRExtendedSrgbLinear` is 16-bit float scRGB, "the recommended format for HDR swapchains in general on desktop platforms". On Windows the system compositor converts it to the display's native space.
  - `HDR10` is 10-bit Rec.2020 with the ST 2084 PQ transfer function.
  - `HDRExtendedDisplayP3Linear` is "the primary choice for HDR on platforms such as iOS and VisionOS".
  - `isFormatSupported()` reports support; "SDR is always supported".
  - Source: [Qt docs: QRhiSwapChain](https://doc.qt.io/qt-6/qrhiswapchain.html)
- Qt has float CPU image formats `QImage::Format_RGBA16FPx4` / `RGBA32FPx4` (since Qt 6.2) and `Format_RGBA64` (since 5.12), which are useful for CPU-side text/title rasterisation into a float pipeline. — [Qt docs: QImage](https://doc.qt.io/qt-6/qimage.html)
- Shotcut's GPU processing is OpenGL-based (movit), "16-bit floating point linear per color component". The forum says that "GPU Effects has been hidden for a long time now due to instability". Shotcut 23.05.07 "restored Settings > GPU Effects". — [Shotcut 25.12 coverage](https://ubuntuhandbook.org/index.php/2025/12/shotcut-25-12-12-bit-video-support/); [Shotcut forum](https://forum.shotcut.org/t/clarification-on-what-gpu-acceleration-means/10772/2); [Neowin Shotcut 23.05.07](https://www.neowin.net/software/shotcut-230507/)
- Shotcut 25.12 (Dec 2025) added 10/12-bit CPU processing and four processing modes: Native 8-bit CPU, Native 10-bit CPU, Linear 10-bit CPU, and Linear 10-bit GPU/CPU (experimental). "GPU→CPU handoff now preserves linear color". — [AlternativeTo news, Jan 2026](https://alternativeto.net/news/2026/1/shotcut-25-12-adds-10-bit-video-cpu-pipeline-linear-color-processing-and-ui-upgrades)
- MLT (LGPL-2.1) still carries a `movit` module (`filter_glsl_manager`, `filter_movit_blur`, `filter_movit_convert`, …). — [MLT movit module](https://github.com/mltframework/mlt/tree/master/src/modules/movit); [MLT COPYING](https://github.com/mltframework/mlt/blob/master/COPYING)
- Kdenlive: KDE forum posts say GPU (movit) rendering was removed or not enabled by default because "its integration in MLT is not reliable in several cases, and the speed improvement is null as soon as CPU effects/transitions are applied". These are user-forum statements, not official docs. — [KDE forum](https://forum.kde.org/viewtopic.php%3Ff=265&t=125953.html); [KDE Discuss](https://discuss.kde.org/t/kdenlive-mlt-movit-devs-say-that-movit-library-has-been-fixed-and-should-work-with-kde/36622)
- Olive is GPL-3.0, C++/Qt with an OpenGL/GLSL renderer. Its README says: "Olive is alpha software and is considered highly unstable" (0.1.0 alpha and 0.2.0 unstable dev builds). — [Olive GitHub](https://github.com/olive-editor/olive)
- Olive 0.2 has node-based compositing, end-to-end OpenColorIO colour management and a "scene linear workflow where all compositing is achieved in a radiometrically accurate way". — [Wikipedia: Olive](https://en.wikipedia.org/wiki/Olive_(software))
- Blender 5.0 (Nov 2025) VSE added a "Compositor modifier" that applies compositor node trees inside the sequencer, plus HDR scopes, HDR support and ACES workflows. Vulkan/NVIDIA material compilation is up to 4x faster than 4.5 LTS. — [It's FOSS: Blender 5.0](https://itsfoss.com/news/blender-5-0-release/); [Digital Production: Blender 5.0](https://digitalproduction.com/2025/11/20/blender-5-0-its-here/)
- libplacebo (mpv's renderer) is LGPL-2.1-or-later.
  - Backends are Vulkan (including MoltenVK; core 1.2 minimum), OpenGL (GLSL ≥130) and D3D11. Metal is not listed.
  - Features include HDR tone mapping, Dolby Vision, ICC and gamut mapping, dithering, debanding and scaling.
  - It is used by mpv, VLC and FFmpeg's `vf_libplacebo`. The latest tag is v7.360.1.
  - Sources: [libplacebo GitHub](https://github.com/haasn/libplacebo); [VideoLAN tags](https://code.videolan.org/videolan/libplacebo/-/tags); [FFmpeg vf_libplacebo.c (LGPL header)](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_libplacebo.c)
- FFmpeg has started shipping per-API GPU filters: `scale_d3d11` and `pad_cuda` in 8.0, and `scale_d3d12`, `mestimate_d3d12` and `deinterlace_d3d12` in 8.1. — [FFmpeg news](https://ffmpeg.org/index.html)

### Inferences
- **Recommended architecture**: a QRhi render graph with RGBA16F intermediates (RGBA32F for accumulation-heavy effects such as blurs and keyers), one fragment or compute pass per effect, and an `QRhiTexture` pool keyed by size/format. Bake built-in effect shaders offline with `qsb` (CMake `qt_add_shaders`). Use `QShaderBaker` at runtime only for dynamically generated code such as OCIO shaders. It produces SPIR-V plus GLSL/HLSL/MSL source, and the D3D and Metal backends compile those at runtime.
- **Backend choice**: Metal on macOS (Apple's OpenGL stops at 4.1, so it has no compute per Qt's 4.3 rule), D3D12 or D3D11 on Windows, and Vulkan on Linux with an OpenGL fallback. The Null backend plus offscreen QRhi allows headless `montage-cli` rendering and CI tests.
- **Lesson from movit, Shotcut and Kdenlive**: mixing GPU and CPU effects in one chain removes the speed-up because of readbacks. Every built-in effect needs a GPU implementation, and CPU-only plugins (frei0r, CPU OFX) should be grouped so there is at most one download/upload per clip.
- **Lesson from Shotcut 25.12**: GPU and CPU paths must agree on linear light at the handoff. Keep Montage's CPU float compositor as the reference implementation and test GPU output against it with a tolerance, not bit-exactness.
- Raising Qt to 6.6 or newer has a cost: QRhi APIs may change in each minor release, so pin the Qt minor per release branch. Qt 6.8 is an LTS, though its LTS status was not verified in this session.
- libplacebo is a strong option for the viewer and export tone-mapping path on Vulkan/GL/D3D11. It has no Metal backend (MoltenVK only), so it does not fit a QRhi-Metal design on macOS without MoltenVK.

### Gaps
- Not verified: the movit licence (believed GPL-2.0-or-later), and Olive's latest commit date and whether development has resumed.
- Not verified: the exact Qt version in which the D3D12 backend and each HDR swapchain format arrived. The QRhiSwapChain page says only "Since: Qt 6.6" for the public class.
- No public performance benchmarks of QRhi-based NLE compositing were found.

## 2. Colour management (OpenColorIO 2.x, ACES 1.3/2.0, GPU shader generation, HDR PQ/HLG, display transforms, scopes)

### Takeaway
OpenColorIO (BSD-3-Clause) is the de facto standard. The current line is 2.5.x: 2.5.0 shipped in Sept 2025 and 2.5.2 in May 2026. 2.6.0 was planned for 30 Sept 2026, and whether it has shipped is unconfirmed. OCIO 2.5 ships built-in ACES 2.0 "studio" and "cg" configs, adds a Vulkan GLSL target, and generates shader code plus LUT textures and uniform blocks for GLSL, GLSL ES, HLSL SM5, MSL 2.0 and Vulkan GLSL 4.6, which maps cleanly onto QRhi. HDR display goes through QRhi's scRGB/HDR10 swapchains.

### Cited Findings
- OCIO is BSD-3-Clause ("Copyright Contributors to the OpenColorIO Project. Redistribution and use in source and binary forms…"). — [OCIO LICENSE](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/LICENSE)
- OCIO 2.5 was "delivered in September 2025 and is in the VFX Reference Platform for calendar year 2026".
  - Built-in ACES 2.0 configs: `ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5` and `ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5`.
  - Vulkan support: "The OCIO GPU renderer may now be used within applications that use Vulkan" (`GPU_LANGUAGE_GLSL_VK_4_6`).
  - New `GradingHueCurveTransform` (Hue-Hue, Hue-Sat, Hue-Lum and other curves).
  - New `edr-video` encoding for mixed HDR/SDR. Display colour spaces in new configs "pass through negative values rather than clamping them".
  - It now requires C++17.
  - Source: [OCIO 2.5 release notes](https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_5.html)
- ACES 2.0 library support was finalised in OCIO 2.4.2, and 2.5.0 added the built-in ACES 2.0 configs, so ACES 2.0 Output Transforms can be used as OCIO views. — [OCIO 2.5 release notes](https://opencolorio.readthedocs.io/en/main/releases/ocio_2_5.html)
- Releases: 2.5.1 on 13 Jan 2026 and 2.5.2 on 13 May 2026. 2.6.0 was planned as the CY2027 VFX Platform release for 30 Sept 2026. — [OCIO TAC update, March 2026](https://tac.aswf.io/meetings/2026-03-18/OCIO_TAC_Update-March_026.pdf); [package index](https://simple-repository.app.cern.ch/project/opencolorio)
- `GpuLanguage` enum (current header): `GPU_LANGUAGE_CG`, `GLSL_1_2`, `GLSL_1_3`, `GLSL_4_0`, `GLSL_VK_4_6`, `HLSL_SM_5_0` (alias `HLSL_DX11`), `GLSL_ES_1_0`, `GLSL_ES_3_0`, `MSL_2_0`. — [OpenColorTypes.h](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/include/OpenColorIO/OpenColorTypes.h)
- GPU shader API (`GpuShaderDesc`):
  - 1D/2D LUTs come through `getTexture()`/`getTextureValues()` and 3D LUTs through `get3DTexture()`/`get3DTextureValues()`. Uniforms come through `getUniform()`, `getNumUniforms()` and `getUniformBufferSize()`, and dynamic properties are exposed for live grading parameters.
  - For Vulkan-style explicit binding, `setDescriptorSetIndex(index, textureBindingStart)` sets the layout, with binding 0 reserved for the uniform buffer, and `getTextureShaderBindingIndex()` returns each texture's binding.
  - Source: [OCIO shader API docs](https://opencolorio.readthedocs.io/en/main/api/shaders.html)
- Olive 0.2 used OCIO end to end with scene-linear compositing. — [Wikipedia: Olive](https://en.wikipedia.org/wiki/Olive_(software))
- Blender 5.0 added a Convert Colorspace compositor node, OCIO-config-driven tooltips for displays and views, HDR support, HDR scopes in the VSE, and ACES workflows. — [It's FOSS: Blender 5.0](https://itsfoss.com/news/blender-5-0-release/)
- QRhi HDR swapchains are scRGB (`HDRExtendedSrgbLinear`), HDR10 PQ, and extended linear Display P3. — [Qt docs: QRhiSwapChain](https://doc.qt.io/qt-6/qrhiswapchain.html)
- FFmpeg 8.0 added a `colordetect` filter. — [FFmpeg news](https://ffmpeg.org/index.html)
- libplacebo offers "Dynamic HDR tone mapping", Dolby Vision support and "A colorimetrically accurate color management engine with support for soft gamut mapping, ICC profiles". — [libplacebo](https://github.com/haasn/libplacebo)

### Inferences
- **Integration recipe with QRhi**:
  1. Ask OCIO for `GPU_LANGUAGE_GLSL_VK_4_6` with `setDescriptorSetIndex`.
  2. Wrap the generated function in a Montage fragment/compute shader template.
  3. Bake it at runtime with `QShaderBaker` into SPIR-V, HLSL and MSL.
  4. Upload the LUTs as `QRhiTexture` (1D LUTs as 2D textures) and the uniforms into a `QRhiBuffer`.
  5. Cache the baked `QShader` by OCIO processor cache ID.
- Use the OCIO CPU processor for the existing CPU path and the scopes, so CPU and GPU match.
- **Pipeline**:
  1. Decode, then read the FFmpeg colour tags (`color_primaries`, `color_trc`, `colorspace`, `color_range`).
  2. Convert YUV to RGB in a Montage shader.
  3. Apply the OCIO input transform into a scene-linear working space (ACEScg, or linear Rec.709 for a "video" mode).
  4. Composite and grade.
  5. Apply the OCIO display/view transform for the viewer, and the output transform for export.
  6. Write the output colour tags, plus HDR10 mastering-display and content-light-level side data, into the encoder. This is general FFmpeg knowledge and was not re-verified here.
- **HDR viewer**: use a `HDR10` swapchain with an OCIO Rec.2100-PQ display view where the OS reports HDR. The alternative is scRGB, where linear values are scaled so 1.0 = 80 nits; that is standard scRGB convention, not verified here. Fall back to SDR when `isFormatSupported()` is false.
- **Colour-managed scopes**: compute waveform, vectorscope and histogram from the float working texture after the display or output transform, chosen by the user, as compute passes into a storage buffer. HDR scopes need a nits/PQ scale, as Blender 5.0 now provides.
- Montage's current `.cube` LUT support could move to OCIO `FileTransform` (cube, 3dl, CLF and others), which gives GPU support for free.

### Gaps
- Not confirmed: whether OCIO 2.6.0 shipped on 30 Sept 2026, and the ACES 2.0 release date.
- HLG display colour spaces in the built-in ACES 2.0 configs were not checked individually.
- No authoritative source was found on how commercial NLEs colour-manage their scopes.

## 3. Hardware-accelerated decode/encode via FFmpeg hwaccel, zero-copy to GPU textures, fallback strategy

### Takeaway
FFmpeg 6.1 to 8.1 now covers every platform API: VideoToolbox, D3D11VA, D3D12VA, DXVA2, NVDEC/NVENC (CUDA), QSV, VAAPI, AMF, and Vulkan Video decode (H.264/HEVC/AV1/VP9) and encode (H.264/HEVC/AV1). FFmpeg 8.x adds Vulkan compute codecs (FFv1, ProRes, ProRes RAW, DPX). No FFmpeg 9.0 release existed on ffmpeg.org as of the research date; master has bumped the libavcodec major version to 63. Qt Multimedia's own FFmpeg backend is the best reference for zero-copy decode into QRhi textures.

### Cited Findings
- FFmpeg 6.1 "Heaviside" (10 Nov 2023) introduced multi-threaded Vulkan hardware decoding for H.264, HEVC and AV1, and a VAAPI AV1 encoder. — [Phoronix](https://www.phoronix.com/news/FFmpeg-6.1-Released)
- FFmpeg 7.1 "Péter" (30 Sept 2024): VVC decoder stable, MV-HEVC decoding, and Vulkan H.264/HEVC encoding. — [FFmpeg news](https://ffmpeg.org/index.html)
- FFmpeg 8.0 "Huffman" (22 Aug 2025):
  - Vulkan compute-based codecs ("FFv1 (encode and decode), ProRes RAW (decode only)") that work on any Vulkan 1.3 implementation.
  - Vulkan VP9 hwaccel, Vulkan AV1 encoding, VAAPI VVC.
  - Native APV and ProRes RAW decoders.
  - Filters `colordetect`, `pad_cuda`, `scale_d3d11` and Whisper.
  - Sources: [FFmpeg news](https://ffmpeg.org/index.html); [Phoronix](https://www.phoronix.com/news/FFmpeg-8.0-Released)
- FFmpeg 8.1 "Hoare" (16 Mar 2026): Vulkan compute ProRes encode/decode and DPX decoding; D3D12 H.264/AV1 encoding; `scale_d3d12`, `mestimate_d3d12`, `deinterlace_d3d12`; Rockchip H.264/HEVC encoding. The ffmpeg.org news page lists no 9.0 release. — [FFmpeg news](https://ffmpeg.org/index.html)
- FFmpeg master defines `LIBAVCODEC_VERSION_MAJOR 63`. — [version_major.h](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/version_major.h)
- Master `configure` hwaccels:
  - Vulkan: apv, av1, dpx, ffv1, h264, hevc, prores_raw, prores, and vp9 (VP9 needs `vulkan_1_4`).
  - D3D12VA: av1, h264, hevc, mpeg2, vc1, vp9.
  - Source: [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure)
- Qt Multimedia's FFmpeg backend supports the hardware backends "cuda, drm, dxva2, d3d11va, d3d12va, opencl, qsv, vaapi, vdpau, videotoolbox, mediacodec, and vulkan". It does GPU texture conversion of decoded frames, which `QT_DISABLE_HW_TEXTURES_CONVERSION` disables. For VAAPI the conversion is off by default and needs `QT_XCB_GL_INTEGRATION=xcb_egl`. — [Qt docs: Advanced FFmpeg configuration](https://doc.qt.io/qt-6/advanced-ffmpeg-configuration.html)
- Qt's D3D11 path delivers decoded frames as "a texture array shared with the D3D11 display device". Qt split hardware decoding from conversion into RHI textures with a `TextureConverter` class. — [Qt qffmpeghwaccel_d3d11.cpp](https://contribute.qt-project.org/doc/d9/db3/qffmpeghwaccel__d3d11_8cpp_source.html); [Qt qffmpeghwaccel.cpp history](https://code.qt.io/cgit/qt/qtmultimedia.git/log/src/plugins/multimedia/ffmpeg/qffmpeghwaccel.cpp); [Qt VAAPI source](https://code.qt.io/cgit/qt/qtmultimedia.git/tree/src/plugins/multimedia/ffmpeg/qffmpeghwaccel_vaapi.cpp)
- Kdenlive 25.04 added a Quick Sync H.264/H.265 render profile and macOS VideoToolbox hardware render profiles. — [Kdenlive 25.04 release](https://kdenlive.org/news/releases/25.04.0/)
- FFmpeg's legal page warns that "once you start trying to make money from patented technologies, the owners of the patents will come after their licensing fees." — [FFmpeg legal](https://ffmpeg.org/legal.html)

### Inferences
- **Zero-copy paths per platform** (design guidance from FFmpeg/Qt architecture, not individually verified):
  - **macOS**: VideoToolbox gives `AV_PIX_FMT_VIDEOTOOLBOX` (CVPixelBuffer). Turn it into one MTLTexture per plane with `CVMetalTextureCache`, wrap each with `QRhiTexture::createFrom`, then run an NV12/P010 → RGB shader.
  - **Windows**: create the FFmpeg `AVD3D11VADeviceContext` on the same ID3D11Device QRhi uses (or a shared one), and sample the array-slice texture through per-plane SRVs. With QRhi D3D12, prefer the FFmpeg `d3d12va` hwaccel on QRhi's device, or use shared NT handles.
  - **Linux**: use VAAPI → DRM-PRIME (dma-buf) → EGLImage for the GL backend or VkImage import for Vulkan. Another route is FFmpeg Vulkan decode on a VkDevice shared with QRhi through `QRhiVulkanNativeHandles`. FFmpeg needs the video queue families and extensions enabled on that device.
  - **NVIDIA**: Vulkan Video, or NVDEC with CUDA-graphics interop.
- **Fallback strategy**: probe `avcodec_get_hw_config()` per stream. In `get_format`, fall back to software when the hw format is not offered (common for 4:2:2/4:4:4 and some 10-bit profiles), and mark the clip as software-decoded for the session. Cap concurrent hw decoder instances, because multicam and scrubbing open many decoders. Allow a per-clip override and keep frame-accurate seeking identical across both paths.
- **Encode**: offer `h264/hevc_videotoolbox`, `*_nvenc`, `*_qsv`, `*_amf`, `*_vaapi`, `*_vulkan` and, from 8.1, `*_d3d12va` as "fast" presets beside x264/x265. The OS encoders also avoid shipping a GPL encoder for those exports.
- Montage currently has no hwaccel code. Qt Multimedia's FFmpeg plugin is LGPL/GPL Qt code, so it is a design reference; whether it can be copied depends on Montage's final licence.

### Gaps
- The FFmpeg HWAccelIntro wiki (trac.ffmpeg.org) was blocked by bot protection, so the per-vendor capability matrix (NVDEC 4:2:2 support, NVENC session limits, QSV via libvpl) was not verified.
- No source was found that confirms the exact FFmpeg version adding D3D12VA decode.

## 4. Speech-to-text, transcript editing, diarisation, caption export and CEA-608/708 embedding

### Takeaway
whisper.cpp (MIT, with OpenAI's MIT-licensed weights) is the clear in-process choice. It has Metal, CUDA, Vulkan, Core ML and OpenVINO backends, Silero VAD, word-level timestamps and a C API. FFmpeg 8.0 also ships an LGPL `whisper` filter built on it. For diarisation, sherpa-onnx (Apache-2.0) runs the pyannote segmentation model (MIT) plus speaker-embedding models and clustering through a C/C++ API without Python. Captions:
- SRT/VTT are easy to write directly.
- FFmpeg has `scc`/`mcc` muxers and MOV `c608` tracks.
- x264, x265, NVENC and VideoToolbox insert CEA-608/708 SEI from `AV_FRAME_DATA_A53_CC` side data.
- FFmpeg has no text→608 encoder. Montage needs libcaption (MIT) or its own 608 byte-pair generator.

### Cited Findings
- whisper.cpp licence: "MIT license" (LICENSE: "Copyright (c) 2023-2026 The ggml authors"). — [whisper.cpp](https://github.com/ggml-org/whisper.cpp); [LICENSE](https://github.com/ggml-org/whisper.cpp/blob/master/LICENSE)
- whisper.cpp features:
  - Backends: Metal/Accelerate/NEON, CUDA, Vulkan, Core ML (ANE), OpenVINO, ROCm, AVX/VSX.
  - Integer quantisation (for example Q5_0).
  - "Speaker segmentation via tinydiarize (experimental)" and Silero-VAD support.
  - Word-level timestamps (`-ml 1`) and karaoke output (`-owts`).
  - A "C-style API" in `include/whisper.h`.
  - Source: [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
- whisper.cpp model sizes and memory:

  | Model | Disk | Memory |
  |---|---|---|
  | tiny | 75 MiB | ~273 MB |
  | base | 142 MiB | ~388 MB |
  | small | 466 MiB | ~852 MB |
  | medium | 1.5 GiB | ~2.1 GB |
  | large | 2.9 GiB | ~3.9 GB |

  Source: [whisper.cpp](https://github.com/ggml-org/whisper.cpp)
- "Whisper's code and model weights are released under the MIT License." `turbo` (809M parameters) is "an optimized version of large-v3" that is not trained for translation. — [openai/whisper](https://github.com/openai/whisper)
- FFmpeg 8.0 added a Whisper filter. `configure --enable-whisper` requires `whisper >= 1.7.5`, and `af_whisper.c` is LGPL-2.1-or-later. — [FFmpeg news](https://ffmpeg.org/index.html); [configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure); [af_whisper.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/af_whisper.c)
- sherpa-onnx is Apache-2.0 (LICENSE checked). Its offline speaker diarisation C API combines pyannote segmentation-3.0 (ONNX), a speaker-embedding extractor (3D-Speaker, NeMo or WeSpeaker) and fast clustering. — [sherpa-onnx LICENSE](https://github.com/k2-fsa/sherpa-onnx); [LocalAI diarization docs](https://localai.io/features/audio-diarization/); [k2-fsa HF space](https://huggingface.co/spaces/k2-fsa/speaker-diarization/blob/main/model.py)
- pyannote.audio, including the speaker-diarization-3.1 pipeline and the segmentation-3.0 model, is MIT licensed. Praat bundles it and reproduces the licence. — [Praat manual: pyannote MIT License](https://www.fon.hum.uva.nl/praat/manual/pyannote_audio_MIT_License.html)
- pyannote "speaker-diarization-community-1" is CC-BY-4.0 and outperforms 3.1 on AMI, DIHARD 3 and VoxConverse. It provides "exclusive" diarisation for easier alignment with transcripts, and downloads are gated behind sharing contact information. A third-party directory gives its release date as 2025-04-15. — [Hugging Face model card](https://huggingface.co/pyannote/speaker-diarization-community-1)
- Kdenlive's speech-to-text uses Whisper (with a GPU script and a models folder) and VOSK. VOSK is no longer installed in the Flatpak as of 25.04. — [Kdenlive 25.04 release](https://kdenlive.org/news/releases/25.04.0/)
- FFmpeg master has `scc` and `mcc` demuxers and muxers. — [allformats.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/allformats.c)
- The MOV muxer handles `AV_CODEC_ID_EIA_608`, which ffprobe shows as `eia_608 (c608)` in Final Cut Pro MOVs. — [movenc.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/movenc.c); [FFmpeg trac #7694](https://trac.ffmpeg.org/ticket/7694)
- Encoders with an `a53cc` option that insert A53 closed-caption SEI from frame side data:
  - libx264: `"a53cc", "Use A53 Closed Captions (if available)"`, default on.
  - libx265, NVENC and VideoToolbox (`AV_FRAME_DATA_A53_CC`).
  - Sources: [libx264.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/libx264.c); [libx265.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/libx265.c); [nvenc.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/nvenc.c); [videotoolboxenc.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/videotoolboxenc.c)
- FFmpeg has only a `ccaption` *decoder*. No CEA-608 encoder from text exists in `allcodecs.c`. — [allcodecs.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/allcodecs.c)
- A 2015 ffmpeg-user thread said FFmpeg could not embed SCC into MP4/MOV; muxer support has improved since (see above). — [ffmpeg-user 2015](https://ffmpeg.org/pipermail/ffmpeg-user/2015-August/027941.html)
- libcaption is MIT, pure C with no dependencies, version 0.8. It is a "CEA608 / CEA708 closed-caption encoder/decoder" with utilities to "create h.264 SEI … NALUs". Its limits: "608 support is currently limited to encoding and decoding the necessary control and preamble codes", "708 support is limited to encoding the 608 data in NTSC field 1", and "B-frame support for caption creation is minimal". — [libcaption](https://github.com/szatmary/libcaption)
- GStreamer's `rsclosedcaption` plugin has `tttocea608`, `tttocea708` and `cea708mux`. `h264ccinserter` inserts caption metas as SEI. A typical pipeline is `tttocea608` → `cccombiner` → encoder. — [GStreamer rsclosedcaption](https://gstreamer.freedesktop.org/documentation/rsclosedcaption/index.html); [h264ccinserter](https://gstreamer.freedesktop.org/documentation/closedcaption/h264ccinserter.html); [GStreamer Discourse](https://discourse.gstreamer.org/t/add-closed-captions-cea-708-to-videostream/4249)

### Inferences
- **Transcript editing pipeline**:
  1. Resample to 16 kHz mono with libswresample.
  2. Run whisper.cpp in-process on a worker thread with VAD, word timestamps, and progress and abort callbacks.
  3. Store a transcript document (words, source-media times, confidence, speaker) per media item.
  4. Map text selections to source ranges, and turn "delete words" into ripple-delete edits on the timeline.
  5. Generate captions from word timings with line-length, CPS and duration rules.
- Link whisper.cpp directly rather than using FFmpeg's `whisper` filter, for control over models, timestamps and cancellation. The filter suits `montage-cli`.
- Download models on first use rather than bundling them (large-v3/turbo is around 1.5–3 GB). Whisper weights are MIT. If pyannote community-1 is used, the CC-BY-4.0 attribution must appear in the app's credits.
- **CEA-608/708 export**:
  1. Write the caption track as 608 pop-on byte pairs, via libcaption or Montage's own encoder.
  2. For embedded captions in H.264/HEVC, attach `AV_FRAME_DATA_A53_CC` side data to each output frame before `avcodec_send_frame`. The encoder writes the SEI (`a53cc` is on by default in libx264).
  3. For ProRes/MOV deliverables, mux a `c608` track.
  4. For sidecars, use FFmpeg's `scc` or `mcc` muxer, or write SCC text directly.
  5. Test with ccextractor or the FFmpeg `ccaption` decoder.

### Gaps
- The current whisper.cpp release number was not retrieved (GitHub API unavailable in this session).
- Not found: an open-source C/C++ library with full 608 roll-up/paint-on support and 708 service-block authoring beyond libcaption's stated limits. GStreamer's Rust plugin licence was not confirmed.
- Not researched: non-Whisper ASR models (NVIDIA Parakeet and similar) and their licences.

## 5. Motion tracking, stabilisation, optical flow and frame interpolation (OpenCV, vid.stab, minterpolate, RIFE, planar tracking)

### Takeaway
Licences and status:
- **OpenCV** is Apache-2.0. CSRT and KCF are now contrib-only. The main `video` module has the DNN trackers Nano and Vit.
- **vid.stab** has been relicensed from GPL to LGPL-2.1-or-later (releases up to v1.1.2 were GPL), although FFmpeg's configure still gates `libvidstab` behind `--enable-gpl`.
- **minterpolate** is LGPL but slow.
- **RIFE** (Practical-RIFE and rife-ncnn-vulkan, both MIT, on ncnn, BSD-3) is the practical open "Speed Warp" equivalent.
- **libmv** (MIT, Blender's tracker) is a permissively licensed base for planar and region tracking.
- **Gyroflow** (GPL-3.0, with an OFX plugin) covers gyro-based stabilisation.

### Cited Findings
- OpenCV's LICENSE is Apache-2.0. The 4.x branch `version.hpp` reads 4.15.0, so it is in development, and 5.x documentation is published. — [OpenCV LICENSE](https://github.com/opencv/opencv/blob/4.x/LICENSE); [version.hpp](https://github.com/opencv/opencv/blob/4.x/modules/core/include/opencv2/core/version.hpp); [OpenCV 5.x TrackerNano javadoc](https://docs.opencv.org/5.x/javadoc/org/opencv/video/TrackerNano.html)
- TrackerCSRT and TrackerKCF are in the opencv_contrib `tracking` module. TrackerNano (a DNN tracker, about 1.9 MB, two models) and TrackerVit (VitTrack) are in the main `video` module. — [OpenCV tracking module](https://docs.opencv.org/4.x/d9/df8/group__tracking.html); [TrackerNano](https://docs.opencv.org/4.x/d8/d69/classcv_1_1TrackerNano.html); [LearnOpenCV](https://learnopencv.com/object-tracking-using-opencv-cpp-python/)
- vid.stab is now "free software under the GNU Lesser General Public License, version 2.1 or (at your option) any later version… An application may link vid.stab without taking on this licence." "Releases up to and including v1.1.2 were under the GPL." The reason given: under the GPL "FFmpeg could only build the vidstabdetect and vidstabtransform filters under --enable-gpl, and most distributed FFmpeg builds therefore shipped without video stabilization." — [vid.stab LICENSE](https://github.com/georgmartius/vid.stab); [RELICENSE.md](https://github.com/georgmartius/vid.stab/blob/master/RELICENSE.md)
- vid.stab README: FFmpeg "must run in two-pass mode" (vidstabdetect, then vidstabtransform). "Both passes must see the same frames". "ffmpeg's configure still gates --enable-libvidstab behind --enable-gpl". `libvidstab` is still in FFmpeg's `EXTERNAL_LIBRARY_GPL_LIST`. — [vid.stab README](https://github.com/georgmartius/vid.stab); [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure)
- `vf_minterpolate.c` is LGPL (GNU Lesser General Public License header). FFmpeg 8.1 added `mestimate_d3d12`. — [vf_minterpolate.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/vf_minterpolate.c); [FFmpeg news](https://ffmpeg.org/index.html)
- FFmpeg filters that need `--enable-gpl`: blackframe, boxblur, colormatrix, cover_rect, cropdetect, delogo, eq, find_rect, fspp, histeq, hqdn3d, interlace, kerndeint, mcdeint, mpdecimate, nnedi, owdenoise, perspective, phase, pp7, pullup, repeatfields, sab, signature, smartblur, spp, stereo3d, super2xsai, tinterlace, uspp, vaguedenoiser. — [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure)
- Practical-RIFE is MIT (Copyright 2021 hzwer). rife-ncnn-vulkan is MIT (Copyright 2020 nihui). ncnn is BSD-3-Clause, with third-party components under their own licences. — [Practical-RIFE](https://github.com/hzwer/Practical-RIFE); [rife-ncnn-vulkan](https://github.com/nihui/rife-ncnn-vulkan); [ncnn LICENSE](https://github.com/Tencent/ncnn/blob/master/LICENSE.txt)
- Gyroflow and its OFX plugin are GPL-3.0. The project is described as "GPLv3 License with App Store Exception". The OFX plugin applies Gyroflow stabilisation inside Resolve and other OFX hosts without transcoding, using Metal on macOS and OpenGL/OpenCL/CUDA on Windows and Linux. — [Gyroflow](https://github.com/gyroflow/gyroflow); [gyroflow-ofx](https://github.com/gyroflow/gyroflow-ofx); [Gyroflow OFX docs](https://docs.gyroflow.xyz/app/video-editor-plugins/openfx)
- libmv, the tracking library inside Blender, carries an MIT-style permission notice ("Copyright (c) 2012 libmv authors. Permission is hereby granted, free of charge…"). — [Blender intern/libmv track_region.h](https://projects.blender.org/blender/blender/src/branch/main/intern/libmv/libmv/tracking/track_region.h)

### Inferences
- **Point and object tracking**: OpenCV CSRT (contrib, accurate, CPU) for clip trackers driving Transform keyframes. Nano or Vit (main module, DNN) for fast object tracking.
- **Planar tracking** (Mocha-like corner pin) has no turnkey open library. Options: KLT (`calcOpticalFlowPyrLK`) plus RANSAC `findHomography` per frame with drift correction, or libmv's region tracker with homography motion models. Blender's plane track is built on libmv, though that was not verified in this session. Either feeds a four-corner pin in the GPU transform.
- **Stabilisation**: link libvidstab directly (now LGPL) or reimplement with OpenCV (`goodFeaturesToTrack` + LK + `estimateAffinePartial2D` + trajectory smoothing). Store the per-frame transforms in the project and apply them in the GPU transform with zoom-to-fill. This avoids FFmpeg's two-pass file workflow. Gyroflow (GPL-3.0) is acceptable for a GPL app, through its OFX plugin or its core library.
- **Retiming (Speed Warp equivalent)**: RIFE 4.x through ncnn-Vulkan, or through ONNX Runtime with an exported model, generates intermediate frames. Cache the results to disk per clip and speed. Offer frame blending, and `minterpolate` as a CPU fallback.

### Gaps
- No official technical description of DaVinci Resolve's Speed Warp (Studio-only, Neural Engine) was found.
- Not separately verified: the licence of the RIFE model weights (they are not in the repos), and OpenCV 5.0's release status.
- No benchmarks were gathered.

## 6. AI segmentation/masks and voice isolation (ONNX Runtime, SAM 2 / MobileSAM / RVM, Demucs / DeepFilterNet / RNNoise)

### Takeaway
ONNX Runtime (MIT) is the portable inference layer. On Windows, DirectML is in maintenance mode and new work has moved to Windows ML. For masks, SAM 2 is Apache-2.0 for code and checkpoints, which is the best licence/quality fit, and Kdenlive 25.04 already uses it. SAM 3 is under Meta's custom "SAM License" with trade-control and military-use restrictions. Other options:
- MobileSAM and EfficientSAM are Apache-2.0, and BiRefNet is MIT.
- RobustVideoMatting is GPL-3.0, which is acceptable for a GPL app.
- For dialogue cleanup, DeepFilterNet (MIT/Apache-2.0) and RNNoise (BSD-3, already available as FFmpeg `arnndn`) are permissive.
- Demucs is MIT but unmaintained.

### Cited Findings
- ONNX Runtime is MIT ("Copyright (c) Microsoft Corporation"), and main's `VERSION_NUMBER` is 1.31.0. — [onnxruntime LICENSE](https://github.com/microsoft/onnxruntime); [VERSION_NUMBER](https://github.com/microsoft/onnxruntime/blob/main/VERSION_NUMBER)
- "DirectML is in maintenance mode. If your PC runs Windows 11, version 24H2 (build 26100) or later, consider using Windows ML". Windows ML includes a copy of ONNX Runtime and dynamically downloads vendor execution providers. — [ONNX Runtime DirectML EP](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html); [Microsoft: Windows ML overview](https://learn.microsoft.com/windows/ai/new-windows-ml/overview)
- "The SAM 2 model checkpoints, SAM 2 demo code (front-end and back-end), and SAM 2 training code are licensed under Apache 2.0". SAM 2.1 sizes: Tiny 38.9M, Small 46M, Base+ 80.8M, Large 224.4M parameters. It uses "streaming memory for real-time video processing". — [SAM 2](https://github.com/facebookresearch/sam2)
- The SAM 3 "SAM License" (last updated 19 Nov 2025) is a royalty-free licence to use, modify and redistribute, but:
  - Redistribution must carry the agreement.
  - Users must not "reverse engineer, decompile or discover the underlying components".
  - ITAR and "military or warfare" uses, among others, are prohibited, and sanctioned parties are excluded.
  - Source: [SAM 3 LICENSE](https://github.com/facebookresearch/sam3/blob/main/LICENSE)
- Licences checked from LICENSE files: MobileSAM Apache-2.0, EfficientSAM Apache-2.0, BiRefNet MIT, RobustVideoMatting GPL-3.0. — [MobileSAM](https://github.com/ChaoningZhang/MobileSAM); [EfficientSAM](https://github.com/yformer/EfficientSAM); [BiRefNet](https://github.com/ZhengPeng7/BiRefNet); [RobustVideoMatting](https://github.com/PeterL1n/RobustVideoMatting)
- Kdenlive 25.04: "A new plugin based on the SAM2 model now allows you to create object masks to remove the background of your videos or apply an effect only to an object. All processing is done locally". It installs as a Python virtual-environment plugin, with an "option to offload memory to CPU" for low-VRAM GPUs, and warns about SAM2's high memory use. — [Kdenlive 25.04 release](https://kdenlive.org/news/releases/25.04.0/)
- Demucs README: "this repository is not maintained anymore" (the author left Meta, and the fork is also "not actively maintained"). "Demucs is released under the MIT license". — [Demucs](https://github.com/facebookresearch/demucs)
- DeepFilterNet is dual-licensed MIT or Apache-2.0. — [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet)
- RNNoise's COPYING is a BSD-style licence (Jean-Marc Valin, Mozilla, Xiph.Org, Amazon). FFmpeg's `af_arnndn.c` is derived from that code (Mozilla, Xiph, Jean-Marc Valin copyrights) and is not in the GPL-only filter list. — [RNNoise COPYING](https://github.com/xiph/rnnoise/blob/main/COPYING); [af_arnndn.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavfilter/af_arnndn.c); [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure)

### Inferences
- **Runtime**: ONNX Runtime with these execution providers: CoreML on macOS; CUDA/TensorRT on NVIDIA; DirectML on Windows today with WinML as the forward path; OpenVINO on Intel; CPU fallback. Download models on demand and verify them by hash.
- **Masks UX**: click or box prompts on a keyframe, run SAM 2 image encoder and decoder, then propagate across frames. Propagation in ONNX is harder than single images because of SAM 2's memory-attention state, which may need a custom export or EfficientTAM-style models. Store the masks as a per-frame alpha cache (16-bit PNG or RLE) and feed them as a matte texture into the GPU compositor. Allow refinement with BiRefNet (MIT) or RVM (GPL-3.0) for hair-level mattes.
- Avoid SAM 3 unless Montage accepts the custom licence. Its anti-reverse-engineering and trade-control clauses sit awkwardly with GPL redistribution terms (redistribution "under the terms of this Agreement"), so legal review is needed.
- **Dialogue cleanup tiers**:
  1. Instant: FFmpeg `arnndn` (RNNoise, BSD), already in Montage's FFmpeg.
  2. Quality: DeepFilterNet3 (48 kHz, MIT/Apache) via ONNX.
  3. Offline stem separation (dialogue / music / effects): Demucs htdemucs via ONNX export. Note that Demucs is no longer maintained.

### Gaps
- Not found: official ONNX export tooling for SAM 2 video propagation, and EfficientTAM's licence.
- DeepFilterNet's model-weight licence and C API were not separately verified.

## 7. Interchange (OpenTimelineIO C++, adapters, FCPXML, AAF, FCP7 XML)

### Takeaway
OTIO is Apache-2.0 with a C++17 core (`opentime`, `opentimelineio`) that reads and writes `.otio`/`.otioz`/`.otiod` natively. Every other format adapter is Python, moved after v0.16 into separate packages (OpenTimelineIO-Plugins: AAF via pyaaf2, CMX 3600, FCP7 XML, FCPX XML, ALE, and others). Kdenlive 25.04 shows the native route: a C++ OTIO import/export rewrite (by Darby Johnston, KDE e.V.-funded). For AAF:
- Import in C: LibAAF (GPL-2.0-or-later, read-only, audio-oriented, used by Ardour).
- Read/write in Python: pyaaf2 (MIT).
- The official C++ AAF SDK (AAF SDK Public Source License) is heavy.

### Cited Findings
- OTIO is Apache-2.0 (LICENSE.txt). "The core OTIO library is implemented in C++", with `opentime` a "dependency-less library for dealing strictly with time". CMake defaults to `CMAKE_CXX_STANDARD 17`. — [OTIO GitHub](https://github.com/AcademySoftwareFoundation/OpenTimelineIO)
- "For releases after v0.16, the OpenTimelineIO PyPI package will only include the core libraries and file formats". Only `otio_json`, `otiod` and `otioz` remain in core. The OpenTimelineIO-Plugins package adds AAF, ale, burnins, cmx_3600, fcp_xml, fcpx_xml, hls_playlist, maya_sequencer, svg and xges. — [OTIO GitHub](https://github.com/AcademySoftwareFoundation/OpenTimelineIO); [otio-fcpx-xml-adapter (PyPI)](https://pypi.org/project/otio-fcpx-xml-adapter); [otio-aaf-adapter (PyPI)](https://pypi.org/project/otio-aaf-adapter/1.0.0/)
- Kdenlive 25.04: Darby Johnston "rewrote the OpenTimelineIO import and export function using the C++ library". It handles multiple tracks and clips, markers and guides, and colour clips. "Effects, filters, and transitions are not exported as each application uses its own standard". Metadata export is temporarily disabled on macOS, and the Python implementation is deprecated. — [Kdenlive 25.04 release](https://kdenlive.org/news/releases/25.04.0/); [Kdenlive MR !561](https://invent.kde.org/multimedia/kdenlive/-/merge_requests/561); [KDE e.V. OTIO job ad](https://ev.kde.org/resources/jobad-opentimelineio2024.pdf)
- pyaaf2 is MIT, "pure Python… zero dependencies", and does not use the AAF SDK. The official C++ AAF SDK is under the "AAF SDK Public Source License Agreement". — [pyaaf2 (PyPI)](https://pypi.org/project/pyaaf2/1.0.0.dev5); [AAF SDK PSL](https://aaf.sourceforge.net/docs/AAFSDKPSL.html); [AAF SDK SourceForge](https://sourceforge.net/p/aaf)
- LibAAF:
  - Licence: the LICENSE file is GPLv2, and source headers say "either version 2 of the License, or (at your option) any later version".
  - It is a C library with no dependencies that reads AAF only. It is "audio-oriented" and "only supports a single video clip".
  - It handles embedded audio essence, multichannel audio, fades, crossfades and timecode.
  - Tested with Media Composer (8.4.5–23.12), Premiere Pro (12.0–23.5.0), Pro Tools (10.3.10–2023.12), Resolve (17.4.6–18.5), Logic and Fairlight.
  - Source: [LibAAF](https://github.com/agfline/LibAAF)
- Ardour 8.4 added beta AAF import using LibAAF. — [Phoronix: Ardour 8.4](https://www.phoronix.com/news/Ardour-8.4-DAW)

### Inferences
- **Step 1**: replace Montage's hand-written OTIO JSON (`src/core/Interchange.cpp`) with the OTIO C++ library (Apache-2.0, GPLv3-compatible) for both import and export. Map compound clips to nested `Stack`s, speed to `LinearTimeWarp`, and markers to `Marker`. Carry Montage-specific effects in OTIO `metadata` as Kdenlive does.
- **FCPXML and FCP7 XML (xmeml)**: write native C++ readers and writers with `QXmlStreamReader`/`Writer` into the OTIO in-memory model. Use the Python adapters as behavioural references. The alternative, embedding CPython plus OpenTimelineIO-Plugins, adds a lot of packaging weight (a signed Python runtime on macOS and Windows).
- **AAF**: import with LibAAF (GPL-2.0-or-later is compatible with a GPLv3 Montage; it is audio-focused, so video track import will be limited), into OTIO. Export is the hard part. Options are a bundled Python with pyaaf2 and otio-aaf-adapter, or a native writer. The AAF SDK is C++ under a custom public source licence, so check GPL compatibility before use.
- CMX 3600 export already exists in Montage. An EDL import is low effort.

### Gaps
- Not retrieved: the latest OTIO release number (GitHub API unavailable) and the current FCPXML DTD version.
- The AAF SDK PSL's compatibility with the GPL was not analysed.

## 8. Plugin effect standards (OpenFX, frei0r)

### Takeaway
OpenFX (BSD-3-Clause, now under ASWF) is the professional standard. OFX 1.5 (2024) added GPU rendering suites (OpenCL, CUDA, Metal), colour-management APIs, a DrawSuite for overlays without OpenGL, and Windows ARM64. Resolve, Natron, Vegas, Nuke, Scratch and Baselight host it, and the repo includes `HostSupport` code for writing a host. frei0r is a tiny C API with 8-bit RGBA only, and both its plugins and API are GPL. It is easy to host and fits a GPL app, but would quantise Montage's float pipeline.

### Cited Findings
- OpenFX 1.5 is "the first major update to the open standard for visual effects plugins for nine years" (1.4 was 2015). It adds:
  - GPU support via OpenCL, CUDA and Metal.
  - Enhanced colour management.
  - A DrawSuite ("On-screen drawing… without requiring OpenGL").
  - Choice params and binary data params.
  - Windows ARM64.
  - Sources: [CG Channel](https://www.cgchannel.com/2024/09/openfx-1-5-has-been-released/); [OpenFX 1.5 release notes](https://openfx.readthedocs.io/en/main/ReleaseNotes/relnotes-1.5.html); [openeffects.org](https://openeffects.org/new-openfx-1-5-release-streamlines-workflows-and-creative-applications/)
- The openfx repo is BSD-3-Clause, with `include`, `Support`, `HostSupport`, `Examples` and `openfx-cpp` directories. It names Nuke, Scratch, Vegas and Baselight as hosts. — [openfx GitHub](https://github.com/AcademySoftwareFoundation/openfx)
- Resolve supports OpenCL and CUDA extensions to OFX, and Metal pipelines since Resolve 16, with plugins getting the platform command queue. — [Hackernoon: Metal OFX plugin for Resolve](https://hackernoon.com/an-easy-way-to-develop-your-own-apple-metal-plugin-and-integrate-it-into-davinci-resolve); [Gyroflow OFX docs](https://docs.gyroflow.xyz/app/video-editor-plugins/openfx)
- Natron is GPLv2 ("free, open-source (GPLv2 license) video compositor"). It hosts OFX with "almost all features of OpenFX v1.4" and bundles openfx-io, openfx-misc, openfx-gmic and openfx-arena. — [Natron README](https://github.com/NatronGitHub/Natron)
- frei0r: "Frei0r sourcecode is released under the terms of the GNU General Public License and, eventually other compatible Free Software licenses" (COPYING is GPLv2). The API is a single header. Colour models are `F0R_COLOR_MODEL_BGRA8888`, `RGBA8888` and `PACKED32`, and parameter types are bool, double, color, position and string. — [frei0r README](https://github.com/dyne/frei0r); [frei0r.h](https://github.com/dyne/frei0r/blob/master/include/frei0r.h)
- FFmpeg lists `frei0r` in `EXTERNAL_LIBRARY_GPL_LIST`. — [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure)

### Inferences
- **OFX host scope** is large: the property, parameter, image-effect, memory, multithread, message, progress and interact/draw suites, plus the 1.5 GPU suites. The work is a parameter-to-keyframe bridge, a render-call bridge (CPU float RGBA buffers and, later, GPU textures through the OpenCL/CUDA/Metal suites), and threading.
- Natron's host code (GPLv2 per README) and the BSD `HostSupport` library are references. Licence wording for Natron's host code should be checked before copying, because GPLv2-only code cannot be combined with Apache-2.0 deps.
- Commercial OFX plugins are often licensed or tested per host (mostly Resolve), so a new host gets "works with free OFX plugins (openfx-misc, G'MIC, Gyroflow)" first. This is unverified market knowledge.
- **frei0r** is cheap (an afternoon to host) and gives about 100 effects that MLT/Shotcut/Kdenlive users know. Its 8-bit-only colour models force a float→8-bit→float round trip and readback, so label those effects "8-bit" in the UI.

### Gaps
- Not verified: Vegas's and Resolve's OFX version support levels (for example, whether Resolve implements the 1.5 colour-management suite), and per-host commercial plugin licensing policies.

## 9. Multicam editing, nested timelines, audio waveform and proxy caching patterns

### Takeaway
Among open-source editors, Kdenlive ships the reference multicam workflow: angles on stacked tracks, a monitor grid, and live cutting by number keys. Its 25.04 release also reworked audio thumbnails for speed and precision. Montage already has audio-waveform sync, compound clips and 960 px proxies (README), so multicam and caching are mostly architectural extensions rather than new libraries.

### Cited Findings
- Kdenlive multicam: put clips "in different tracks at the same position" and enable Tool → Multicam tool. Then cut live during playback "by pressing their corresponding numbers (for track V1, press key 1…)" or by clicking a track in the project monitor. Users have asked for improvements, including a dedicated multicam-clip model. — [Kdenlive features](https://kdenlive.org/features/); [KDE Discuss: Multicam improvements needed](https://discuss.kde.org/t/multicam-improvements-needed/34181)
- Kdenlive 25.04: "refactoring… to make the audio thumbnails faster and more precise", vertical zoom of audio waveforms, and a fix for proxy clips with alpha. — [Kdenlive 25.04 release](https://kdenlive.org/news/releases/25.04.0/)
- Shotcut 25.12 added processing modes trading speed for quality, from Native 8-bit CPU to Linear 10-bit GPU/CPU. — [AlternativeTo](https://alternativeto.net/news/2026/1/shotcut-25-12-adds-10-bit-video-cpu-pipeline-linear-color-processing-and-ui-upgrades)

### Inferences
These are design patterns from general NLE knowledge, not individually sourced.
- **Multicam clip model**: a multicam source is a special nested sequence of N angle tracks, synced by timecode, in/out points or Montage's existing waveform cross-correlation. A timeline instance references it with an `activeAngle` per segment, and angle switches are edits (cut + angle change) that can be undone.
  - The viewer shows a 2×2/3×3/4×4 grid decoded from proxies, which matters for hwaccel decoder counts.
  - "Live switch" records angle changes during playback. Audio can follow the video angle or stay fixed.
  - "Flatten multicam" converts to normal clips, and OTIO export flattens to the active angles.
- **Nested timelines**: compound clips referencing sequences, with cycle detection. Each nested sequence renders through the same graph, and its output frames are cached by a content hash of the subtree plus the frame time. Invalidate the cache upward on edits.
- **Waveform cache**: a per-media, per-audio-stream multi-resolution min/max peak pyramid. For example, the base level holds min/max per 256 samples, and each level above is 4× coarser. It is stored as a sidecar file in a cache directory keyed by a file hash plus stream index, built in background threads at import and memory-mapped for drawing. This is the same idea as Premiere's .pek and Audacity's summary blocks, though their formats were not verified here.
- **Proxies**: keep frame count, timecode and audio identical to the original, and use an intra-frame codec (ProRes Proxy/LT, DNxHR LB or MJPEG) at ½–¼ resolution. Switch proxies per-viewer and never use them for export. Montage's 960 px intra proxies already follow this.
- **Render cache**: cache composited segments to disk (ProRes or DNxHR, or float EXR for VFX segments), keyed by a segment hash. This is the "render in to out" and "smart cache" pattern.

### Gaps
- Not sourced: official documentation of Premiere, Resolve or FCP cache file formats, and the status of multicam in Shotcut, OpenShot and Blender VSE.

## 10. Text, titles and motion graphics (rich text, templates, Lottie via rlottie/ThorVG, Skia vs QPainter)

### Takeaway
Use QPainter/QTextDocument rendering into float `QImage`s (RGBA16FPx4 since Qt 6.2) or GPU textures for titles. Use ThorVG (MIT, active, v1.x) rather than rlottie for Lottie: rlottie is archived with no security support and does not support text layers or expressions. Lottie has been an open spec since Sept 2024 (v1.0 under the Linux Foundation's Joint Development Foundation), so it is a reasonable "motion-graphics template" substrate. MOGRT is Adobe's format. Skia (BSD-3) would be a large build dependency that duplicates Qt's text stack.

### Cited Findings
- rlottie: "This project has been archived and is no longer maintained. We does not provide security support, vulnerability review, patches, releases, or CVE assignment/coordination". Unsupported After Effects features include skew, merge paths, layer effects, text, expressions and several mask modes. — [rlottie](https://github.com/Samsung/rlottie)
- rlottie licensing: "rlottie basically comes with MIT license … but some parts of shared code are covered by different licenses": `src/vector` (MIT, plus a Skia notice), `src/vector/freetype` (FTL), `pixman` (pixman licence), `stb` (stb), and `rapidjson` (its own licence). — [rlottie COPYING](https://github.com/Samsung/rlottie/blob/master/COPYING)
- ThorVG is MIT ("Copyright (c) 2020 - 2026 ThorVG Project"), and main's meson version is 1.2.0. Lottie support was added in 2023 together with LottieFiles. — [ThorVG LICENSE](https://github.com/thorvg/thorvg); [Wikipedia: ThorVG](https://en.wikipedia.org/wiki/Thor_Vector_Graphics)
- On 17 Sept 2024 the Lottie Animation Community (under the Joint Development Foundation, a Linux Foundation project) released Lottie Specification v1.0. It contains a JSON schema, human-readable documentation, IANA registration and the ".lot" extension. — [Linux Foundation press release](https://www.linuxfoundation.org/press/lottie-animation-community-announces-lottie-v1.0-specification); [LottieFiles blog](https://lottiefiles.com/blog/inside-lottiefiles/lottie-specification-v1-0-a-milestone-for-open-source-animation)
- Skia is BSD-3-Clause ("Copyright (c) 2011 Google Inc… Redistribution and use…"). — [Skia LICENSE](https://skia.googlesource.com/skia/+/refs/heads/main/LICENSE)
- Qt's float image formats (`Format_RGBA16FPx4`, `RGBA32FPx4`) have existed since Qt 6.2. — [Qt docs: QImage](https://doc.qt.io/qt-6/qimage.html)

### Inferences
- **Titles engine**: a `QTextDocument` model (rich text, per-run fonts, colours and tracking) laid out with `QTextLayout`. Render per-glyph through `QGlyphRun`/`QPainterPath` so text can be animated per character, word or line with keyframes (type-on, slide, blur). Rasterise at output resolution into RGBA16F, cache by a parameter hash, and upload as a texture. Montage's existing title features (outline, shadow, box, tracking) fit this model.
- **Templates**: define a Montage template as JSON (layers, exposed parameters such as text, colours and durations) plus optional Lottie animations rendered by ThorVG with text and colour overrides. Lottie "slots" theming in ThorVG and the spec was not verified. Import of After Effects-authored motion graphics then goes through Bodymovin/Lottie export.
- **Skia vs QPainter**: Skia's advantages are GPU-accelerated paths, better text shaping integration and a Lottie player (Skottie). Its costs are a gn/ninja build, no stable API and a large binary. Only consider it if motion graphics become a major pillar. QPainter plus ThorVG covers titles and Lottie without new heavy dependencies.

### Gaps
- Not verified: ThorVG's Lottie text-layer, expression and slot support, Glaxnimate's licence and Kdenlive integration, and Skottie's maturity.

## 11. Packaging constraints: GPL vs LGPL components in macOS/Windows binaries (FFmpeg GPL with x264/x265)

### Takeaway
Montage ships FFmpeg built with `--enable-gpl` (x264/x265), so "the GPL applies to all of FFmpeg" and in practice the distributed application must be GPL-compatible. Because Montage would also combine Apache-2.0 components (OpenCV, OTIO, sherpa-onnx, SAM 2 code), the combined work must be distributed under GPLv3. Avoid GPL-2.0-only dependencies (LibAAF is "or later", so it is fine), and never ship `--enable-nonfree` components (fdk-aac, DeckLink). The GPL route makes Apple and Microsoft app-store distribution impractical unless all copyright holders grant an exception, as Gyroflow did for its own code; that is impossible for FFmpeg and x264.

### Cited Findings
- FFmpeg is LGPL-2.1-or-later, but "If those parts get used the GPL applies to all of FFmpeg".
  - The LGPL checklist: build without `--enable-gpl` and `--enable-nonfree`, "Make sure your program is not using any GPL libraries (notably libx264)", and use dynamic linking.
  - Distribute the matching FFmpeg source on the same server as the binaries, and credit FFmpeg in the about box and download page.
  - FFmpeg "is not available under any other licensing terms, especially not proprietary/commercial ones".
  - Source: [FFmpeg legal](https://ffmpeg.org/legal.html)
- FFmpeg master configure licence lists:

  | List | Libraries |
  |---|---|
  | GPL | avisynth, frei0r, libcdio, libdavs2, libdvdnav, libdvdread, librubberband, libvidstab, libx264, libx265, libxavs, libxavs2, libxvid |
  | Nonfree | decklink, libfdk_aac, libmpeghdec |
  | Version 3 (LGPLv3/GPLv3) | gmp, libaribb24, libastcenc, liblensfun, libopencore_amrnb/amrwb, libvo_amrwbenc, mbedtls, rkmpp |
  | GPLv3 | libsmbclient |

  It also lists GPL-only internal filters (Section 5). — [FFmpeg configure](https://github.com/FFmpeg/FFmpeg/blob/master/configure)
- x265 is copyrighted by MulticoreWare. Companies that do not want GPL terms can get a commercial licence "less restrictive than the GPL v2". — [x265 docs: introduction](https://x265.readthedocs.io/en/2.5/introduction.html)
- vid.stab was relicensed to LGPL so that FFmpeg builds could ship stabilisation without the GPL, but FFmpeg's configure still requires `--enable-gpl` for it. — [vid.stab RELICENSE.md](https://github.com/georgmartius/vid.stab/blob/master/RELICENSE.md); [vid.stab README](https://github.com/georgmartius/vid.stab)
- Gyroflow is distributed as "GPLv3 License with App Store Exception". — [Gyroflow](https://github.com/gyroflow/gyroflow)
- Peer licences, from repo licence files:
  - Shotcut GPL-3.0; OpenShot (openshot-qt) GPL-3.0; libopenshot LGPL-3.0; Olive GPL-3.0; MLT LGPL-2.1; Natron GPLv2.
  - Kdenlive ships a `LICENSES/GPL-3.0-only.txt`.
  - Sources: [Shotcut COPYING](https://github.com/mltframework/shotcut); [openshot-qt COPYING](https://github.com/OpenShot/openshot-qt); [libopenshot](https://github.com/OpenShot/libopenshot); [Olive](https://github.com/olive-editor/olive); [MLT](https://github.com/mltframework/mlt/blob/master/COPYING); [Natron](https://github.com/NatronGitHub/Natron)
- Component licences gathered in this research:

  | Component | Licence | Source |
  |---|---|---|
  | OCIO | BSD-3 | [OCIO LICENSE](https://github.com/AcademySoftwareFoundation/OpenColorIO/blob/main/LICENSE) |
  | OTIO | Apache-2.0 | [OTIO](https://github.com/AcademySoftwareFoundation/OpenTimelineIO) |
  | OpenFX | BSD-3 | [openfx](https://github.com/AcademySoftwareFoundation/openfx) |
  | whisper.cpp | MIT | [whisper.cpp](https://github.com/ggml-org/whisper.cpp) |
  | Whisper weights | MIT | [openai/whisper](https://github.com/openai/whisper) |
  | sherpa-onnx | Apache-2.0 | [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) |
  | ONNX Runtime | MIT | [onnxruntime](https://github.com/microsoft/onnxruntime) |
  | OpenCV | Apache-2.0 | [OpenCV](https://github.com/opencv/opencv/blob/4.x/LICENSE) |
  | ncnn | BSD-3 | [ncnn](https://github.com/Tencent/ncnn/blob/master/LICENSE.txt) |
  | vid.stab (current) | LGPL-2.1+ | [vid.stab](https://github.com/georgmartius/vid.stab) |
  | libplacebo | LGPL-2.1+ | [libplacebo](https://github.com/haasn/libplacebo) |
  | libcaption | MIT | [libcaption](https://github.com/szatmary/libcaption) |
  | LibAAF | GPL-2.0+ | [LibAAF](https://github.com/agfline/LibAAF) |
  | pyaaf2 | MIT | [pyaaf2](https://pypi.org/project/pyaaf2/1.0.0.dev5) |
  | frei0r | GPL | [frei0r](https://github.com/dyne/frei0r) |
  | ThorVG | MIT | [ThorVG](https://github.com/thorvg/thorvg) |
  | rlottie | MIT + FTL / pixman / stb / rapidjson parts | [rlottie COPYING](https://github.com/Samsung/rlottie/blob/master/COPYING) |
  | Skia | BSD-3 | [Skia](https://skia.googlesource.com/skia/+/refs/heads/main/LICENSE) |
  | SAM 2 | Apache-2.0 | [SAM 2](https://github.com/facebookresearch/sam2) |
  | SAM 3 | custom SAM License | [SAM 3](https://github.com/facebookresearch/sam3/blob/main/LICENSE) |
  | MobileSAM, EfficientSAM | Apache-2.0 | [MobileSAM](https://github.com/ChaoningZhang/MobileSAM); [EfficientSAM](https://github.com/yformer/EfficientSAM) |
  | BiRefNet | MIT | [BiRefNet](https://github.com/ZhengPeng7/BiRefNet) |
  | RobustVideoMatting | GPL-3.0 | [RVM](https://github.com/PeterL1n/RobustVideoMatting) |
  | Practical-RIFE, rife-ncnn-vulkan | MIT | [Practical-RIFE](https://github.com/hzwer/Practical-RIFE); [rife-ncnn-vulkan](https://github.com/nihui/rife-ncnn-vulkan) |
  | DeepFilterNet | MIT/Apache-2.0 | [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) |
  | Demucs | MIT (unmaintained) | [Demucs](https://github.com/facebookresearch/demucs) |
  | RNNoise | BSD-style | [RNNoise](https://github.com/xiph/rnnoise/blob/main/COPYING) |
  | Gyroflow | GPL-3.0 | [Gyroflow](https://github.com/gyroflow/gyroflow) |
  | libmv | MIT | [libmv](https://projects.blender.org/blender/blender/src/branch/main/intern/libmv/libmv/tracking/track_region.h) |
  | pyannote seg-3.0 / 3.1 | MIT | [Praat manual](https://www.fon.hum.uva.nl/praat/manual/pyannote_audio_MIT_License.html) |
  | pyannote community-1 | CC-BY-4.0 | [Hugging Face](https://huggingface.co/pyannote/speaker-diarization-community-1) |

### Inferences
- **Licence of Montage itself**: GPL x264/x265 plus Apache-2.0 deps (OpenCV, OTIO, sherpa-onnx) means the combined binaries must be distributed under GPLv3, since Apache-2.0 is incompatible with GPLv2-only. Declaring Montage "GPL-3.0-or-later" is the cleanest choice. Qt under LGPLv3 with dynamic linking is compatible (general knowledge; Qt's licence page not fetched).
- **Dependency rules**:
  - Avoid GPL-2.0-only code. Natron's and Kdenlive's exact "only" or "or later" terms need checking before reusing their code.
  - Never enable fdk-aac or DeckLink in shipped FFmpeg builds (nonfree, so not redistributable). Use FFmpeg's native AAC encoder.
  - Note that FFmpeg's `librubberband` (GPL) is the usual pitch-preserving time-stretch for speed changes. Permissive alternatives exist but were not researched here.
- **Store distribution**: GPL code from third parties (FFmpeg, x264) cannot receive an app-store exception from Montage. Distribute as a notarised DMG on macOS and a signed MSI/MSIX outside the store on Windows, plus Flatpak/AppImage. Publish the exact FFmpeg source and configure flags next to each release, as the FFmpeg checklist requires.
- **Optional LGPL-only "lite" build**: an FFmpeg build without `--enable-gpl`, using platform encoders (VideoToolbox, Media Foundation/NVENC/QSV/AMF) instead of x264/x265, would keep a future proprietary or app-store route open. Every other dependency chosen above (OCIO, OTIO, OFX, whisper.cpp, ONNX Runtime, OpenCV, ThorVG, libvidstab, libcaption, SAM 2) is permissive or LGPL, so the GPL items (x264/x265, frei0r, RVM, Gyroflow, LibAAF) are the only blockers. Each should be a separable plugin or module.
- **Patents**: GPL and LGPL give no codec patent licences (FFmpeg's legal page warns about patents). Using OS encoders shifts some of that to the platform vendor; this needs legal review and is not verified here.

### Gaps
- Not fetched: the FSF/Apple App Store GPL incompatibility statements, Qt's own licensing page, x264's commercial licensing terms (x264 LLC), and codec patent-pool obligations (Via LA, Access Advance). Legal review is advised for any proprietary or store distribution plan.
