#!/usr/bin/env bash
# Builds leqm-nrt (Luca Trisciani's Leq(M) meter, GPL-3.0), which the cinema loudness test compares Montage's Leq(M)
# with. Used only by tests; nothing of it is linked into Montage. Needs libsndfile's headers.
#
#   scripts/build-leqm-nrt.sh [prefix]     (default /opt/leqm-nrt)
#
# Installs <prefix>/bin/leqm-nrt. A prefix that already has this commit is left as it is.
set -euo pipefail

COMMIT=8601f2af1cc9479c785f14358bb42bb479f2aa52
PREFIX=${1:-/opt/leqm-nrt}

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$COMMIT" ] && [ -x "$PREFIX/bin/leqm-nrt" ]; then
  echo "leqm-nrt $COMMIT is already in $PREFIX"
  exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git init -q "$tmp/leqm-nrt"
git -C "$tmp/leqm-nrt" fetch -q --depth 1 https://github.com/lucat/leqm-nrt.git "$COMMIT"
git -C "$tmp/leqm-nrt" -c advice.detachedHead=false checkout -q FETCH_HEAD
# Its one source file, read through libsndfile (its configure script would also look for FFmpeg's older API).
mkdir -p "$PREFIX/bin"
cc -O2 -w -DHAVE_LIBSNDFILE -o "$PREFIX/bin/leqm-nrt" "$tmp/leqm-nrt/src/leqm-nrt.c" \
  $(pkg-config --cflags --libs sndfile) -lm -lpthread
echo "$COMMIT" > "$PREFIX/VERSION"
echo "Built leqm-nrt $COMMIT in $PREFIX"
