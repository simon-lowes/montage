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
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libopencolorio-dev liblilv-dev \
  xvfb xauth curl
# ONNX Runtime (object masks): Ubuntu does not package it, so Microsoft's
# release archive goes to /opt/onnxruntime, where CMake looks for it.
ORT_VERSION=1.30.0
case "$(uname -m)" in
  x86_64) ORT_ARCH=x64 ;;
  aarch64) ORT_ARCH=aarch64 ;;
  *) ORT_ARCH="" ;;
esac
if [ -n "$ORT_ARCH" ] && [ ! -f /opt/onnxruntime/include/onnxruntime_cxx_api.h ]; then
  tmp=$(mktemp -d)
  curl -fsSL --retry 3 --max-time 600 -o "$tmp/ort.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-linux-$ORT_ARCH-$ORT_VERSION.tgz"
  $SUDO mkdir -p /opt/onnxruntime
  $SUDO tar -xzf "$tmp/ort.tgz" -C /opt/onnxruntime --strip-components=1
  rm -rf "$tmp"
fi
echo "Dependencies installed. Build with: cmake -S . -B build -G Ninja && cmake --build build"
