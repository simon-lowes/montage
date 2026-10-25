#!/usr/bin/env bash
# Builds Celemony's ARA test plug-in (ARATestPlugIn from the ARA SDK's examples, Apache-2.0) as a VST3 and a CLAP
# plug-in, which the ARA hosting test loads. Used only by tests; nothing of it is linked into Montage.
#
#   scripts/build-ara-test-plugin.sh [prefix]     (default /opt/ara-test-plugin)
#
# Puts <prefix>/ARATestPlugIn.vst3 and <prefix>/ARATestPlugIn.clap there (MONTAGE_TEST_ARA_PLUGINS=<prefix>). A
# prefix that already has this version is left as it is.
set -euo pipefail

ARA_TAG=releases/2.3.0
ARA_COMMIT=0005c532bb03eb210e7bb490329fc6d7ba1792c7
CLAP_TAG=1.2.6
VST3_TAG=v3.8.0_build_66
VERSION="ara-$ARA_COMMIT clap-$CLAP_TAG vst3-$VST3_TAG"
PREFIX=${1:-/opt/ara-test-plugin}

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$VERSION" ] && [ -e "$PREFIX/ARATestPlugIn.vst3" ] \
  && [ -e "$PREFIX/ARATestPlugIn.clap" ]; then
  echo "The ARA test plug-in is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
clone() { git -c advice.detachedHead=false clone -q --depth 1 --branch "$1" "$2" "$3"; }
clone "$ARA_TAG" https://github.com/Celemony/ARA_SDK.git "$tmp/ara"
got=$(git -C "$tmp/ara" rev-parse HEAD)
[ "$got" = "$ARA_COMMIT" ] || { echo "ARA SDK $ARA_TAG is commit $got, expected $ARA_COMMIT" >&2; exit 1; }
git -C "$tmp/ara" submodule update -q --init --depth 1 ARA_API ARA_Library ARA_Examples
clone "$CLAP_TAG" https://github.com/free-audio/clap.git "$tmp/clap"
clone "$VST3_TAG" https://github.com/steinbergmedia/vst3sdk.git "$tmp/vst3sdk"
# Only the parts the plug-in is built from (no editor library, documentation or tutorials).
git -C "$tmp/vst3sdk" submodule update -q --init --depth 1 base cmake pluginterfaces public.sdk

cmake -S "$tmp/ara/ARA_Examples" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release -DARA_CLAP_SDK_DIR="$tmp/clap" \
  -DARA_VST3_SDK_DIR="$tmp/vst3sdk" -DSMTG_ENABLE_VSTGUI_SUPPORT=OFF -DSMTG_ADD_VSTGUI=OFF \
  -DSMTG_CREATE_PLUGIN_LINK=OFF > "$tmp/build.log" 2>&1 \
  && cmake --build "$tmp/build" -j "$(nproc)" --config Release --target ARATestPlugInVST3 ARATestPlugInCLAP >> "$tmp/build.log" 2>&1 \
  || { tail -n 80 "$tmp/build.log" >&2; exit 1; }
mkdir -p "$PREFIX"
rm -rf "$PREFIX/ARATestPlugIn.vst3" "$PREFIX/ARATestPlugIn.clap"
cp -R "$tmp/build/bin/Release/ARATestPlugIn.vst3" "$tmp/build/bin/Release/ARATestPlugIn.clap" "$PREFIX/"
echo "$VERSION" > "$PREFIX/VERSION"
echo "Built the ARA test plug-in in $PREFIX"
