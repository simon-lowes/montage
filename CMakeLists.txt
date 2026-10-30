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
option(MONTAGE_WITH_LLAMA "Words from the footage with a small language model on this computer (llama.cpp, MIT, fetched at configure time)" ON)
option(MONTAGE_WITH_RNNOISE "Voice isolation with RNNoise (BSD, fetched with its model at configure time)" ON)
option(MONTAGE_WITH_THORVG "Lottie animations and SVG graphics with ThorVG (MIT, fetched at configure time)" ON)
option(MONTAGE_WITH_FDL "ASC Framing Decision Lists (the ASC's libfdl_core, Apache-2.0, with jsoncons, BSL-1.0, fetched at configure time)" ON)
option(MONTAGE_WITH_LUA "Lua scripting (Lua 5.4, MIT, fetched at configure time)" ON)
option(MONTAGE_WITH_LV2 "Host LV2 audio plugins, when lilv is installed" ON)
option(MONTAGE_WITH_LIBRAW "Camera raw stills (CR3, NEF, ARW, DNG...), when LibRaw 0.20+ is installed" ON)
option(MONTAGE_WITH_MYSOFA "Measured HRTFs (SOFA files) for binaural ambisonics, when libmysofa is installed" ON)
option(MONTAGE_WITH_OPENJPEG "IMF masters in the JPEG 2000 IMF profiles, when OpenJPEG 2.5+ is installed" ON)
option(MONTAGE_WITH_OPENSSL "Encrypted DCPs, signed compositions and KDMs, when OpenSSL 3 is installed" ON)
option(MONTAGE_WITH_OPENEXR "Multi-layer OpenEXR, Cryptomatte and HTJ2K (OpenEXR with Imath and OpenJPH, BSD, fetched at configure time)" ON)
option(MONTAGE_WITH_SPATIAL_VIDEO "Spatial video out: MV-HEVC through x265's multiview encoder (x265 4.2, GPL-2.0-or-later, downloaded and built with Montage)" ON)
option(MONTAGE_WITH_OMT "Open Media Transport in and out (libvmx, its VMX codec, MIT, fetched at configure time)" ON)
option(MONTAGE_WITH_WEBRTC "Live review in any web browser over WebRTC (libdatachannel, MPL-2.0, with libjuice, usrsctp, libSRTP, Plog and nlohmann/json, fetched at configure time), when OpenSSL 3 is installed" ON)
option(MONTAGE_WITH_BMX "Broadcast MXF by the book: AS-11, RDD 9 and ST 436 ancillary data (the EBU's bmx with libMXF, BSD, fetched at configure time)" ON)
option(MONTAGE_GPU "The GPU compositor through Qt's RHI, where Qt is 6.6 or later" ON)
option(MONTAGE_WITH_OCIO "OpenColorIO configs and ACES transforms, when OpenColorIO 2.1+ is installed" ON)
option(MONTAGE_WITH_ONNXRUNTIME "AI object masks (EdgeTAM), when ONNX Runtime 1.16+ is installed (ONNXRUNTIME_ROOT or the usual prefixes)" ON)
option(MONTAGE_REQUIRE_ONNXRUNTIME "Fail to configure without ONNX Runtime (release builds)" OFF)
option(MONTAGE_REQUIRE_OPENSSL "Fail to configure without OpenSSL 3 (release builds)" OFF)
option(MONTAGE_WITH_C2PA "Content Credentials (C2PA), when scripts/build-c2pa.sh has built c2pa-rs (C2PA_ROOT or /opt/c2pa)" ON)
option(MONTAGE_REQUIRE_C2PA "Fail to configure without the C2PA library (release builds)" OFF)

# Windows ships an older onnxruntime.dll of its own in System32, which the
# loader finds before PATH: programs get a copy of ours beside them
# (MONTAGE_ORT_DLL is found in src/CMakeLists.txt).
function(montage_copy_runtime target)
  if(WIN32 AND MONTAGE_ORT_DLL)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MONTAGE_ORT_DLL}" "$<TARGET_FILE_DIR:${target}>")
  endif()
endfunction()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Concurrent Xml Network)
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

if(MONTAGE_WITH_LLAMA)
  # llama.cpp's library alone (no common, tools or server), static. It uses whisper.cpp's ggml when that is built
  # (b11430 has the same ggml as whisper.cpp 1.9.5), its own otherwise.
  include(FetchContent)
  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
  set(LLAMA_OPENSSL OFF CACHE BOOL "" FORCE)
  set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
  set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
  set(MONTAGE_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_Declare(llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG b11430
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(llama)
  # b11430's models/clef.cpp calls std::find without <algorithm>, which newer libstdc++ (GCC 16, as on MSYS2) no
  # longer brings in through <memory>.
  if(NOT MSVC)
    target_compile_options(llama PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-include algorithm>)
  endif()
  set(BUILD_SHARED_LIBS ${MONTAGE_SAVED_BUILD_SHARED_LIBS})
endif()

if(MONTAGE_WITH_OPENEXR)
  # OpenEXR 3.5 as static libraries, with its own Imath and its bundled libdeflate, zstd and OpenJPH (HTJ2K), so every
  # platform reads and writes the same compressions; no tools, examples, tests or install.
  include(FetchContent)
  foreach(opt OPENEXR_BUILD_TOOLS OPENEXR_INSTALL_TOOLS OPENEXR_BUILD_EXAMPLES OPENEXR_BUILD_PYTHON OPENEXR_TEST_LIBRARIES
              OPENEXR_TEST_TOOLS OPENEXR_TEST_PYTHON OPENEXR_INSTALL OPENEXR_INSTALL_PKG_CONFIG OPENEXR_INSTALL_DOCS
              BUILD_WEBSITE IMATH_INSTALL IMATH_INSTALL_PKG_CONFIG PYTHON)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(opt OPENEXR_FORCE_INTERNAL_IMATH OPENEXR_FORCE_INTERNAL_DEFLATE OPENEXR_FORCE_INTERNAL_OPENJPH
              OPENEXR_FORCE_INTERNAL_ZSTD)
    set(${opt} ON CACHE BOOL "" FORCE)
  endforeach()
  set(MONTAGE_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
  set(MONTAGE_SAVED_BUILD_TESTING ${BUILD_TESTING})
  set(BUILD_SHARED_LIBS OFF)
  set(MONTAGE_SAVED_PIC ${CMAKE_POSITION_INDEPENDENT_CODE})
  set(BUILD_TESTING OFF)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  FetchContent_Declare(openexr
    GIT_REPOSITORY https://github.com/AcademySoftwareFoundation/openexr.git
    GIT_TAG v3.5.2
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(openexr)
  if(NOT WIN32)
    # zstd marks its API visibility("default") over OpenEXRCore's hidden default, so the executable would export its
    # zstd and shared libraries that use the system libzstd (Qt, libtiff) would bind to this copy instead.
    target_compile_definitions(OpenEXRCore PRIVATE "ZSTDLIB_VISIBLE=__attribute__((visibility(\"hidden\")))"
                                                   "ZSTDERRORLIB_VISIBLE=__attribute__((visibility(\"hidden\")))")
  endif()
  set(BUILD_SHARED_LIBS ${MONTAGE_SAVED_BUILD_SHARED_LIBS})
  set(BUILD_TESTING ${MONTAGE_SAVED_BUILD_TESTING})
  set(CMAKE_POSITION_INDEPENDENT_CODE ${MONTAGE_SAVED_PIC})
endif()

if(MONTAGE_WITH_BMX)
  # bmx 1.7 (the EBU's MXF library, with its libMXF and libMXF++) as static libraries for AS-11 and RDD 9 files;
  # uriparser built from source, expat the system's (shared libraries such as fontconfig use it too).
  include(FetchContent)
  set(MONTAGE_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
  set(MONTAGE_SAVED_BUILD_TESTING ${BUILD_TESTING})
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_TESTING OFF)
  # bmx and its libraries ask for CMake 3.12, so their option(BUILD_SHARED_LIBS ... ON) would replace the OFF above
  # with a cached ON (shared libraries on a first configure, and ON for everything after) unless options defer to
  # variables.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(URIPARSER_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  foreach(opt BMX_BUILD_TESTING BMX_BUILD_APPS BMX_BUILD_TOOLS BMX_BUILD_WITH_LIBCURL BMX_BUILD_EXPAT_SOURCE)
    set(${opt} OFF CACHE BOOL "" FORCE)
  endforeach()
  foreach(opt BMX_BUILD_LIB_ONLY BMX_BUILD_URIPARSER_SOURCE)
    set(${opt} ON CACHE BOOL "" FORCE)
  endforeach()
  set(LIBMXF_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(LIBMXFPP_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(bmx
    GIT_REPOSITORY https://github.com/ebu/bmx.git
    GIT_TAG v1.7
    GIT_SHALLOW TRUE
    GIT_SUBMODULES "")
  FetchContent_MakeAvailable(bmx)
  if(MINGW)
    # libMXF's Windows memory-mapped file guards its copies with MSVC's __try / __except, which GCC lacks.
    target_compile_options(MXF PRIVATE -include ${CMAKE_SOURCE_DIR}/third_party/bmx/mingw-seh.h)
  endif()
  unset(CMAKE_POLICY_DEFAULT_CMP0077)
  foreach(var BUILD_SHARED_LIBS BUILD_TESTING)
    if("${MONTAGE_SAVED_${var}}" STREQUAL "")
      unset(${var})
    else()
      set(${var} ${MONTAGE_SAVED_${var}})
    endif()
  endforeach()
endif()

if(MONTAGE_WITH_WEBRTC)
  # libdatachannel (MPL-2.0) and the libraries it brings as submodules, static, on OpenSSL: WebRTC for live review in a
  # browser (src/render/WebRtcReview.h). No WebSocket support, examples or tests.
  find_package(OpenSSL 3 QUIET COMPONENTS SSL Crypto)
  if(OPENSSL_FOUND)
    include(FetchContent)
    foreach(opt NO_WEBSOCKET NO_EXAMPLES NO_TESTS)
      set(${opt} ON CACHE BOOL "" FORCE)
    endforeach()
    foreach(opt USE_GNUTLS USE_MBEDTLS USE_NICE PREFER_SYSTEM_LIB)
      set(${opt} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(MONTAGE_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_Declare(libdatachannel
      GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
      GIT_TAG v0.24.6
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(libdatachannel)
    if("${MONTAGE_SAVED_BUILD_SHARED_LIBS}" STREQUAL "")
      unset(BUILD_SHARED_LIBS)
    else()
      set(BUILD_SHARED_LIBS ${MONTAGE_SAVED_BUILD_SHARED_LIBS})
    endif()
    set(MONTAGE_HAVE_WEBRTC ON)
  else()
    message(STATUS "OpenSSL 3 not found: no live review in a browser")
  endif()
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

if(MONTAGE_WITH_LUA)
  include(FetchContent)
  # Lua's own release; its library sources (not the lua and luac programs) compiled here.
  FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.7.tar.gz
    URL_HASH SHA256=9fbf5e28ef86c69858f6d3d34eccc32e911c1a28b4120ff3e84aaa70cfbf1e30
    SOURCE_SUBDIR no-cmake)
  FetchContent_MakeAvailable(lua)
  set(LUA_SRC ${lua_SOURCE_DIR}/src)
  set(LUA_FILES lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm
                lundump lvm lzio lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib lstrlib ltablib lutf8lib linit)
  list(TRANSFORM LUA_FILES PREPEND ${LUA_SRC}/)
  list(TRANSFORM LUA_FILES APPEND .c)
  # Compiled as C++, as Lua supports, so its errors unwind as exceptions through the C++ that calls it (destructors
  # run) rather than longjmp past it; its headers are then included without extern "C".
  set_source_files_properties(${LUA_FILES} PROPERTIES LANGUAGE CXX)
  add_library(montage_lua STATIC ${LUA_FILES})
  target_include_directories(montage_lua PUBLIC ${LUA_SRC})
  set_target_properties(montage_lua PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
  target_compile_options(montage_lua PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w -O2> $<$<CXX_COMPILER_ID:MSVC>:/w /O2>)
  if(APPLE)
    target_compile_definitions(montage_lua PRIVATE LUA_USE_MACOSX)
  elseif(UNIX)
    target_compile_definitions(montage_lua PRIVATE LUA_USE_LINUX)
    target_link_libraries(montage_lua PRIVATE m ${CMAKE_DL_LIBS})
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

if(MONTAGE_WITH_FDL)
  include(FetchContent)
  # The ASC's reference implementation of Framing Decision Lists (render/Fdl.h): its C library compiled here as a
  # static library (its own build makes a shared one), with jsoncons (header-only) for JSON and schema validation.
  FetchContent_Declare(ascfdl
    GIT_REPOSITORY https://github.com/ascmitc/fdl.git
    GIT_TAG e7c7d2b217f1955d7e996001b4d39aa694e34501  # v1.0.2
    SOURCE_SUBDIR no-cmake)
  FetchContent_Declare(jsoncons
    GIT_REPOSITORY https://github.com/danielaparker/jsoncons.git
    GIT_TAG v1.5.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake)
  FetchContent_MakeAvailable(ascfdl jsoncons)
  set(FDL_CORE ${ascfdl_SOURCE_DIR}/native/core)
  file(GLOB MONTAGE_FDL_SOURCES ${FDL_CORE}/src/*.cpp)
  # Its schemas embedded (as its build does), and its export macro empty for a static library.
  execute_process(COMMAND ${CMAKE_COMMAND} -DFDL_SCHEMA_DIR=${ascfdl_SOURCE_DIR}/schema
    -DFDL_SCHEMA_OUT=${CMAKE_BINARY_DIR}/ascfdl/fdl_schema_data.h -P ${FDL_CORE}/cmake/GenerateSchemaHeader.cmake)
  file(WRITE ${CMAKE_BINARY_DIR}/ascfdl/fdl_export.h "#pragma once\n#define FDL_API\n")
  include(${FDL_CORE}/cmake/FDLVersion.cmake)
  add_library(montage_fdl STATIC ${MONTAGE_FDL_SOURCES})
  target_include_directories(montage_fdl PUBLIC ${FDL_CORE}/include ${CMAKE_BINARY_DIR}/ascfdl
    PRIVATE ${FDL_CORE}/src ${jsoncons_SOURCE_DIR}/include)
  target_compile_definitions(montage_fdl PRIVATE FDL_ABI_VERSION_MAJOR=${FDL_ABI_VERSION_MAJOR}
    FDL_ABI_VERSION_MINOR=${FDL_ABI_VERSION_MINOR} FDL_ABI_VERSION_PATCH=${FDL_ABI_VERSION_PATCH}
    $<$<BOOL:${WIN32}>:NOMINMAX _CRT_SECURE_NO_WARNINGS>)
  set_target_properties(montage_fdl PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF CXX_STANDARD 17)
  target_compile_options(montage_fdl PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-w> $<$<CXX_COMPILER_ID:MSVC>:/w /bigobj>)
endif()

# SQLite (public domain) for the sound library's index (media/SoundLibrary.h), from its amalgamation, with full-text
# search (FTS5). The system's SQLite is used instead with MONTAGE_SYSTEM_SQLITE (as a distribution's build would), or
# when the amalgamation cannot be downloaded (offline, or a network that refuses it) and the system's has FTS5.
include(FetchContent)
enable_language(C)
option(MONTAGE_SYSTEM_SQLITE "Use the system's SQLite (built with FTS5) instead of the amalgamation" OFF)
set(MONTAGE_SQLITE_URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip)
set(MONTAGE_SQLITE_SHA256 1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032)
# Whether the system has an SQLite with FTS5 (found, and a full-text table made in it).
function(montage_system_sqlite_usable out)
  set(${out} OFF PARENT_SCOPE)
  if(CMAKE_CROSSCOMPILING)
    return()
  endif()
  find_package(SQLite3 3.9 QUIET)
  if(NOT SQLite3_FOUND)
    return()
  endif()
  include(CheckCSourceRuns)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_INCLUDES ${SQLite3_INCLUDE_DIRS})
  set(CMAKE_REQUIRED_LIBRARIES ${SQLite3_LIBRARIES})
  set(CMAKE_REQUIRED_QUIET ON)
  unset(MONTAGE_SYSTEM_SQLITE_FTS5 CACHE)  # (looked for again each time: one may have been installed since)
  check_c_source_runs([[
#include <sqlite3.h>
int main(void) {
    sqlite3* db = 0;
    if (sqlite3_open(":memory:", &db) != SQLITE_OK) return 1;
    const int r = sqlite3_exec(db, "CREATE VIRTUAL TABLE t USING fts5(a)", 0, 0, 0);
    sqlite3_close(db);
    return r != SQLITE_OK;
}]] MONTAGE_SYSTEM_SQLITE_FTS5)
  cmake_pop_check_state()
  if(MONTAGE_SYSTEM_SQLITE_FTS5)
    set(${out} ON PARENT_SCOPE)
    set(MONTAGE_SYSTEM_SQLITE_VERSION ${SQLite3_VERSION} PARENT_SCOPE)
  endif()
endfunction()
set(MONTAGE_SQLITE_FROM_SYSTEM OFF)
set(MONTAGE_SQLITE_ZIP ${CMAKE_BINARY_DIR}/sqlite-amalgamation-3500400.zip)
if(MONTAGE_SYSTEM_SQLITE)
  montage_system_sqlite_usable(MONTAGE_SQLITE_FROM_SYSTEM)
  if(NOT MONTAGE_SQLITE_FROM_SYSTEM)
    message(FATAL_ERROR "MONTAGE_SYSTEM_SQLITE is on, but no SQLite with FTS5 was found")
  endif()
elseif(NOT FETCHCONTENT_SOURCE_DIR_SQLITE AND NOT EXISTS ${FETCHCONTENT_BASE_DIR}/sqlite-src/sqlite3.c
       AND NOT EXISTS ${MONTAGE_SQLITE_ZIP})
  # (Three tries; a file that is not the amalgamation, as a proxy's page, is not kept.)
  foreach(MONTAGE_SQLITE_TRY RANGE 1 3)
    file(DOWNLOAD ${MONTAGE_SQLITE_URL} ${MONTAGE_SQLITE_ZIP}.part STATUS MONTAGE_SQLITE_STATUS INACTIVITY_TIMEOUT 30 TIMEOUT 300)
    list(GET MONTAGE_SQLITE_STATUS 0 MONTAGE_SQLITE_CODE)
    list(GET MONTAGE_SQLITE_STATUS 1 MONTAGE_SQLITE_WHY)
    if(MONTAGE_SQLITE_CODE EQUAL 0)
      file(SHA256 ${MONTAGE_SQLITE_ZIP}.part MONTAGE_SQLITE_GOT)
      if(MONTAGE_SQLITE_GOT STREQUAL MONTAGE_SQLITE_SHA256)
        file(RENAME ${MONTAGE_SQLITE_ZIP}.part ${MONTAGE_SQLITE_ZIP})
        break()
      endif()
      set(MONTAGE_SQLITE_WHY "what came was not the amalgamation")
    endif()
    file(REMOVE ${MONTAGE_SQLITE_ZIP}.part)
  endforeach()
  if(NOT EXISTS ${MONTAGE_SQLITE_ZIP})
    montage_system_sqlite_usable(MONTAGE_SQLITE_FROM_SYSTEM)
    if(NOT MONTAGE_SQLITE_FROM_SYSTEM)
      message(FATAL_ERROR "The SQLite amalgamation could not be downloaded (${MONTAGE_SQLITE_WHY}) and the system has no "
                          "SQLite with FTS5: install one (libsqlite3-dev), or point FETCHCONTENT_SOURCE_DIR_SQLITE at an "
                          "unpacked ${MONTAGE_SQLITE_URL}")
    endif()
    message(WARNING "The SQLite amalgamation could not be downloaded (${MONTAGE_SQLITE_WHY}): using the system's")
  endif()
endif()
if(MONTAGE_SQLITE_FROM_SYSTEM)
  message(STATUS "SQLite: the system's, ${MONTAGE_SYSTEM_SQLITE_VERSION}")
  add_library(montage_sqlite INTERFACE)
  target_link_libraries(montage_sqlite INTERFACE SQLite::SQLite3)
else()
  set(MONTAGE_SQLITE_TIMESTAMP "")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(MONTAGE_SQLITE_TIMESTAMP DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  endif()
  if(EXISTS ${MONTAGE_SQLITE_ZIP})
    set(MONTAGE_SQLITE_URL ${MONTAGE_SQLITE_ZIP})
  endif()
  FetchContent_Declare(sqlite
    URL ${MONTAGE_SQLITE_URL}
    URL_HASH SHA256=${MONTAGE_SQLITE_SHA256}
    ${MONTAGE_SQLITE_TIMESTAMP})
  FetchContent_MakeAvailable(sqlite)
  add_library(montage_sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
  target_include_directories(montage_sqlite PUBLIC ${sqlite_SOURCE_DIR})
  target_compile_definitions(montage_sqlite PRIVATE SQLITE_ENABLE_FTS5 SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION
                             SQLITE_DQS=0 SQLITE_DEFAULT_FOREIGN_KEYS=1)
  set_target_properties(montage_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden
                        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
  target_compile_options(montage_sqlite PRIVATE $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-w -O2> $<$<C_COMPILER_ID:MSVC>:/w /O2>)
  find_package(Threads REQUIRED)
  target_link_libraries(montage_sqlite PRIVATE Threads::Threads)
  if(UNIX AND NOT APPLE)
    target_link_libraries(montage_sqlite PRIVATE m)  # (its maths functions)
  endif()
endif()

if(MONTAGE_WITH_OMT)
  # libvmx (MIT), the VMX codec Open Media Transport sends pictures in (media/Omt.h), at a fixed commit. Its x86 code
  # is built for SSE4.2, LZCNT and BMI1 (and its AVX2 path for AVX2), file by file, so the rest of Montage is not;
  # Omt.cpp checks the processor before using it.
  FetchContent_Declare(libvmx
    GIT_REPOSITORY https://github.com/openmediatransport/libvmx.git
    GIT_TAG 31724bb96530325a989bc157c8e07538d6f1c4db)
  FetchContent_MakeAvailable(libvmx)
  set(MONTAGE_VMX_SRC ${libvmx_SOURCE_DIR}/src)
  add_library(montage_vmx STATIC ${MONTAGE_VMX_SRC}/vmxcodec.cpp ${MONTAGE_VMX_SRC}/vmxcodec_x86.cpp
                                 ${MONTAGE_VMX_SRC}/vmxcodec_avx2.cpp ${MONTAGE_VMX_SRC}/vmxcodec_arm.cpp)
  target_include_directories(montage_vmx PUBLIC ${MONTAGE_VMX_SRC})
  set_target_properties(montage_vmx PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden
                        AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(montage_vmx PRIVATE -include ${CMAKE_SOURCE_DIR}/third_party/libvmx/montage-compat.h)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(montage_vmx PRIVATE -fdeclspec)
  endif()
  if(NOT MSVC)
    target_compile_options(montage_vmx PRIVATE -O3 -w)
  endif()
  # Its x86 code's instruction sets, on those two files only (checked for at run time); in a universal macOS build,
  # for the x86_64 half alone.
  set(MONTAGE_VMX_X86 -msse4.2 -mssse3 -mpopcnt -mlzcnt -mbmi)
  set(MONTAGE_VMX_AVX2 -mavx2 -mfma -mlzcnt -mbmi -mbmi2)
  if(CMAKE_OSX_ARCHITECTURES)
    set(MONTAGE_VMX_TARGETS_X86 FALSE)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
      set(MONTAGE_VMX_TARGETS_X86 TRUE)
    endif()
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(MONTAGE_VMX_TARGETS_X86 TRUE)
  else()
    set(MONTAGE_VMX_TARGETS_X86 FALSE)
  endif()
  if(MONTAGE_VMX_TARGETS_X86 AND NOT MSVC)
    if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
      list(TRANSFORM MONTAGE_VMX_X86 PREPEND "-Xarch_x86_64 ")
      list(TRANSFORM MONTAGE_VMX_AVX2 PREPEND "-Xarch_x86_64 ")
    endif()
    list(JOIN MONTAGE_VMX_X86 " " MONTAGE_VMX_X86)
    list(JOIN MONTAGE_VMX_AVX2 " " MONTAGE_VMX_AVX2)
    # (as one string, COMPILE_FLAGS, since COMPILE_OPTIONS would fold the repeated -Xarch_x86_64 together)
    set_source_files_properties(${MONTAGE_VMX_SRC}/vmxcodec_x86.cpp PROPERTIES COMPILE_FLAGS "${MONTAGE_VMX_X86}")
    set_source_files_properties(${MONTAGE_VMX_SRC}/vmxcodec_avx2.cpp PROPERTIES COMPILE_FLAGS "${MONTAGE_VMX_AVX2}")
  endif()
  target_link_libraries(montage_vmx PRIVATE Threads::Threads)
endif()

list(LENGTH CMAKE_OSX_ARCHITECTURES MONTAGE_OSX_ARCH_COUNT)
if(MONTAGE_WITH_SPATIAL_VIDEO AND MONTAGE_OSX_ARCH_COUNT GREATER 1)
  message(WARNING "Spatial video is left out of a universal build (x265 builds for one architecture at a time)")
  set(MONTAGE_WITH_SPATIAL_VIDEO OFF)
endif()
if(MONTAGE_WITH_SPATIAL_VIDEO)
  # x265 4.2 built with its multiview (MV-HEVC) encoder, which distribution builds leave out, as an 8-bit static
  # library. Its symbols are hidden, so they never stand in for those of the x265 FFmpeg itself may load.
  include(ExternalProject)
  enable_language(C)
  set(MONTAGE_X265_TIMESTAMP "")
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(MONTAGE_X265_TIMESTAMP DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  endif()
  set(MONTAGE_X265_PREFIX ${CMAKE_BINARY_DIR}/x265-mv)
  set(MONTAGE_X265_LIB ${MONTAGE_X265_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}x265${CMAKE_STATIC_LIBRARY_SUFFIX})
  set(MONTAGE_X265_FLAGS "")
  if(NOT WIN32)
    set(MONTAGE_X265_FLAGS "-fvisibility=hidden")
  endif()
  ExternalProject_Add(montage_x265_build
    URL https://download.videolan.org/pub/videolan/x265/x265_4.2.tar.gz
    URL_HASH SHA256=40b1ea0453e0309f0eba934e0ddf533f8f6295966679e8894e8f1c1c8d5e1210
    ${MONTAGE_X265_TIMESTAMP}
    SOURCE_SUBDIR source
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${MONTAGE_X265_PREFIX} -DCMAKE_INSTALL_LIBDIR=lib
               -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER} -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
               -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES} -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
               "-DCMAKE_C_FLAGS=${MONTAGE_X265_FLAGS}" "-DCMAKE_CXX_FLAGS=${MONTAGE_X265_FLAGS}"
               -DENABLE_MULTIVIEW=ON -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_LIBNUMA=OFF -DENABLE_PIC=ON
               -DENABLE_HDR10_PLUS=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    BUILD_BYPRODUCTS ${MONTAGE_X265_LIB})
  add_library(montage_x265 STATIC IMPORTED GLOBAL)
  set_target_properties(montage_x265 PROPERTIES IMPORTED_LOCATION ${MONTAGE_X265_LIB}
    # (Its header by its full path: another x265's, as Homebrew's, may be on the include path first.)
    INTERFACE_COMPILE_DEFINITIONS "MONTAGE_X265_HEADER=\"${MONTAGE_X265_PREFIX}/include/x265.h\"")
  find_package(Threads REQUIRED)
  target_link_libraries(montage_x265 INTERFACE Threads::Threads ${CMAKE_DL_LIBS})
  add_dependencies(montage_x265 montage_x265_build)
endif()

add_subdirectory(src)

if(MONTAGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
