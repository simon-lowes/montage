cmake_minimum_required(VERSION 3.21)
project(Montage VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(WIN32)
  # M_PI and friends; <cmath> hides them in strict C++ mode on Windows.
  add_compile_definitions(_USE_MATH_DEFINES)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(MONTAGE_BUILD_APP "Build the Montage GUI application" ON)
option(MONTAGE_BUILD_TESTS "Build the Montage test suite" ON)
option(MONTAGE_WITH_VST3 "Host VST3 audio plugins (VST 3 SDK, MIT, vendored)" ON)
option(MONTAGE_WITH_WHISPER "Speech-to-text with whisper.cpp (MIT, fetched at configure time)" ON)
option(MONTAGE_WITH_RNNOISE "Voice isolation with RNNoise (BSD, fetched with its model at configure time)" ON)
option(MONTAGE_WITH_THORVG "Lottie animations and SVG graphics with ThorVG (MIT, fetched at configure time)" ON)
option(MONTAGE_WITH_LV2 "Host LV2 audio plugins, when lilv is installed" ON)
option(MONTAGE_WITH_LIBRAW "Camera raw stills (CR3, NEF, ARW, DNG...), when LibRaw 0.20+ is installed" ON)
option(MONTAGE_WITH_OPENJPEG "IMF masters in the JPEG 2000 IMF profiles, when OpenJPEG 2.5+ is installed" ON)
option(MONTAGE_WITH_OCIO "OpenColorIO configs and ACES transforms, when OpenColorIO 2.1+ is installed" ON)
option(MONTAGE_WITH_ONNXRUNTIME "AI object masks (EdgeTAM), when ONNX Runtime 1.16+ is installed (ONNXRUNTIME_ROOT or the usual prefixes)" ON)
option(MONTAGE_REQUIRE_ONNXRUNTIME "Fail to configure without ONNX Runtime (release builds)" OFF)

# Windows ships an older onnxruntime.dll of its own in System32, which the
# loader finds before PATH: programs get a copy of ours beside them
# (MONTAGE_ORT_DLL is found in src/CMakeLists.txt).
function(montage_copy_runtime target)
  if(WIN32 AND MONTAGE_ORT_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MONTAGE_ORT_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Concurrent Xml)
if(MONTAGE_BUILD_APP)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Multimedia Network)
endif()
if(MONTAGE_BUILD_TESTS)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libswresample)
find_package(Threads REQUIRED)

if(MONTAGE_WITH_WHISPER)
  # whisper.cpp + ggml as static libraries. GGML_NATIVE off keeps the
  # binaries portable (no -march=native); Metal is used on macOS.
  include(FetchContent)
  set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(MONTAGE_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(whisper
    GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
    GIT_TAG v1.9.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(whisper)
  set(BUILD_SHARED_LIBS ${MONTAGE_SAVED_BUILD_SHARED_LIBS})
endif()

if(MONTAGE_WITH_RNNOISE)
  include(FetchContent)
  # RNNoise has no CMake build; only its sources and the trained model (a
  # generated C file, pinned by the hash in the repository's model_version).
  set(MONTAGE_RNNOISE_MODEL 0a8755f8e2d834eff6a54714ecc7d75f9932e845df35f8b59bc52a7cfe6e8b37)
  FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG 70f1d256acd4b34a572f999a05c87bf00b67730d
    SOURCE_SUBDIR no-cmake)
  FetchContent_Declare(rnnoise_model
    URL https://media.xiph.org/rnnoise/models/rnnoise_data-${MONTAGE_RNNOISE_MODEL}.tar.gz
    URL_HASH SHA256=${MONTAGE_RNNOISE_MODEL}
    SOURCE_SUBDIR no-cmake)
  FetchContent_MakeAvailable(rnnoise rnnoise_model)
  enable_language(C)
  set(RNNOISE_SRC ${rnnoise_SOURCE_DIR}/src)
  add_library(montage_rnnoise STATIC
    ${RNNOISE_SRC}/denoise.c ${RNNOISE_SRC}/rnn.c ${RNNOISE_SRC}/pitch.c ${RNNOISE_SRC}/kiss_fft.c
    ${RNNOISE_SRC}/celt_lpc.c ${RNNOISE_SRC}/nnet.c ${RNNOISE_SRC}/nnet_default.c
    ${RNNOISE_SRC}/parse_lpcnet_weights.c ${RNNOISE_SRC}/rnnoise_tables.c
    ${rnnoise_model_SOURCE_DIR}/src/rnnoise_data.c)
  target_include_directories(montage_rnnoise
    PUBLIC ${rnnoise_SOURCE_DIR}/include
    PRIVATE ${rnnoise_model_SOURCE_DIR}/src ${RNNOISE_SRC})
  set_target_properties(montage_rnnoise PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
  target_compile_options(montage_rnnoise PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w -O2>)
  if(NOT WIN32)
    target_link_libraries(montage_rnnoise PRIVATE m)
  endif()
endif()

if(MONTAGE_WITH_THORVG)
  include(FetchContent)
  # ThorVG builds with Meson; its sources are compiled here instead, with the
  # CPU renderer and the Lottie (with expressions), SVG, font, PNG and JPEG loaders.
  FetchContent_Declare(thorvg
    GIT_REPOSITORY https://github.com/thorvg/thorvg.git
    GIT_TAG b004a778f53a5df61f06be45748761ae42c52dda  # v1.1.2
    SOURCE_SUBDIR no-cmake)
  FetchContent_MakeAvailable(thorvg)
  set(TVG ${thorvg_SOURCE_DIR}/src)
  file(GLOB_RECURSE MONTAGE_THORVG_SOURCES
    ${TVG}/common/*.cpp ${TVG}/renderer/*.cpp
    ${TVG}/loaders/raw/*.cpp ${TVG}/loaders/svg/*.cpp ${TVG}/loaders/sfnt/*.cpp
    ${TVG}/loaders/png/*.cpp ${TVG}/loaders/jpg/*.cpp ${TVG}/loaders/lottie/*.cpp)
  list(FILTER MONTAGE_THORVG_SOURCES EXCLUDE REGEX "/renderer/gpu_engine/")
  file(WRITE ${CMAKE_BINARY_DIR}/thorvg/config.h
    "#define THORVG_VERSION_STRING \"1.1.2\"\n"
    "#define THORVG_THREAD_SUPPORT 1\n"
    "#define THORVG_CPU_ENGINE_SUPPORT 1\n"
    "#define THORVG_PARTIAL_RENDER_SUPPORT 1\n"
    "#define THORVG_SVG_LOADER_SUPPORT 1\n"
    "#define THORVG_PNG_LOADER_SUPPORT 1\n"
    "#define THORVG_JPG_LOADER_SUPPORT 1\n"
    "#define THORVG_LOTTIE_LOADER_SUPPORT 1\n"
    "#define THORVG_LOTTIE_EXPRESSIONS_SUPPORT 1\n"
    "#define THORVG_SFNT_LOADER_SUPPORT 1\n"
    "#define THORVG_TTF_LOADER_SUPPORT 1\n"
    "#define THORVG_OTF_LOADER_SUPPORT 1\n"
    "#define THORVG_FILE_IO_SUPPORT 1\n"
    "#define WIN32_LEAN_AND_MEAN 1\n")
  add_library(montage_thorvg STATIC ${MONTAGE_THORVG_SOURCES})
  # The include directories Meson gives each part.
  set(JERRY ${TVG}/loaders/lottie/jerryscript)
  target_include_directories(montage_thorvg
    PUBLIC ${thorvg_SOURCE_DIR}/inc
    PRIVATE ${CMAKE_BINARY_DIR}/thorvg ${TVG}/common ${TVG}/renderer ${TVG}/renderer/cpu_engine ${TVG}/loaders
      ${TVG}/loaders/raw ${TVG}/loaders/svg ${TVG}/loaders/sfnt ${TVG}/loaders/png ${TVG}/loaders/jpg ${TVG}/loaders/lottie
      ${JERRY}/jerry-port/common ${JERRY}/jerry-core/api ${JERRY}/jerry-core/include ${JERRY}/jerry-core/ecma/base
      ${JERRY}/jerry-core/ecma/builtin-objects ${JERRY}/jerry-core/ecma/builtin-objects/typedarray
      ${JERRY}/jerry-core/ecma/operations ${JERRY}/jerry-core/jcontext ${JERRY}/jerry-core/jmem ${JERRY}/jerry-core/jrt
      ${JERRY}/jerry-core/lit ${JERRY}/jerry-core/parser/js ${JERRY}/jerry-core/parser/regexp ${JERRY}/jerry-core/vm)
  target_compile_definitions(montage_thorvg PUBLIC TVG_STATIC PRIVATE $<$<BOOL:${WIN32}>:NOMINMAX>)
  set_target_properties(montage_thorvg PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_STANDARD 14)
  target_compile_options(montage_thorvg PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w -O2> $<$<CXX_COMPILER_ID:MSVC>:/w /O2>)
  find_package(Threads REQUIRED)
  target_link_libraries(montage_thorvg PRIVATE Threads::Threads)
endif()

add_subdirectory(src)

if(MONTAGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
