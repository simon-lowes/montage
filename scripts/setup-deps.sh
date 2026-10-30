#!/usr/bin/env bash
# Installs Montage's build dependencies on Debian / Ubuntu (24.04+).
set -euo pipefail
SUDO=""
[ "$(id -u)" -ne 0 ] && SUDO="sudo"
# Retry flaky mirrors, give up on a stalled connection instead of hanging, and wait a bounded time for another apt
# (a runner's own updates) to let go of its lock. Each step says when it starts, so a log shows where one stalls.
APT_OPTS=(-o Acquire::Retries=3 -o Acquire::http::Timeout=30 -o Acquire::https::Timeout=30 -o DPkg::Lock::Timeout=180)
PACKAGES=(build-essential cmake ninja-build pkg-config nasm ffmpeg
  qt6-base-dev qt6-multimedia-dev libqt6opengl6-dev libgl1-mesa-dev qt6-l10n-tools
  libavcodec-dev libavformat-dev libavdevice-dev libavutil-dev libswscale-dev libswresample-dev libopencolorio-dev liblilv-dev libraw-dev libopenjp2-7-dev libmysofa-dev uuid-dev libexpat1-dev libssl-dev
  xvfb xauth curl)
# (man pages' index is not rebuilt after each package: on CI runners that alone can take minutes)
if [ -f /var/lib/man-db/auto-update ]; then $SUDO rm -f /var/lib/man-db/auto-update; fi
install_packages() {
  echo "$(date -u +%T) apt-get update"
  $SUDO apt-get "${APT_OPTS[@]}" update -qq
  echo "$(date -u +%T) apt-get install"
  # (each download listed, so a log shows which one stalls; a mirror that trickles rather than stalls passes apt's own
  # timeouts, so the whole install is given a limit)
  $SUDO env DEBIAN_FRONTEND=noninteractive timeout "$1" apt-get "${APT_OPTS[@]}" install -y -q -o Dpkg::Use-Pty=0 "${PACKAGES[@]}" |
    { grep --line-buffered -E '^(Get:|Fetched|Err:|E:|W:)' || true; }
  return "${PIPESTATUS[0]}"
}
if ! install_packages 240; then
  # A slow mirror: a cloud runner's own (named directly, or through its mirror list) is swapped for Ubuntu's main
  # archive, and the install tried again, keeping what was already downloaded.
  echo "$(date -u +%T) apt-get install did not finish; trying again from archive.ubuntu.com"
  $SUDO sed -i -e 's#mirror+file:/etc/apt/apt-mirrors\.txt#http://archive.ubuntu.com/ubuntu/#g' \
    -e 's#://azure\.archive\.ubuntu\.com/#://archive.ubuntu.com/#g' /etc/apt/sources.list.d/*.sources /etc/apt/sources.list 2>/dev/null || true
  $SUDO dpkg --configure -a || true
  install_packages 480
fi
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
  echo "$(date -u +%T) ONNX Runtime $ORT_VERSION"
  # (A download slower than 50 KB/s for a minute is given up and retried.)
  curl -fsSL --retry 3 --retry-all-errors --connect-timeout 30 --speed-limit 51200 --speed-time 60 --max-time 600 -o "$tmp/ort.tgz" \
    "https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-linux-$ORT_ARCH-$ORT_VERSION.tgz"
  $SUDO mkdir -p /opt/onnxruntime
  $SUDO tar -xzf "$tmp/ort.tgz" -C /opt/onnxruntime --strip-components=1
  rm -rf "$tmp"
fi
echo "Dependencies installed. Build with: cmake -S . -B build -G Ninja && cmake --build build"
