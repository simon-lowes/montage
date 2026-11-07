# Qt Test's console output does not reach ctest on Windows, so there each run also writes its results to a file
# (<test>-ctest.txt), which the packaging workflow prints when a test fails.
function(montage_test_command name out)
  if(WIN32)
    set(${out} ${name} -o ${name}-ctest.txt,txt -o -,txt PARENT_SCOPE)
  else()
    set(${out} ${name} PARENT_SCOPE)
  endif()
endfunction()

function(montage_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE montage_engine Qt6::Test)
  set_target_properties(${name} PROPERTIES AUTOMOC ON)
  montage_copy_runtime(${name})
  montage_test_command(${name} command)
  add_test(NAME ${name} COMMAND ${command})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
endfunction()

montage_test(test_core)
montage_test(test_render)
target_link_libraries(test_render PRIVATE Qt6::Network)  # (free ports for the remote review stream)
# (The DCP tests alone take minutes on a slow container: 317 s in the nightly build's in October 2026.) Qt 6.4's
# watchdog stops a run 300 s after it starts, whichever function it is in, so the run is given as long as ctest gives it.
set_tests_properties(test_render PROPERTIES TIMEOUT 900 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QTEST_FUNCTION_TIMEOUT=900000")
montage_test(test_media)
target_link_libraries(test_media PRIVATE Qt6::Network)  # (a free port for the capture tests)
if(TARGET OpenEXR::OpenEXR)
  target_link_libraries(test_media PRIVATE OpenEXR::OpenEXR)  # (writing multi-part files)
endif()
# Several minutes of real encoding, decoding and model runs: slow CI runners need more than the default (macOS
# runners took 846 s on main in October 2026, and over 900 s on a slower one; a learned effect's training test adds a
# few minutes more).
set_tests_properties(test_media PROPERTIES TIMEOUT 2100)
target_compile_definitions(test_media PRIVATE MONTAGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                           MONTAGE_TEST_TOOLS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tools")
target_compile_definitions(test_core PRIVATE MONTAGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
target_compile_definitions(test_render PRIVATE MONTAGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
# hls.js (Apache-2.0) and dash.js (BSD-3-Clause), test tools only (never shipped): test_render plays streaming packages
# through them in a headless browser. Fetched once; without them (offline) that test skips.
option(MONTAGE_TEST_STREAMING_PLAYERS "Fetch hls.js and dash.js for the streaming playback test" ON)
if(MONTAGE_TEST_STREAMING_PLAYERS)
  set(players ${CMAKE_BINARY_DIR}/streaming-players)
  foreach(pkg "hls.js|1.5.20|3791f532701590ec10ecb4a2abf6ea63c65ae9ce91de4cc6f6553ba4629605ce|dist/hls.min.js"
              "dashjs|4.7.4|62a9c283837fc4600e35d54cac7dfd73361e779ea657e894f8d7cae5d3ba78ed|dist/dash.all.min.js")
    string(REPLACE "|" ";" pkg "${pkg}")
    list(GET pkg 0 name)
    list(GET pkg 1 version)
    list(GET pkg 2 hash)
    list(GET pkg 3 file)
    get_filename_component(leaf ${file} NAME)
    if(NOT EXISTS ${players}/${leaf})
      # (the hash checked here, not by file(DOWNLOAD), so a wrong file skips the test instead of stopping the configure)
      file(DOWNLOAD https://registry.npmjs.org/${name}/-/${name}-${version}.tgz ${players}/${name}.tgz STATUS got TLS_VERIFY ON)
      list(GET got 0 code)
      if(code EQUAL 0)
        file(SHA256 ${players}/${name}.tgz got_hash)
        if(NOT got_hash STREQUAL hash)
          set(code 1)
          set(got "SHA-256 ${got_hash}, not ${hash}")
        endif()
      endif()
      if(code EQUAL 0)
        file(ARCHIVE_EXTRACT INPUT ${players}/${name}.tgz DESTINATION ${players}/${name} PATTERNS package/${file})
        file(COPY ${players}/${name}/package/${file} DESTINATION ${players})
        file(REMOVE_RECURSE ${players}/${name} ${players}/${name}.tgz)
      else()
        message(STATUS "${name} ${version} not fetched (${got}): the streaming playback test will skip")
      endif()
    endif()
  endforeach()
  if(EXISTS ${players}/hls.min.js AND EXISTS ${players}/dash.all.min.js)
    target_compile_definitions(test_render PRIVATE MONTAGE_TEST_STREAMING_PLAYERS="${players}")
  endif()
endif()
if(MONTAGE_WITH_FDL)
  target_compile_definitions(test_render PRIVATE MONTAGE_TEST_FDL_DIR="${ascfdl_SOURCE_DIR}/resources/FDL")
endif()

if(MONTAGE_BUILD_APP)
  add_executable(test_app test_app.cpp)
  target_link_libraries(test_app PRIVATE montage_ui Qt6::Test)
  set_target_properties(test_app PROPERTIES AUTOMOC ON)
  montage_copy_runtime(test_app)
  montage_test_command(test_app command)
  add_test(NAME test_app COMMAND ${command})
  set_tests_properties(test_app PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MONTAGE_NO_PLUGIN_SCAN=1;MONTAGE_NO_RECOVERY_PROMPT=1;MONTAGE_RECOVERY_DIR=${CMAKE_CURRENT_BINARY_DIR}/recovery-test" TIMEOUT 600)
  add_dependencies(test_app montage_test_plugins montage_test_sidechain montage_test_ofx montage-plugin-probe)
  target_compile_definitions(test_app PRIVATE
    MONTAGE_TEST_CLAP_DIR="${CMAKE_CURRENT_BINARY_DIR}/clap"
    MONTAGE_TEST_OFX_DIR="${CMAKE_CURRENT_BINARY_DIR}/ofx"
    MONTAGE_PLUGIN_PROBE="$<TARGET_FILE:montage-plugin-probe>"
    MONTAGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    MONTAGE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  # Qt's lupdate, to check it finds every tr() text (everyTranslatableTextIsFound).
  find_program(MONTAGE_TEST_LUPDATE_BIN NAMES lupdate-qt6 lupdate
    HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}" /usr/lib/qt6/bin)
  if(MONTAGE_TEST_LUPDATE_BIN)
    set_property(TEST test_app APPEND PROPERTY ENVIRONMENT "MONTAGE_TEST_LUPDATE=${MONTAGE_TEST_LUPDATE_BIN}")
  endif()
endif()

# ---- Audio plugins: test CLAP plugins built as loadable *.clap modules ----
enable_language(C)
function(montage_test_clap target output dir source)
  add_library(${target} MODULE ${source})
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".clap" OUTPUT_NAME ${output}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/clap/${dir}
    C_VISIBILITY_PRESET hidden)
endfunction()
montage_test_clap(montage_test_plugins MontageTestPlugins good plugins/test_clap_plugins.c)
montage_test_clap(montage_test_hang MontageTestHang hang plugins/test_clap_misbehaving.c)
target_compile_definitions(montage_test_hang PRIVATE MONTAGE_TEST_HANG)
montage_test_clap(montage_test_crash MontageTestCrash crash plugins/test_clap_misbehaving.c)
target_compile_definitions(montage_test_crash PRIVATE MONTAGE_TEST_CRASH)
montage_test_clap(montage_test_latency MontageTestLatency latency plugins/test_clap_latency.c)
montage_test_clap(montage_test_convolver MontageTestConvolver convolver plugins/test_clap_convolver.c)
montage_test_clap(montage_test_sidechain MontageTestSidechain sidechain plugins/test_clap_sidechain.c)
if(NOT WIN32 AND NOT APPLE)
  target_link_libraries(montage_test_sidechain PRIVATE m)
endif()

# ---- Video plugins: test OpenFX plugins built as *.ofx.bundle folders ----
if(WIN32)
  set(MONTAGE_OFX_ARCH Win64)
elseif(APPLE)
  set(MONTAGE_OFX_ARCH MacOS)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
  set(MONTAGE_OFX_ARCH Linux-aarch64)
else()
  set(MONTAGE_OFX_ARCH Linux-x86-64)
endif()
function(montage_test_ofx target output dir)
  add_library(${target} MODULE plugins/test_ofx_plugins.c)
  target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/third_party/openfx/include)
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME ${output}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ofx/${dir}/${output}.ofx.bundle/Contents/${MONTAGE_OFX_ARCH}
    C_VISIBILITY_PRESET hidden)
endfunction()
montage_test_ofx(montage_test_ofx MontageTestOfx good)
# Test Invert's OpenGL render and OpenGL on-screen controls (Linux: the OpenGL calls come from libGL).
find_package(OpenGL QUIET)
if(TARGET OpenGL::GL AND UNIX AND NOT APPLE)
  target_compile_definitions(montage_test_ofx PRIVATE MONTAGE_TEST_OFX_GL)
  target_link_libraries(montage_test_ofx PRIVATE OpenGL::GL ${CMAKE_DL_LIBS})
endif()
montage_test_ofx(montage_test_ofx_crash MontageTestOfxCrash crash)
target_compile_definitions(montage_test_ofx_crash PRIVATE MONTAGE_TEST_OFX_CRASH)
# (on-screen controls and OpenGL renders, in the render tests the GPU job runs)
add_dependencies(test_render montage_test_ofx montage-plugin-probe)
target_compile_definitions(test_render PRIVATE MONTAGE_TEST_OFX_DIR="${CMAKE_CURRENT_BINARY_DIR}/ofx"
  MONTAGE_PLUGIN_PROBE="$<TARGET_FILE:montage-plugin-probe>")

montage_test(test_plugins)
add_dependencies(test_plugins montage_test_plugins montage_test_hang montage_test_crash montage_test_latency montage_test_sidechain
  montage_test_convolver montage-plugin-probe montage-pluginhost montage_test_ofx montage_test_ofx_crash)
target_compile_definitions(test_plugins PRIVATE
  MONTAGE_TEST_CLAP_DIR="${CMAKE_CURRENT_BINARY_DIR}/clap"
  MONTAGE_TEST_OFX_DIR="${CMAKE_CURRENT_BINARY_DIR}/ofx"
  MONTAGE_PLUGIN_PROBE="$<TARGET_FILE:montage-plugin-probe>"
  MONTAGE_PLUGIN_HOST="$<TARGET_FILE:montage-pluginhost>")

if(TARGET PkgConfig::LILV)
  # An LV2 gain with latency, as a bundle (shared module + Turtle files).
  set(LV2_BUNDLE ${CMAKE_CURRENT_BINARY_DIR}/lv2/montage-gain.lv2)
  add_library(montage_test_lv2 MODULE plugins/test_lv2_gain.c)
  target_include_directories(montage_test_lv2 PRIVATE $<TARGET_PROPERTY:PkgConfig::LILV,INTERFACE_INCLUDE_DIRECTORIES>)
  set_target_properties(montage_test_lv2 PROPERTIES PREFIX "" OUTPUT_NAME montage_test_lv2_gain
    LIBRARY_OUTPUT_DIRECTORY ${LV2_BUNDLE} C_VISIBILITY_PRESET hidden)
  if(NOT WIN32)
    target_link_libraries(montage_test_lv2 PRIVATE m)
  endif()
  configure_file(plugins/lv2/manifest.ttl.in ${LV2_BUNDLE}/manifest.ttl @ONLY)
  configure_file(plugins/lv2/gain.ttl ${LV2_BUNDLE}/gain.ttl COPYONLY)
  add_dependencies(test_plugins montage_test_lv2)
  target_compile_definitions(test_plugins PRIVATE MONTAGE_TEST_LV2_BUNDLE="${LV2_BUNDLE}")
endif()

if(MONTAGE_WITH_VST3)
  # A VST3 gain written against the SDK interfaces, built as one .vst3 file.
  set(VST3SDK ${PROJECT_SOURCE_DIR}/third_party/vst3sdk)
  add_library(montage_test_vst3 MODULE plugins/test_vst3_gain.cpp
    ${VST3SDK}/pluginterfaces/base/funknown.cpp
    ${VST3SDK}/pluginterfaces/base/coreiids.cpp
    ${VST3SDK}/public.sdk/source/vst/vstinitiids.cpp)
  target_include_directories(montage_test_vst3 PRIVATE ${VST3SDK})
  if(APPLE)
    target_link_libraries(montage_test_vst3 PRIVATE "-framework CoreFoundation")  # FUID::generate
  endif()
  target_compile_options(montage_test_vst3 PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w>)
  set_target_properties(montage_test_vst3 PROPERTIES PREFIX "" SUFFIX ".vst3" OUTPUT_NAME MontageTestVst3
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/vst3
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  add_dependencies(test_plugins montage_test_vst3)
  target_compile_definitions(test_plugins PRIVATE MONTAGE_TEST_VST3_DIR="${CMAKE_CURRENT_BINARY_DIR}/vst3")
endif()

# ---- Reference output: a stand-in NDI Runtime, named and placed as NDI_RUNTIME_DIR_V6 has it ----
add_library(montage_fake_ndi MODULE plugins/fake_ndi.c)
target_include_directories(montage_fake_ndi PRIVATE ${PROJECT_SOURCE_DIR}/third_party/ndi/include)
if(WIN32)
  set_target_properties(montage_fake_ndi PROPERTIES PREFIX "" OUTPUT_NAME "Processing.NDI.Lib.x64" SUFFIX ".dll")
elseif(APPLE)
  set_target_properties(montage_fake_ndi PROPERTIES PREFIX "lib" OUTPUT_NAME "ndi" SUFFIX ".dylib")
else()
  set_target_properties(montage_fake_ndi PROPERTIES PREFIX "lib" OUTPUT_NAME "ndi" SUFFIX ".so.6")
endif()
set_target_properties(montage_fake_ndi PROPERTIES C_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ndi RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/ndi)
add_dependencies(test_render montage_fake_ndi)
target_compile_definitions(test_render PRIVATE MONTAGE_TEST_NDI_DIR="${CMAKE_CURRENT_BINARY_DIR}/ndi"
  MONTAGE_TEST_NDI_LIBRARY="$<TARGET_FILE:montage_fake_ndi>")
