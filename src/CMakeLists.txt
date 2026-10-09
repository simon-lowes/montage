add_library(montage_engine STATIC
  core/Model.h core/Model.cpp
  core/MediaLog.h core/MediaLog.cpp
  core/KeyframeEdit.h core/KeyframeEdit.cpp
  core/MaskPath.h core/MaskPath.cpp
  core/ColorWarp.h core/ColorWarp.cpp
  core/ObjectMask.h core/ObjectMask.cpp
  core/VisualIndex.h core/VisualIndex.cpp
  core/AutoTag.h core/AutoTag.cpp
  core/Transcript.h core/Transcript.cpp
  core/Captions.h core/Captions.cpp
  core/CaptionFormats.cpp
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
  core/Surround.h core/Surround.cpp
  core/Effects.h core/Effects.cpp
  core/EditOps.h core/EditOps.cpp
  core/TimelineCompare.h core/TimelineCompare.cpp
  core/MergeClips.h core/MergeClips.cpp
  core/ClipAnimation.h core/ClipAnimation.cpp
  core/GradeVersions.h core/GradeVersions.cpp
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
  core/Interchange.h core/Interchange.cpp
  automation/McpServer.h automation/McpServer.cpp
  media/Image.h media/Image.cpp
  media/Decoder.h media/Decoder.cpp
  media/Vector.h media/Vector.cpp
  media/CameraRaw.h media/CameraRaw.cpp
  media/SuperScale.h media/SuperScale.cpp
  media/Faces.h media/Faces.cpp
  media/FaceTracks.h media/FaceTracks.cpp
  media/DepthMap.h media/DepthMap.cpp
  media/Matting.h media/Matting.cpp
  media/TextToSpeech.h media/TextToSpeech.cpp
  media/Inpaint.h media/Inpaint.cpp
  media/ValueMap.h
  media/Rife.h media/Rife.cpp
  media/HwAccel.h media/HwAccel.cpp
  media/MediaPool.h media/MediaPool.cpp
  media/Relink.h media/Relink.cpp
  media/Analysis.h media/Analysis.cpp
  media/Loudness.h media/Loudness.cpp
  media/AudioSync.h media/AudioSync.cpp
  media/FieldRecorder.h media/FieldRecorder.cpp
  media/ImageSequence.h media/ImageSequence.cpp
  media/Interpret.h media/Interpret.cpp
  media/Psd.h media/Psd.cpp
  media/DualSystem.h media/DualSystem.cpp
  media/SpeakerSwitch.h media/SpeakerSwitch.cpp
  media/AutoDuck.h media/AutoDuck.cpp
  media/MicBleed.h media/MicBleed.cpp
  media/Tracking.h media/Tracking.cpp
  media/Reframe.h media/Reframe.cpp
  media/ModelFiles.h media/ModelFiles.cpp
  media/Segmenter.h media/Segmenter.cpp
  media/Diarizer.h media/Diarizer.cpp
  media/VisualSearch.h media/VisualSearch.cpp
  media/SpeechSearch.h media/SpeechSearch.cpp
  media/TextReader.h media/TextReader.cpp
  media/SpeechEnhance.h media/SpeechEnhance.cpp
  media/Beats.h media/Beats.cpp
  media/Translator.h media/Translator.cpp
  render/Processing.h render/Processing.cpp
  render/Ofx.h render/Ofx.cpp
  render/FaceRefine.h render/FaceRefine.cpp
  render/Relight.h render/Relight.cpp
  render/FilmLook.h render/FilmLook.cpp
  render/Retime.h render/Retime.cpp
  render/VideoDenoise.h render/VideoDenoise.cpp
  render/Shapes.h render/Shapes.cpp
  render/AafExport.h render/AafExport.cpp
  render/ClipAnalysis.h render/ClipAnalysis.cpp
  render/MusicEdit.h render/MusicEdit.cpp
  render/Highlights.h render/Highlights.cpp
  render/Shorts.h render/Shorts.cpp
  render/Letterbox.h render/Letterbox.cpp
  render/LightLevel.h render/LightLevel.cpp
  render/Versions.h render/Versions.cpp
  render/ReviewExport.h render/ReviewExport.cpp
  render/DcpMxf.h render/DcpMxf.cpp
  render/Dcp.h render/Dcp.cpp
  render/RoomTone.h render/RoomTone.cpp
  render/ExtendClip.h render/ExtendClip.cpp
  render/PaperEdit.h render/PaperEdit.cpp
  render/VfxPull.h render/VfxPull.cpp
  render/Spherical.h render/Spherical.cpp
  render/ClipPlacement.h render/ClipPlacement.cpp
  render/AudioReactive.h render/AudioReactive.cpp
  render/QualityCheck.h render/QualityCheck.cpp
  render/LutExport.h render/LutExport.cpp
  render/ProjectManager.h render/ProjectManager.cpp
  render/AutoBroll.h render/AutoBroll.cpp
  render/AutoMix.h render/AutoMix.cpp
  render/VoiceMatch.h render/VoiceMatch.cpp
  render/ColorSpace.h render/ColorSpace.cpp
  render/Ocio.h render/Ocio.cpp
  render/Compositor.h render/Compositor.cpp
  render/RenderCache.h render/RenderCache.cpp
  render/AudioFx.h render/AudioFx.cpp
  render/VideoFx.h render/VideoFx.cpp
  render/StyleFx.h render/StyleFx.cpp
  render/TemporalFx.h render/TemporalFx.cpp
  render/Deconvolve.h render/Deconvolve.cpp
  render/Exporter.h render/Exporter.cpp
  audio/Plugins.h audio/PluginScan.cpp audio/ClapHost.cpp
  audio/DynLib.h audio/DynLib.cpp
  audio/PluginEffect.h audio/PluginEffect.cpp
  audio/SpeechCleanup.h audio/SpeechCleanup.cpp
  audio/AudioRepair.h audio/AudioRepair.cpp
  audio/SpectralRepair.h audio/SpectralRepair.cpp
  audio/TimeStretch.h audio/TimeStretch.cpp
)
target_include_directories(montage_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)
# OpenFX 1.5 headers (BSD-3-Clause, the ASWF's), for hosting video plugins (render/Ofx).
target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/openfx/include)

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
if(APPLE)
  target_sources(montage_engine PRIVATE audio/AuHost.cpp)
  target_link_libraries(montage_engine PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()
target_compile_options(montage_engine PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)
set_target_properties(montage_engine PROPERTIES AUTOMOC OFF)
# Spelling word lists (core/SpellCheck.h), built in.
qt_add_resources(montage_engine "dictionaries" PREFIX "/montage/dict" BASE "${CMAKE_CURRENT_SOURCE_DIR}/resources/dict"
  FILES resources/dict/english.txt resources/dict/american.txt resources/dict/british.txt)

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
if(MONTAGE_WITH_THORVG)
  target_link_libraries(montage_engine PRIVATE montage_thorvg)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_THORVG)
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
