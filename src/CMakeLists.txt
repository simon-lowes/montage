add_library(montage_engine STATIC
  core/Model.h core/Model.cpp
  core/Transcript.h core/Transcript.cpp
  core/Captions.h core/Captions.cpp
  core/TranscriptEdit.h core/TranscriptEdit.cpp
  core/Effects.h core/Effects.cpp
  core/EditOps.h core/EditOps.cpp
  core/History.h core/History.cpp
  core/ProjectIO.h core/ProjectIO.cpp
  core/Interchange.h core/Interchange.cpp
  media/Image.h media/Image.cpp
  media/Decoder.h media/Decoder.cpp
  media/HwAccel.h media/HwAccel.cpp
  media/MediaPool.h media/MediaPool.cpp
  media/Analysis.h media/Analysis.cpp
  media/Loudness.h media/Loudness.cpp
  media/AudioSync.h media/AudioSync.cpp
  render/Processing.h render/Processing.cpp
  render/Compositor.h render/Compositor.cpp
  render/Exporter.h render/Exporter.cpp
  audio/Plugins.h audio/PluginScan.cpp audio/ClapHost.cpp
  audio/DynLib.h audio/DynLib.cpp
  audio/PluginEffect.h audio/PluginEffect.cpp
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
target_link_libraries(montage_engine PUBLIC Qt6::Core Qt6::Gui PkgConfig::FFMPEG Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(montage_engine PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()
target_compile_options(montage_engine PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)
set_target_properties(montage_engine PROPERTIES AUTOMOC OFF)

# Without whisper the transcriber reports that speech recognition is unavailable.
target_sources(montage_engine PRIVATE media/Transcriber.h media/Transcriber.cpp)
if(MONTAGE_WITH_WHISPER)
  target_link_libraries(montage_engine PRIVATE whisper)
  target_compile_definitions(montage_engine PUBLIC MONTAGE_WITH_WHISPER)
endif()

add_executable(montage-cli cli/main.cpp)
target_link_libraries(montage-cli PRIVATE montage_engine)
set_target_properties(montage-cli PROPERTIES AUTOMOC OFF)
install(TARGETS montage-cli RUNTIME DESTINATION bin)

if(MONTAGE_BUILD_APP)
  add_subdirectory(app)
endif()

# Loads plugin files in a separate process while scanning (crash isolation).
# Built next to the editor so the app finds it beside itself.
add_executable(montage-plugin-probe tools/plugin_probe.cpp)
target_link_libraries(montage-plugin-probe PRIVATE montage_engine)
set_target_properties(montage-plugin-probe PROPERTIES AUTOMOC OFF
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/app)
install(TARGETS montage-plugin-probe RUNTIME DESTINATION bin)
