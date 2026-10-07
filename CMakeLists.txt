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

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Concurrent)
if(MONTAGE_BUILD_APP)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Multimedia)
endif()
if(MONTAGE_BUILD_TESTS)
  find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavformat libavcodec libavutil libswscale libswresample)
find_package(Threads REQUIRED)

add_subdirectory(src)

if(MONTAGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
