#!/usr/bin/env bash
# Builds libultrahdr's sample application (ultrahdr_app; Apache-2.0, with libjpeg-turbo, which its build fetches), the
# reference decoder the gain-map JPEG test reads Montage's files with. Used only by tests; nothing of it is linked into
# Montage.
#
#   scripts/build-ultrahdr.sh [prefix]     (default /opt/ultrahdr)
#
# Installs <prefix>/bin/ultrahdr_app. A prefix that already has this version is left as it is.
set -euo pipefail

TAG=v1.4.0
COMMIT=d52a0d13814ca399fc8a07e23de1d2c63f0e8404
PREFIX=${1:-/opt/ultrahdr}

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$TAG" ] && [ -x "$PREFIX/bin/ultrahdr_app" ]; then
  echo "libultrahdr $TAG is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" https://github.com/google/libultrahdr.git "$tmp/src"
got=$(git -C "$tmp/src" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "libultrahdr $TAG is commit $got, expected $COMMIT" >&2; exit 1; }

cmake -S "$tmp/src" -B "$tmp/build" -DCMAKE_BUILD_TYPE=Release -DUHDR_BUILD_DEPS=ON -DUHDR_BUILD_EXAMPLES=ON \
  -DUHDR_ENABLE_INSTALL=OFF > "$tmp/build.log" 2>&1 \
  && cmake --build "$tmp/build" -j "$(nproc)" --target ultrahdr_app >> "$tmp/build.log" 2>&1 || { tail -50 "$tmp/build.log" >&2; exit 1; }
mkdir -p "$PREFIX/bin"
cp "$tmp/build/ultrahdr_app" "$PREFIX/bin/ultrahdr_app"
echo "$TAG" > "$PREFIX/VERSION"
echo "Built ultrahdr_app $TAG in $PREFIX"
