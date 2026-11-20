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
  libavcodec-dev libavformat-dev libavdevice-dev libavutil-dev libswscale-dev libswresample-dev libopencolorio-dev liblilv-dev libraw-dev libopenjp2-7-dev libjxl-dev liblcms2-dev libmysofa-dev uuid-dev libexpat1-dev libssl-dev libasound2-dev
  xvfb xauth curl jackd2)
# (man pages' index is not rebuilt after each package: on CI runners that alone can take minutes)
if [ -f /var/lib/man-db/auto-update ]; then $SUDO rm -f /var/lib/man-db/auto-update; fi
# A cloud runner takes its packages from its provider's mirror, named directly or through a mirror list. When that
# mirror's name does not resolve, apt-get update alone spends six minutes finding it out, and the other names in the
# list are the same domain's. So on such a runner the mirrors are asked for a package list first, and apt is pointed
# at the first that answers: the provider's, Ubuntu's main archive, then a mirror under another domain (apt checks
# the lists' signatures whichever serves them).
APT_SOURCES=(/etc/apt/sources.list.d/*.sources /etc/apt/sources.list)
MIRRORS=(http://azure.archive.ubuntu.com/ubuntu/ http://archive.ubuntu.com/ubuntu/ http://mirrors.kernel.org/ubuntu/)
mirror_at=0
on_runner_mirror=0
grep -qsE 'apt-mirrors\.txt|//azure\.archive\.ubuntu\.com/' "${APT_SOURCES[@]}" && on_runner_mirror=1
# Points apt at the first mirror that answers, starting at the given place in the list; fails when none does. (Any
# other machine's sources are its owner's choice and are left alone.)
use_mirror_from() {
  local codename i round
  [ "$on_runner_mirror" -eq 1 ] || return 1
  command -v curl > /dev/null || return 1
  codename=$(. /etc/os-release && echo "${VERSION_CODENAME:-}")
  [ -n "$codename" ] || return 1
  for round in 1 2; do
    for ((i = $1; i < ${#MIRRORS[@]}; i++)); do
      if curl -fsS -o /dev/null --connect-timeout 8 --max-time 15 "${MIRRORS[i]}dists/$codename/InRelease" 2> /dev/null; then
        if [ "$i" -ne "$mirror_at" ]; then
          echo "$(date -u +%T) packages come from ${MIRRORS[i]}"
          $SUDO sed -i -E -e "s#mirror\+file:/etc/apt/apt-mirrors\.txt/?#${MIRRORS[i]}#g" \
            -e "s#https?://(azure\.archive\.ubuntu\.com|archive\.ubuntu\.com|mirrors\.kernel\.org)/ubuntu/?#${MIRRORS[i]}#g" \
            "${APT_SOURCES[@]}" 2> /dev/null || true
          mirror_at=$i
        fi
        return 0
      fi
      echo "$(date -u +%T) ${MIRRORS[i]} does not answer"
    done
    [ "$round" -eq 1 ] && sleep 15
  done
  return 1
}
install_packages() {
  echo "$(date -u +%T) apt-get update"
  # (given a limit as well: with no name service it gives up on each list in turn, minutes in all)
  $SUDO timeout 180 apt-get "${APT_OPTS[@]}" update -qq || echo "$(date -u +%T) apt-get update did not finish"
  echo "$(date -u +%T) apt-get install"
  # (each download listed, so a log shows which one stalls; a mirror that trickles rather than stalls passes apt's own
  # timeouts, so the whole install is given a limit)
  $SUDO env DEBIAN_FRONTEND=noninteractive timeout "$1" apt-get "${APT_OPTS[@]}" install -y -q -o Dpkg::Use-Pty=0 "${PACKAGES[@]}" |
    { grep --line-buffered -E '^(Get:|Fetched|Err:|E:|W:)' || true; }
  return "${PIPESTATUS[0]}"
}
use_mirror_from 0 || true
if ! install_packages 240; then
  # A slow mirror: the next one that answers is used instead, and the install tried again, keeping what was already
  # downloaded.
  echo "$(date -u +%T) apt-get install did not finish; trying again from another mirror"
  use_mirror_from $((mirror_at + 1)) || true
  $SUDO dpkg --configure -a || true
  install_packages 480
fi
# ONNX Runtime (object masks): Ubuntu does not package it, so Microsoft's
# release goes to /opt/onnxruntime, where CMake looks for it.
ORT_VERSION=1.30.0
case "$(uname -m)" in
  x86_64) ORT_ARCH=x64; ORT_NUGET_ARCH=x64 ;;
  aarch64) ORT_ARCH=aarch64; ORT_NUGET_ARCH=arm64 ;;
  *) ORT_ARCH="" ;;
esac
# Microsoft's NuGet package of the same release holds the same library and headers. It is the second source:
# on 19 November 2026 GitHub answered 404 for the release archive that had worked until then.
ort_from_nuget() {
  curl -fsSL --retry 3 --retry-all-errors --connect-timeout 30 --speed-limit 51200 --speed-time 60 --max-time 900 \
    -o "$tmp/ort.nupkg" "https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/$ORT_VERSION/microsoft.ml.onnxruntime.$ORT_VERSION.nupkg" \
    || return 1
  mkdir -p "$tmp/ort/include" "$tmp/ort/lib"
  python3 -I - "$tmp/ort.nupkg" "$tmp/ort" "$ORT_NUGET_ARCH" "$ORT_VERSION" <<'EOF' || return 1
import os, sys, zipfile
nupkg, out, arch, version = sys.argv[1:]
native = f"runtimes/linux-{arch}/native/"
with zipfile.ZipFile(nupkg) as z:
    names = z.namelist()
    if native + "libonnxruntime.so" not in names:
        sys.exit(f"no {native}libonnxruntime.so in the package")
    for name in names:
        base = os.path.basename(name)
        if name.startswith("build/native/include/") and base.endswith(".h"):
            target = os.path.join(out, "include", base)
        elif name == native + "libonnxruntime.so":
            target = os.path.join(out, "lib", "libonnxruntime.so." + version)
        elif name.startswith(native) and base.endswith(".so"):
            target = os.path.join(out, "lib", base)
        elif name in ("LICENSE", "ThirdPartyNotices.txt"):
            target = os.path.join(out, name)
        else:
            continue
        with z.open(name) as src, open(target, "wb") as dst:
            dst.write(src.read())
os.chmod(os.path.join(out, "lib", "libonnxruntime.so." + version), 0o755)
os.symlink("libonnxruntime.so." + version, os.path.join(out, "lib", "libonnxruntime.so.1"))
os.symlink("libonnxruntime.so.1", os.path.join(out, "lib", "libonnxruntime.so"))
with open(os.path.join(out, "VERSION_NUMBER"), "w") as f:
    f.write(version + "\n")
EOF
}
if [ -n "$ORT_ARCH" ] && [ ! -f /opt/onnxruntime/include/onnxruntime_cxx_api.h ]; then
  tmp=$(mktemp -d)
  echo "$(date -u +%T) ONNX Runtime $ORT_VERSION"
  # (A download slower than 50 KB/s for a minute is given up and retried. GitHub's release downloads can answer 504
  # for minutes at a time, so a failed round tries NuGet, and then both again after a pause, for about five minutes.)
  ort_url="https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION/onnxruntime-linux-$ORT_ARCH-$ORT_VERSION.tgz"
  for round in 1 2 3 4 5 6; do
    if curl -fsSL --retry 3 --retry-all-errors --connect-timeout 30 --speed-limit 51200 --speed-time 60 --max-time 600 \
         -o "$tmp/ort.tgz" "$ort_url"; then
      $SUDO mkdir -p /opt/onnxruntime
      $SUDO tar -xzf "$tmp/ort.tgz" -C /opt/onnxruntime --strip-components=1
      break
    fi
    echo "$(date -u +%T) ONNX Runtime's archive could not be downloaded from GitHub; trying its NuGet package"
    rm -rf "$tmp/ort" "$tmp/ort.nupkg"
    if ort_from_nuget; then
      $SUDO mkdir -p /opt/onnxruntime
      $SUDO cp -a "$tmp/ort/." /opt/onnxruntime/
      break
    fi
    [ "$round" -eq 6 ] && { echo "ONNX Runtime could not be downloaded from $ort_url or NuGet" >&2; exit 22; }
    echo "$(date -u +%T) ONNX Runtime download failed; trying again in 45 s"
    sleep 45
  done
  rm -rf "$tmp"
fi
echo "Dependencies installed. Build with: cmake -S . -B build -G Ninja && cmake --build build"
