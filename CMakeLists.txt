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

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Concurrent)
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

add_subdirectory(src)

if(MONTAGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
