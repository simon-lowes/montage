#!/usr/bin/env bash
# Builds the Alliance for Open Media's reference IAMF decoder (libiamf, BSD-3-Clause-Clear) and its iamfdec tool, which
# the IAMF export test decodes Montage's files with. Used only by tests; nothing of it is linked into Montage.
#
#   scripts/build-iamfdec.sh [prefix]     (default /opt/iamfdec; Linux x86-64: it links the codec libraries it ships)
#
# Installs <prefix>/bin/iamfdec and the library it loads. A prefix that already has this version is left as it is.
set -euo pipefail

TAG=v1.1.0
COMMIT=f06e919e2ad5502a2adc4bdd4e146f2e7e7ffb63
PREFIX=${1:-/opt/iamfdec}

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$TAG" ] && [ -x "$PREFIX/bin/iamfdec" ]; then
  echo "libiamf $TAG is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Only its code: the repository's test streams are large.
git -c advice.detachedHead=false clone -q --depth 1 --branch "$TAG" --filter=blob:none --sparse \
  https://github.com/AOMediaCodec/libiamf.git "$tmp/libiamf"
git -C "$tmp/libiamf" sparse-checkout set code
got=$(git -C "$tmp/libiamf" rev-parse HEAD)
[ "$got" = "$COMMIT" ] || { echo "libiamf $TAG is commit $got, expected $COMMIT" >&2; exit 1; }

code="$tmp/libiamf/code"
mkdir -p "$PREFIX"
cmake -S "$code" -B "$tmp/lib" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" > "$tmp/lib.log" 2>&1 \
  && cmake --build "$tmp/lib" -j "$(nproc)" --target install >> "$tmp/lib.log" 2>&1 || { cat "$tmp/lib.log" >&2; exit 1; }
cmake -S "$code/test/tools/iamfdec" -B "$tmp/dec" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  -DCMAKE_BUILD_RPATH="$PREFIX/lib" > "$tmp/dec.log" 2>&1 \
  && cmake --build "$tmp/dec" -j "$(nproc)" >> "$tmp/dec.log" 2>&1 || { cat "$tmp/dec.log" >&2; exit 1; }
mkdir -p "$PREFIX/bin"
cp "$tmp/dec/iamfdec" "$PREFIX/bin/iamfdec"
echo "$TAG" > "$PREFIX/VERSION"
echo "Built iamfdec $TAG in $PREFIX"
