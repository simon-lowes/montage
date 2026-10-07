#!/usr/bin/env bash
# Installs Montage's build dependencies on Debian / Ubuntu (24.04+).
set -euo pipefail
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"
# Retry flaky mirrors and give up on a stalled connection instead of hanging.
APT_OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30)
$SUDO apt-get "${APT_OPTS[@]}" update -qq
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get "${APT_OPTS[@]}" install -y -qq \
  build-essential cmake ninja-build pkg-config ffmpeg \
  qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev libgl1-mesa-dev \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libopencolorio-dev \
  xvfb xauth
echo "Dependencies installed. Build with: cmake -S . -B build -G Ninja && cmake --build build"
