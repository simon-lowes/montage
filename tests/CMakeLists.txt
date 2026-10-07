function(montage_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE montage_engine Qt6::Test)
  set_target_properties(${name} PROPERTIES AUTOMOC ON)
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 300)
endfunction()

montage_test(test_core)
montage_test(test_render)
montage_test(test_media)
target_compile_definitions(test_media PRIVATE MONTAGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

if(MONTAGE_BUILD_APP)
  add_executable(test_app test_app.cpp)
  target_link_libraries(test_app PRIVATE montage_ui Qt6::Test)
  set_target_properties(test_app PROPERTIES AUTOMOC ON)
  add_test(NAME test_app COMMAND test_app)
  set_tests_properties(test_app PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MONTAGE_NO_PLUGIN_SCAN=1;MONTAGE_NO_RECOVERY_PROMPT=1;MONTAGE_RECOVERY_DIR=${CMAKE_CURRENT_BINARY_DIR}/recovery-test" TIMEOUT 300)
  add_dependencies(test_app montage_test_plugins montage-plugin-probe)
  target_compile_definitions(test_app PRIVATE
    MONTAGE_TEST_CLAP_DIR="${CMAKE_CURRENT_BINARY_DIR}/clap"
    MONTAGE_PLUGIN_PROBE="$<TARGET_FILE:montage-plugin-probe>"
    MONTAGE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
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

montage_test(test_plugins)
add_dependencies(test_plugins montage_test_plugins montage_test_hang montage_test_crash montage-plugin-probe)
target_compile_definitions(test_plugins PRIVATE
  MONTAGE_TEST_CLAP_DIR="${CMAKE_CURRENT_BINARY_DIR}/clap"
  MONTAGE_PLUGIN_PROBE="$<TARGET_FILE:montage-plugin-probe>")

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
