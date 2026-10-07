add_library(montage_engine STATIC
  core/Model.h core/Model.cpp
  core/Effects.h core/Effects.cpp
  core/EditOps.h core/EditOps.cpp
  core/History.h core/History.cpp
  core/ProjectIO.h core/ProjectIO.cpp
  core/Interchange.h core/Interchange.cpp
  media/Image.h media/Image.cpp
  media/Decoder.h media/Decoder.cpp
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
target_compile_definitions(montage_engine PUBLIC MONTAGE_VERSION="${PROJECT_VERSION}")
target_link_libraries(montage_engine PUBLIC Qt6::Core Qt6::Gui PkgConfig::FFMPEG Threads::Threads ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_libraries(montage_engine PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()
target_compile_options(montage_engine PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)
set_target_properties(montage_engine PROPERTIES AUTOMOC OFF)

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
