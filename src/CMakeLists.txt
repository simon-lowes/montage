add_library(montage_engine STATIC
  core/Model.h core/Model.cpp
  core/MediaLog.h core/MediaLog.cpp
  core/Cdl.h core/Cdl.cpp
  core/Ale.h core/Ale.cpp
  core/KeyframeEdit.h core/KeyframeEdit.cpp
  core/MaskPath.h core/MaskPath.cpp
  core/ColorWarp.h core/ColorWarp.cpp
  core/ObjectMask.h core/ObjectMask.cpp
  core/VisualIndex.h core/VisualIndex.cpp
  core/AutoTag.h core/AutoTag.cpp
  core/Transcript.h core/Transcript.cpp
  core/Captions.h core/Captions.cpp
  core/CaptionFormats.cpp
  core/Cea708.h core/Cea708.cpp
  core/Teletext.h core/Teletext.cpp
  core/CaptionTools.h core/CaptionTools.cpp
  core/TranscriptEdit.h core/TranscriptEdit.cpp
  core/Slate.h core/Slate.cpp
  core/Chapters.h core/Chapters.cpp
  core/ChapterSuggest.h core/ChapterSuggest.cpp
  core/TextStats.h core/TextStats.cpp
  core/Automation.h core/Automation.cpp
  core/MarkerList.h core/MarkerList.cpp
  core/ReviewPage.h core/ReviewPage.cpp
  core/EffectPresets.h core/EffectPresets.cpp
  core/ScriptCut.h core/ScriptCut.cpp
  core/Cfb.h core/Cfb.cpp
  core/Zip.h core/Zip.cpp
  core/G2p.h core/G2p.cpp
  core/Checkerboard.h core/Checkerboard.cpp
  core/Bleep.h core/Bleep.cpp
  core/FaceIndex.h core/FaceIndex.cpp
  core/Aaf.h core/Aaf.cpp core/AafMetaDictionary.inc
  core/AafImport.h core/AafImport.cpp
  core/Surround.h core/Surround.cpp
  core/Ambisonics.h core/Ambisonics.cpp
  media/SpatialAudio.h media/SpatialAudio.cpp
  core/Effects.h core/Effects.cpp
  core/EditOps.h core/EditOps.cpp
  core/TimelineCompare.h core/TimelineCompare.cpp
  core/Reconform.h core/Reconform.cpp
  core/Adr.h core/Adr.cpp
  core/Provenance.h core/Provenance.cpp
  core/LensProfile.h core/LensProfile.cpp
  core/AudioDescription.h core/AudioDescription.cpp
  core/MergeClips.h core/MergeClips.cpp
  core/ClipAnimation.h core/ClipAnimation.cpp
  core/GradeVersions.h core/GradeVersions.cpp
  core/GradeGraph.h core/GradeGraph.cpp
  core/AudioChannels.h core/AudioChannels.cpp
  core/TranscriptCorrect.h core/TranscriptCorrect.cpp
  core/SpellCheck.h core/SpellCheck.cpp
  core/ColorGroups.h core/ColorGroups.cpp
  core/Interpretation.h core/Interpretation.cpp
  core/Numbers.h core/Numbers.cpp
  core/SpokenSearch.h core/SpokenSearch.cpp
  core/OnScreenText.h core/OnScreenText.cpp
  core/ImportBuilder.h core/InterchangeImport.cpp core/FcpXml.cpp
  core/Multicam.h core/Multicam.cpp
  core/History.h core/History.cpp
  core/ProjectIO.h core/ProjectIO.cpp
  core/ProjectLock.h core/ProjectLock.cpp
  core/Production.h core/Production.cpp
  core/GraphicsTemplate.h core/GraphicsTemplate.cpp
  core/Storyboard.h core/Storyboard.cpp
  automation/ScriptHost.h automation/ScriptHost.cpp
  automation/PythonHost.h automation/PythonHost.cpp
  automation/ScriptPanel.h automation/ScriptPanel.cpp
  core/Interchange.h core/Interchange.cpp
  automation/McpServer.h automation/McpServer.cpp
  media/Image.h media/Image.cpp
  media/Decoder.h media/Decoder.cpp
  media/Exr.h media/Exr.cpp
  media/Vector.h media/Vector.cpp
  media/CameraRaw.h media/CameraRaw.cpp
  media/ProResRaw.h media/ProResRaw.cpp
  media/Mlv.h media/Mlv.cpp
  media/SuperScale.h media/SuperScale.cpp
  media/MotionDeblur.h media/MotionDeblur.cpp
  media/Faces.h media/Faces.cpp
  media/FaceMesh.h media/FaceMesh.cpp
  media/FaceTracks.h media/FaceTracks.cpp
  media/DepthMap.h media/DepthMap.cpp
  media/Matting.h media/Matting.cpp
  media/TextToSpeech.h media/TextToSpeech.cpp
  media/VoiceClone.h media/VoiceClone.cpp
  media/ContentCredentials.h media/ContentCredentials.cpp
  media/SoundSearch.h media/SoundSearch.cpp
  media/StemSeparation.h media/StemSeparation.cpp
  media/BanditSeparation.h media/BanditSeparation.cpp
  media/LipSync.h media/LipSync.cpp
  media/LtcTimecode.h media/LtcTimecode.cpp
  media/MotionData.h media/MotionData.cpp
  media/Inpaint.h media/Inpaint.cpp
  media/ValueMap.h
  media/Rife.h media/Rife.cpp
  media/HwAccel.h media/HwAccel.cpp
  media/MediaPool.h media/MediaPool.cpp
  media/Relink.h media/Relink.cpp
  media/Analysis.h media/Analysis.cpp
  media/Proxy.h media/Proxy.cpp
  media/Inference.h media/Inference.cpp
  render/ColorChart.h render/ColorChart.cpp
  media/Loudness.h media/Loudness.cpp
  media/AudioSync.h media/AudioSync.cpp
  media/FieldRecorder.h media/FieldRecorder.cpp
  media/ImageSequence.h media/ImageSequence.cpp
  media/Interpret.h media/Interpret.cpp
  media/Psd.h media/Psd.cpp
  media/DualSystem.h media/DualSystem.cpp
  media/SpeakerSwitch.h media/SpeakerSwitch.cpp
  media/AutoDuck.h media/AutoDuck.cpp
  media/Offload.h media/Offload.cpp
  media/MicBleed.h media/MicBleed.cpp
  media/Tracking.h media/Tracking.cpp
  media/CameraSolve.h media/CameraSolve.cpp
  media/Reframe.h media/Reframe.cpp
  media/Capture.h media/Capture.cpp
  media/ModelFiles.h media/ModelFiles.cpp
  media/Sofa.h media/Sofa.cpp
  media/LensDatabase.h media/LensDatabase.cpp
  media/Segmenter.h media/Segmenter.cpp
  media/Diarizer.h media/Diarizer.cpp
  media/VisualSearch.h media/VisualSearch.cpp
  media/SpeechSearch.h media/SpeechSearch.cpp
  media/TextReader.h media/TextReader.cpp
  media/SpeechEnhance.h media/SpeechEnhance.cpp
  media/Beats.h media/Beats.cpp
  media/Translator.h media/Translator.cpp
  render/Processing.h render/Processing.cpp render/TransferTables.h
  render/Ofx.h render/Ofx.cpp
  render/FaceRefine.h render/FaceRefine.cpp
  render/Jpeg2000.h render/Jpeg2000.cpp
  render/PackageFiles.h render/PackageFiles.cpp
  render/Imf.h render/Imf.cpp
  render/BroadcastMxf.h render/BroadcastMxf.cpp
  render/Adm.h render/Adm.cpp
  render/Relight.h render/Relight.cpp
  render/FilmLook.h render/FilmLook.cpp
  render/Retime.h render/Retime.cpp
  render/VideoDenoise.h render/VideoDenoise.cpp
  render/Restoration.h render/Restoration.cpp
  render/LensBlur.h render/LensBlur.cpp
  render/ConformCheck.h render/ConformCheck.cpp
  render/Shapes.h render/Shapes.cpp
  render/Title3D.h render/Title3D.cpp
  render/AafExport.h render/AafExport.cpp
  render/ClipAnalysis.h render/ClipAnalysis.cpp
  render/MusicEdit.h render/MusicEdit.cpp
  render/Highlights.h render/Highlights.cpp
  render/Shorts.h render/Shorts.cpp
  render/Letterbox.h render/Letterbox.cpp
  render/LightLevel.h render/LightLevel.cpp
  render/Hdr10Plus.h render/Hdr10Plus.cpp
  control/Midi.h control/Midi.cpp
  control/MackieControl.h control/MackieControl.cpp
  control/Hui.h control/Hui.cpp
  control/Osc.h control/Osc.cpp
  control/Hid.h control/Hid.cpp
  control/SpeedEditor.h control/SpeedEditor.cpp
  control/Tangent.h control/Tangent.cpp
  render/Versions.h render/Versions.cpp
  render/ReviewExport.h render/ReviewExport.cpp
  render/DcpMxf.h render/DcpMxf.cpp
  render/Dcp.h render/Dcp.cpp
  render/DcpSecurity.h render/DcpSecurity.cpp
  render/XmlCanonical.h render/XmlCanonical.cpp
  render/RoomTone.h render/RoomTone.cpp
  render/ExtendClip.h render/ExtendClip.cpp
  render/PaperEdit.h render/PaperEdit.cpp
  render/VfxPull.h render/VfxPull.cpp
  render/Spherical.h render/Spherical.cpp
  render/Stereo.h render/Stereo.cpp
  render/ClipPlacement.h render/ClipPlacement.cpp
  render/AudioReactive.h render/AudioReactive.cpp
  core/Expression.h render/Expressions.cpp
  render/QualityCheck.h render/QualityCheck.cpp
  render/LutExport.h render/LutExport.cpp
  render/ProjectManager.h render/ProjectManager.cpp
  render/AutoBroll.h render/AutoBroll.cpp
  render/AutoMix.h render/AutoMix.cpp
  render/VoiceMatch.h render/VoiceMatch.cpp
  render/DialogueMatch.h render/DialogueMatch.cpp
  render/ColorSpace.h render/ColorSpace.cpp
  render/Gamut.h render/Gamut.cpp
  render/ColorSlice.h render/ColorSlice.cpp
  render/Clf.h render/Clf.cpp
  render/Amf.h render/Amf.cpp
  render/HdrView.h render/HdrView.cpp
  render/Ocio.h render/Ocio.cpp
  render/Compositor.h render/Compositor.cpp
  render/GpuCompositor.h render/GpuCompositor.cpp
  render/OGraf.h render/OGraf.cpp
  render/Gallery.h render/Gallery.cpp
  render/LinedScript.h render/LinedScript.cpp
  render/TrimPasses.h render/TrimPasses.cpp
  render/DialogueLoudness.h render/DialogueLoudness.cpp
  render/RenderCache.h render/RenderCache.cpp
  render/AudioFx.h render/AudioFx.cpp
  render/VideoFx.h render/VideoFx.cpp
  render/StyleFx.h render/StyleFx.cpp
  render/TemporalFx.h render/TemporalFx.cpp
  render/Deconvolve.h render/Deconvolve.cpp
  render/Exporter.h render/Exporter.cpp
  render/RenderFarm.h render/RenderFarm.cpp
  render/GradeGraph.h render/GradeGraph.cpp
  render/VideoPacking.h render/VideoPacking.cpp
  render/ReferenceOutput.h render/ReferenceOutput.cpp render/NdiOutput.cpp render/DeckLinkOutput.cpp
  render/RemoteReview.h render/RemoteReview.cpp
  render/SkyReplace.h render/SkyReplace.cpp
  render/FaceShape.h render/FaceShape.cpp
  render/MonitorCalibration.h render/MonitorCalibration.cpp
  audio/Plugins.h audio/PluginScan.cpp audio/ClapHost.cpp
  audio/DynLib.h audio/DynLib.cpp
  audio/PluginEffect.h audio/PluginEffect.cpp
  audio/SpeechCleanup.h audio/SpeechCleanup.cpp
  audio/DialogueAlign.h audio/DialogueAlign.cpp
  audio/RoomResponse.h audio/RoomResponse.cpp
  audio/Ltc.h audio/Ltc.cpp
  audio/AudioRepair.h audio/AudioRepair.cpp
  audio/SpectralRepair.h audio/SpectralRepair.cpp
  audio/TimeStretch.h audio/TimeStretch.cpp
)
target_include_directories(montage_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)
# OpenFX 1.5 headers (BSD-3-Clause, the ASWF's), for hosting video plugins (render/Ofx).
target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/openfx/include)
# NDI SDK headers (MIT, these files only), for loading the NDI Runtime when installed (render/NdiOutput).
target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/ndi/include)
# DeckLink SDK 12 headers (Blackmagic's permissive licence), for the Desktop Video driver's API when installed
# (render/DeckLinkOutput; not on Windows yet).
if(APPLE)
  target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/decklink/mac)
elseif(NOT WIN32)
  target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/decklink/linux)
endif()

if(MONTAGE_WITH_VST3)
  # The VST 3 interfaces and the few SDK host helpers Montage uses (MIT).
  set(VST3SDK ${PROJECT_SOURCE_DIR}/third_party/vst3sdk)
  set(MONTAGE_VST3SDK_SOURCES
    ${VST3SDK}/pluginterfaces/base/funknown.cpp
    ${VST3SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3SDK}/public.sdk/source/vst/vstinitiids.cpp
    ${VST3SDK}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${VST3SDK}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${VST3SDK}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${VST3SDK}/public.sdk/source/vst/utility/stringconvert.cpp
    ${VST3SDK}/public.sdk/source/common/commonstringconvert.cpp
    ${VST3SDK}/public.sdk/source/common/memorystream.cpp)
  set_source_files_properties(${MONTAGE_VST3SDK_SOURCES} PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>")
  target_sources(montage_engine PRIVATE audio/Vst3Host.cpp ${MONTAGE_VST3SDK_SOURCES})
  target_include_directories(montage_engine PRIVATE ${VST3SDK})
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_VST3)
endif()
target_compile_definitions(montage_engine PUBLIC MONTAGE_VERSION="${PROJECT_VERSION}")
target_link_libraries(montage_engine PUBLIC Qt6::Core Qt6::Gui Qt6::Xml PkgConfig::FFMPEG Threads::Threads ${CMAKE_DL_LIBS})
target_link_libraries(montage_engine PRIVATE Qt6::Network)  # (OGraf graphics: the browser's DevTools socket)
# The GPU compositor (render/GpuCompositor.h): Qt's RHI, public from Qt 6.6 (in the private GUI module, which Qt 6.9 and
# later only load when asked for by name). Older Qt builds render on the CPU, as before.
if(MONTAGE_GPU AND Qt6_VERSION VERSION_GREATER_EQUAL 6.6)
  if(NOT TARGET Qt6::GuiPrivate)
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
  endif()
  if(TARGET Qt6::GuiPrivate)
    target_link_libraries(montage_engine PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_GPU)
    message(STATUS "GPU compositor: on (Qt ${Qt6_VERSION})")
  else()
    message(STATUS "GPU compositor: off (Qt's private GUI module was not found)")
  endif()
else()
  message(STATUS "GPU compositor: off (Qt ${Qt6_VERSION}; it needs 6.6 or later)")
endif()
if(APPLE)
  target_sources(montage_engine PRIVATE audio/AuHost.cpp)
  target_link_libraries(montage_engine PUBLIC "-framework AudioToolbox" "-framework CoreFoundation" "-framework CoreMIDI" "-framework IOKit")
endif()
if(WIN32)
  target_link_libraries(montage_engine PUBLIC winmm setupapi hid)  # MIDI (control/Midi.cpp), HID (control/Hid.cpp)
endif()
target_compile_options(montage_engine PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)
set_target_properties(montage_engine PROPERTIES AUTOMOC OFF)
# Spelling word lists (core/SpellCheck.h), built in.
qt_add_resources(montage_engine "dictionaries" PREFIX "/montage/dict" BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/dict"
  FILES resources/dict/english.txt resources/dict/american.txt resources/dict/british.txt)
# The font DCP subtitles are drawn in (render/Dcp.h; SIL OFL 1.1, LICENSES/OFL-1.1-Liberation.txt).
qt_add_resources(montage_engine "fonts" PREFIX "/montage/fonts" BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/fonts"
  FILES resources/fonts/LiberationSans-Regular.ttf)

if(MONTAGE_WITH_LV2)
  pkg_check_modules(LILV IMPORTED_TARGET GLOBAL lilv-0>=0.24)
  if(LILV_FOUND)
    message(STATUS "lilv ${LILV_VERSION}: LV2 hosting enabled")
    target_sources(montage_engine PRIVATE audio/Lv2Host.cpp)
    target_link_libraries(montage_engine PRIVATE PkgConfig::LILV)
    target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_LV2)
  else()
    message(STATUS "lilv not found: LV2 plugins are listed but not hosted")
  endif()
endif()
# Capture devices (cameras and sound cards) through libavdevice; network feeds need only libavformat.
pkg_check_modules(AVDEVICE IMPORTED_TARGET GLOBAL libavdevice)
if(AVDEVICE_FOUND)
  message(STATUS "libavdevice ${AVDEVICE_VERSION}: recording from capture devices enabled")
  target_link_libraries(montage_engine PRIVATE PkgConfig::AVDEVICE)
  target_compile_definitions(montage_engine PRIVATE MONTAGE_WITH_AVDEVICE)
else()
  message(STATUS "libavdevice not found: recording from network feeds only")
endif()
if(MONTAGE_WITH_LIBRAW)
  pkg_check_modules(LIBRAW IMPORTED_TARGET GLOBAL libraw>=0.20)
  if(LIBRAW_FOUND)
    message(STATUS "LibRaw ${LIBRAW_VERSION}: camera raw stills enabled")
    target_link_libraries(montage_engine PRIVATE PkgConfig::LIBRAW)
    target_compile_definitions(montage_engine PRIVATE MONTAGE_WITH_LIBRAW)
  else()
    message(STATUS "LibRaw not found: camera raw stills are not read")
  endif()
endif()
if(MONTAGE_WITH_MYSOFA)
  # (Not as an imported target: MSYS2's libmysofa.pc names "/ucrt64/include", a path CMake cannot see.)
  pkg_check_modules(MYSOFA libmysofa>=1.0)
  if(MYSOFA_FOUND)
    # SOFA files are read from memory (a zip's entry), which older libmysofa cannot.
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_INCLUDES ${MYSOFA_INCLUDE_DIRS})
    set(CMAKE_REQUIRED_LIBRARIES ${MYSOFA_LINK_LIBRARIES})
    check_symbol_exists(mysofa_load_data mysofa.h MONTAGE_MYSOFA_LOAD_DATA)
    unset(CMAKE_REQUIRED_INCLUDES)
    unset(CMAKE_REQUIRED_LIBRARIES)
  endif()
  if(MYSOFA_FOUND AND MONTAGE_MYSOFA_LOAD_DATA)
    message(STATUS "libmysofa ${MYSOFA_VERSION}: SOFA HRTFs for binaural ambisonics enabled")
    foreach(dir ${MYSOFA_INCLUDE_DIRS})
      if(EXISTS "${dir}")
        target_include_directories(montage_engine PRIVATE "${dir}")
      endif()
    endforeach()
    target_compile_options(montage_engine PRIVATE ${MYSOFA_CFLAGS_OTHER})
    target_link_libraries(montage_engine PRIVATE ${MYSOFA_LINK_LIBRARIES})
    target_compile_definitions(montage_engine PRIVATE MONTAGE_WITH_MYSOFA)
  else()
    message(STATUS "libmysofa (with mysofa_load_data) not found: ambisonics is heard binaurally through the spherical head model only")
  endif()
endif()
if(MONTAGE_WITH_OPENJPEG)
  pkg_check_modules(OPENJPEG IMPORTED_TARGET GLOBAL libopenjp2>=2.5)
  if(OPENJPEG_FOUND)
    message(STATUS "OpenJPEG ${OPENJPEG_VERSION}: IMF export enabled")
    target_link_libraries(montage_engine PRIVATE PkgConfig::OPENJPEG)
    target_compile_definitions(montage_engine PRIVATE MONTAGE_WITH_OPENJPEG)
  else()
    message(STATUS "OpenJPEG 2.5 not found: IMF masters cannot be made")
  endif()
endif()
if(MONTAGE_WITH_OPENSSL)
  find_package(OpenSSL 3 QUIET COMPONENTS Crypto)
  if(OPENSSL_FOUND)
    message(STATUS "OpenSSL ${OPENSSL_VERSION}: encrypted DCPs and KDMs enabled")
    target_link_libraries(montage_engine PRIVATE OpenSSL::Crypto)
    target_compile_definitions(montage_engine PRIVATE MONTAGE_WITH_OPENSSL)
  else()
    message(STATUS "OpenSSL 3 not found: DCPs cannot be encrypted")
  endif()
endif()
if(MONTAGE_WITH_OCIO)
  find_package(OpenColorIO 2.1 CONFIG QUIET)
  if(OpenColorIO_FOUND)
    message(STATUS "OpenColorIO ${OpenColorIO_VERSION}: OCIO transforms enabled")
    target_link_libraries(montage_engine PRIVATE OpenColorIO::OpenColorIO)
    target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_OCIO)
  else()
    # Debian/Ubuntu install the CMake package where find_package does not look.
    pkg_check_modules(OCIO IMPORTED_TARGET OpenColorIO>=2.1)
  endif()
  if(OCIO_FOUND)
    message(STATUS "OpenColorIO ${OCIO_VERSION} (pkg-config): OCIO transforms enabled")
    target_link_libraries(montage_engine PRIVATE PkgConfig::OCIO)
    target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_OCIO)
  elseif(NOT OpenColorIO_FOUND)
    message(STATUS "OpenColorIO not found: OCIO transforms disabled")
  endif()
endif()
if(MONTAGE_WITH_ONNXRUNTIME)
  # Microsoft's release archive (unpacked at ONNXRUNTIME_ROOT, or /opt/onnxruntime)
  # is found by its header and library: its CMake and pkg-config files name a
  # lib64 folder it does not have. Homebrew ships a CMake package, MSYS2 a
  # pkg-config file.
  if(NOT ONNXRUNTIME_ROOT)
    if(DEFINED ENV{ONNXRUNTIME_ROOT})
      set(ONNXRUNTIME_ROOT $ENV{ONNXRUNTIME_ROOT})
    elseif(EXISTS /opt/onnxruntime/include/onnxruntime_cxx_api.h)
      set(ONNXRUNTIME_ROOT /opt/onnxruntime)
    endif()
  endif()
  set(MONTAGE_ORT_TARGET "")
  if(ONNXRUNTIME_ROOT)
    find_path(ORT_INCLUDE_DIR onnxruntime_cxx_api.h PATHS ${ONNXRUNTIME_ROOT}
      PATH_SUFFIXES include include/onnxruntime NO_DEFAULT_PATH)
    find_library(ORT_LIBRARY onnxruntime PATHS ${ONNXRUNTIME_ROOT} PATH_SUFFIXES lib lib64 NO_DEFAULT_PATH)
    if(ORT_INCLUDE_DIR AND ORT_LIBRARY)
      add_library(montage_onnxruntime INTERFACE)
      target_include_directories(montage_onnxruntime INTERFACE ${ORT_INCLUDE_DIR})
      target_link_libraries(montage_onnxruntime INTERFACE ${ORT_LIBRARY})
      set(MONTAGE_ORT_TARGET montage_onnxruntime)
      message(STATUS "ONNX Runtime (${ORT_LIBRARY}): object masks enabled")
    endif()
  endif()
  if(NOT MONTAGE_ORT_TARGET)
    find_package(onnxruntime CONFIG QUIET)
    if(onnxruntime_FOUND AND TARGET onnxruntime::onnxruntime)
      set(MONTAGE_ORT_TARGET onnxruntime::onnxruntime)
      message(STATUS "ONNX Runtime ${onnxruntime_VERSION}: object masks enabled")
    else()
      pkg_check_modules(ORT IMPORTED_TARGET libonnxruntime>=1.16)
      if(ORT_FOUND)
        set(MONTAGE_ORT_TARGET PkgConfig::ORT)
        message(STATUS "ONNX Runtime ${ORT_VERSION} (pkg-config): object masks enabled")
      endif()
    endif()
  endif()
  if(MONTAGE_ORT_TARGET)
    target_link_libraries(montage_engine PRIVATE ${MONTAGE_ORT_TARGET})
    target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_ONNXRUNTIME)
    if(WIN32)
      find_file(MONTAGE_ORT_DLL_PATH NAMES onnxruntime.dll libonnxruntime.dll
        HINTS ${ONNXRUNTIME_ROOT}/lib ${ONNXRUNTIME_ROOT}/bin ${ORT_PREFIX}/bin ${onnxruntime_DIR}/../../../bin
        PATH_SUFFIXES bin NO_CACHE)
      set(MONTAGE_ORT_DLL "${MONTAGE_ORT_DLL_PATH}" CACHE INTERNAL "ONNX Runtime DLL copied beside the programs")
      message(STATUS "ONNX Runtime DLL: ${MONTAGE_ORT_DLL}")
    endif()
  elseif(MONTAGE_REQUIRE_ONNXRUNTIME)
    message(FATAL_ERROR "ONNX Runtime not found (set ONNXRUNTIME_ROOT)")
  else()
    message(STATUS "ONNX Runtime not found: object masks can be shown but not tracked")
  endif()
endif()
if(MONTAGE_WITH_C2PA)
  # c2pa-rs's C API as a static library (scripts/build-c2pa.sh), with the system libraries rustc says it needs (in
  # its order, repeats kept: the Windows GNU toolchain's runtime libraries need them).
  if(NOT C2PA_ROOT)
    if(DEFINED ENV{C2PA_ROOT})
      set(C2PA_ROOT $ENV{C2PA_ROOT})
    elseif(EXISTS /opt/c2pa/include/c2pa.h)
      set(C2PA_ROOT /opt/c2pa)
    endif()
  endif()
  if(C2PA_ROOT)
    find_path(C2PA_INCLUDE_DIR c2pa.h PATHS ${C2PA_ROOT}/include NO_DEFAULT_PATH)
    find_file(C2PA_LIBRARY libc2pa_c.a PATHS ${C2PA_ROOT}/lib NO_DEFAULT_PATH)
  endif()
  if(C2PA_INCLUDE_DIR AND C2PA_LIBRARY)
    set(C2PA_NATIVE "")
    if(EXISTS ${C2PA_ROOT}/lib/c2pa-native-libs.txt)
      file(READ ${C2PA_ROOT}/lib/c2pa-native-libs.txt C2PA_NATIVE_TEXT)
      string(STRIP "${C2PA_NATIVE_TEXT}" C2PA_NATIVE_TEXT)
      separate_arguments(C2PA_NATIVE_ARGS UNIX_COMMAND "${C2PA_NATIVE_TEXT}")
      set(framework OFF)
      foreach(arg IN LISTS C2PA_NATIVE_ARGS)
        if(framework)
          list(APPEND C2PA_NATIVE "-framework ${arg}")
          set(framework OFF)
        elseif(arg STREQUAL "-framework")
          set(framework ON)
        elseif(arg MATCHES "^-l(.+)$")
          list(APPEND C2PA_NATIVE ${CMAKE_MATCH_1})
        elseif(arg MATCHES "^(.+)\\.lib$")
          list(APPEND C2PA_NATIVE ${CMAKE_MATCH_1})
        endif()
      endforeach()
    endif()
    add_library(montage_c2pa INTERFACE)
    target_include_directories(montage_c2pa INTERFACE ${C2PA_INCLUDE_DIR})
    # A static library: its functions are not imported from a DLL (c2pa.h declares them dllimport on Windows otherwise).
    target_compile_definitions(montage_c2pa INTERFACE C2PA_DYNAMIC_LOADING=1)
    target_link_libraries(montage_c2pa INTERFACE ${C2PA_LIBRARY} ${C2PA_NATIVE})
    target_link_libraries(montage_engine PRIVATE montage_c2pa)
    target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_C2PA)
    message(STATUS "C2PA (${C2PA_LIBRARY}; ${C2PA_NATIVE}): Content Credentials enabled")
  elseif(MONTAGE_REQUIRE_C2PA)
    message(FATAL_ERROR "C2PA library not found (run scripts/build-c2pa.sh and set C2PA_ROOT)")
  else()
    message(STATUS "C2PA library not found: exports are not signed with Content Credentials")
  endif()
endif()
if(MONTAGE_WITH_OPENEXR)
  target_link_libraries(montage_engine PRIVATE OpenEXR::OpenEXR)
  # OpenJPH's own API (HTJ2K codestreams for IMF), compiled into OpenEXRCore.
  target_include_directories(montage_engine PRIVATE ${openexr_SOURCE_DIR}/external/OpenJPH/src/core/common
                                                    ${openexr_SOURCE_DIR}/external/OpenJPH/src/core/openjph)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_OPENEXR)
endif()
if(MONTAGE_WITH_BMX)
  target_sources(montage_engine PRIVATE render/MxfFiles.h render/MxfFiles.cpp)
  target_link_libraries(montage_engine PRIVATE bmx)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_BMX)
endif()
if(MONTAGE_WITH_THORVG)
  target_link_libraries(montage_engine PRIVATE montage_thorvg)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_THORVG)
endif()

if(MONTAGE_WITH_LUA)
  target_link_libraries(montage_engine PRIVATE montage_lua)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_LUA)
endif()

if(MONTAGE_WITH_RNNOISE)
  target_link_libraries(montage_engine PRIVATE montage_rnnoise)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_RNNOISE)
endif()

# Without whisper the transcriber reports that speech recognition is unavailable.
target_sources(montage_engine PRIVATE media/Transcriber.h media/Transcriber.cpp)

# RIFE's graph without its weights (scripts/gen-rife-graph.py), compiled in as bytes.
set(MONTAGE_RIFE_GRAPH ${CMAKE_CURRENT_SOURCE_DIR}/media/rife-4.26.onnx)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MONTAGE_RIFE_GRAPH})
file(READ ${MONTAGE_RIFE_GRAPH} MONTAGE_RIFE_HEX HEX)
file(SIZE ${MONTAGE_RIFE_GRAPH} MONTAGE_RIFE_SIZE)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MONTAGE_RIFE_BYTES "${MONTAGE_RIFE_HEX}")
configure_file(media/RifeGraph.cpp.in ${CMAKE_CURRENT_BINARY_DIR}/RifeGraph.cpp @ONLY)
target_sources(montage_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/RifeGraph.cpp)
# Spleeter's graphs without their weights (scripts/gen-spleeter-graph.py), compiled in the same way.
foreach(stems 2 4 5)
  set(graph ${CMAKE_CURRENT_SOURCE_DIR}/media/spleeter-${stems}stems.onnx)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${graph})
  file(READ ${graph} graph_hex HEX)
  file(SIZE ${graph} graph_size)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," graph_bytes "${graph_hex}")
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/SpleeterGraph${stems}.cpp.new
       "// Generated by CMake from media/spleeter-${stems}stems.onnx.\n#include <cstddef>\n\n"
       "extern const unsigned char kSpleeter${stems}Graph[] = {${graph_bytes}};\n"
       "extern const size_t kSpleeter${stems}GraphSize = ${graph_size};\n")
  configure_file(${CMAKE_CURRENT_BINARY_DIR}/SpleeterGraph${stems}.cpp.new ${CMAKE_CURRENT_BINARY_DIR}/SpleeterGraph${stems}.cpp COPYONLY)
  target_sources(montage_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/SpleeterGraph${stems}.cpp)
endforeach()
# Bandit v2's graph without its weights (scripts/gen-bandit-graph.py), the same way.
set(graph ${CMAKE_CURRENT_SOURCE_DIR}/media/bandit-v2-eng.onnx)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${graph})
file(READ ${graph} graph_hex HEX)
file(SIZE ${graph} graph_size)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," graph_bytes "${graph_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/BanditGraph.cpp.new
     "// Generated by CMake from media/bandit-v2-eng.onnx.\n#include <cstddef>\n\n"
     "extern const unsigned char kBanditEngGraph[] = {${graph_bytes}};\n"
     "extern const size_t kBanditEngGraphSize = ${graph_size};\n")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/BanditGraph.cpp.new ${CMAKE_CURRENT_BINARY_DIR}/BanditGraph.cpp COPYONLY)
target_sources(montage_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/BanditGraph.cpp)
# The lip-sync networks without their weights (scripts/gen-lipsync-graphs.py), the same way.
set(lipsync_cpp "// Generated by CMake from media/lipsync-*.onnx.\n#include <cstddef>\n\n")
foreach(part Unet:unet VaeEncoder:vae-encoder VaeDecoder:vae-decoder Audio:audio)
  string(REPLACE ":" ";" part "${part}")
  list(GET part 0 symbol)
  list(GET part 1 file)
  set(graph ${CMAKE_CURRENT_SOURCE_DIR}/media/lipsync-${file}.onnx)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${graph})
  file(READ ${graph} graph_hex HEX)
  file(SIZE ${graph} graph_size)
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," graph_bytes "${graph_hex}")
  string(APPEND lipsync_cpp "extern const unsigned char kLipSync${symbol}Graph[] = {${graph_bytes}};\n"
                            "extern const size_t kLipSync${symbol}GraphSize = ${graph_size};\n")
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/LipSyncGraphs.cpp.new "${lipsync_cpp}")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/LipSyncGraphs.cpp.new ${CMAKE_CURRENT_BINARY_DIR}/LipSyncGraphs.cpp COPYONLY)
target_sources(montage_engine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/LipSyncGraphs.cpp)
if(MONTAGE_WITH_WHISPER)
  target_link_libraries(montage_engine PRIVATE whisper)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_WHISPER)
endif()

add_executable(montage-cli cli/main.cpp)
target_link_libraries(montage-cli PRIVATE montage_engine)
if(MONTAGE_BUILD_APP)
  # `montage-cli mcp --live`: the stdio bridge to a running app's agent link (app/LiveBridge).
  target_sources(montage-cli PRIVATE app/LiveBridge.cpp app/LiveBridge.h)
  target_link_libraries(montage-cli PRIVATE Qt6::Network)
  target_compile_definitions(montage-cli PRIVATE MONTAGE_LIVE_BRIDGE=1)
endif()
montage_copy_runtime(montage-cli)
set_target_properties(montage-cli PROPERTIES AUTOMOC OFF)
install(TARGETS montage-cli RUNTIME DESTINATION bin)

if(MONTAGE_BUILD_APP)
  add_subdirectory(app)
endif()

# Loads plugin files in a separate process while scanning (crash isolation).
# Built next to the editor so the app finds it beside itself.
add_executable(montage-plugin-probe tools/plugin_probe.cpp)
target_link_libraries(montage-plugin-probe PRIVATE montage_engine)
montage_copy_runtime(montage-plugin-probe)
set_target_properties(montage-plugin-probe PROPERTIES AUTOMOC OFF
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/app)
install(TARGETS montage-plugin-probe RUNTIME DESTINATION bin)
