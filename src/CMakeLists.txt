add_library(montage_engine STATIC
  core/Model.h core/Model.cpp
  core/ObjectMask.h core/ObjectMask.cpp
  core/Transcript.h core/Transcript.cpp
  core/Captions.h core/Captions.cpp
  core/TranscriptEdit.h core/TranscriptEdit.cpp
  core/Effects.h core/Effects.cpp
  core/EditOps.h core/EditOps.cpp
  core/ImportBuilder.h core/InterchangeImport.cpp core/FcpXml.cpp
  core/Multicam.h core/Multicam.cpp
  core/History.h core/History.cpp
  core/ProjectIO.h core/ProjectIO.cpp
  core/Interchange.h core/Interchange.cpp
  automation/McpServer.h automation/McpServer.cpp
  media/Image.h media/Image.cpp
  media/Decoder.h media/Decoder.cpp
  media/HwAccel.h media/HwAccel.cpp
  media/MediaPool.h media/MediaPool.cpp
  media/Analysis.h media/Analysis.cpp
  media/Loudness.h media/Loudness.cpp
  media/AudioSync.h media/AudioSync.cpp
  media/SpeakerSwitch.h media/SpeakerSwitch.cpp
  media/Tracking.h media/Tracking.cpp
  media/ModelFiles.h media/ModelFiles.cpp
  media/Segmenter.h media/Segmenter.cpp
  media/Diarizer.h media/Diarizer.cpp
  render/Processing.h render/Processing.cpp
  render/Retime.h render/Retime.cpp
  render/ClipAnalysis.h render/ClipAnalysis.cpp
  render/ColorSpace.h render/ColorSpace.cpp
  render/Ocio.h render/Ocio.cpp
  render/Compositor.h render/Compositor.cpp
  render/Exporter.h render/Exporter.cpp
  audio/Plugins.h audio/PluginScan.cpp audio/ClapHost.cpp
  audio/DynLib.h audio/DynLib.cpp
  audio/PluginEffect.h audio/PluginEffect.cpp
  audio/SpeechCleanup.h audio/SpeechCleanup.cpp
)
target_include_directories(montage_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(montage_engine PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)

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
if(MONTAGE_WITH_RNNOISE)
  target_link_libraries(montage_engine PRIVATE montage_rnnoise)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_RNNOISE)
endif()

# Without whisper the transcriber reports that speech recognition is unavailable.
target_sources(montage_engine PRIVATE media/Transcriber.h media/Transcriber.cpp)
if(MONTAGE_WITH_WHISPER)
  target_link_libraries(montage_engine PRIVATE whisper)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_WHISPER)
endif()

add_executable(montage-cli cli/main.cpp)
target_link_libraries(montage-cli PRIVATE montage_engine)
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
