#!/usr/bin/env bash
# Builds the C2PA library (Content Credentials: the Content Authenticity Initiative's c2pa-rs, MIT or Apache-2.0)
# from its C API crate on crates.io, as a static library Montage links into its programs.
#
#   scripts/build-c2pa.sh [prefix]     (default /opt/c2pa; needs Rust's cargo)
#
# Installs <prefix>/include/c2pa.h, <prefix>/lib/libc2pa_c.a and <prefix>/lib/c2pa-native-libs.txt (the system
# libraries the static library needs, as rustc prints them), where CMake looks (C2PA_ROOT), and <prefix>/licenses: the
# licence files of every crate compiled into the library, one folder each, and INDEX.txt (crate, version, licence),
# which packaging copies into the packages. Takes a few minutes; a prefix that already has this version is left as
# it is. Needs jq.
set -euo pipefail

VERSION=0.91.0
SHA256=612bffa120fdb58b5b6767602b0c0c77a20e139a7102b3e13e85236308ee49f4
PREFIX=${1:-/opt/c2pa}

if [ "$(cat "$PREFIX/VERSION" 2>/dev/null || true)" = "$VERSION" ] && [ -f "$PREFIX/lib/libc2pa_c.a" ] && [ -f "$PREFIX/licenses/INDEX.txt" ]; then
  echo "C2PA $VERSION is already in $PREFIX"
  exit 0
fi
command -v cargo > /dev/null || { echo "cargo not found: install Rust (https://rustup.rs)" >&2; exit 1; }
command -v jq > /dev/null || { echo "jq not found" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 3 --max-time 300 -o "$tmp/c2pa.crate" "https://static.crates.io/crates/c2pa-c-ffi/c2pa-c-ffi-$VERSION.crate"
if command -v sha256sum > /dev/null; then got=$(sha256sum "$tmp/c2pa.crate" | cut -d' ' -f1); else got=$(shasum -a 256 "$tmp/c2pa.crate" | cut -d' ' -f1); fi
[ "$got" = "$SHA256" ] || { echo "c2pa-c-ffi-$VERSION.crate: checksum $got, expected $SHA256" >&2; exit 1; }
tar -xzf "$tmp/c2pa.crate" -C "$tmp"
src="$tmp/c2pa-c-ffi-$VERSION"

# Rust's own cryptography (no OpenSSL to find), local files only (no HTTP client): its Cargo.lock pins every crate.
(cd "$src" && cargo rustc --release --locked --no-default-features --features rust_native_crypto,file_io \
  --crate-type staticlib -- --print native-static-libs) 2> "$tmp/build.log" || { cat "$tmp/build.log" >&2; exit 1; }
native=$(sed -n 's/.*native-static-libs: //p' "$tmp/build.log" | tail -1)

lib=$(find "$src/target/release" -maxdepth 1 \( -name 'libc2pa_c.a' -o -name 'c2pa_c.lib' \) | head -1)
[ -n "$lib" ] && [ -f "$src/target/release/c2pa.h" ] || { echo "the build made no library or header" >&2; exit 1; }
# The crates compiled in (this platform's, the features above, no build-only crates) and their licence files.
features=(--locked --no-default-features --features rust_native_crypto,file_io)
(cd "$src" && cargo tree "${features[@]}" -e normal --prefix none --format '{p}') |
  sed -e 's/ (\*)$//' | awk '{ sub(/^v/, "", $2); print $1 " " $2 }' | sort -u > "$tmp/crates.txt"
(cd "$src" && cargo metadata "${features[@]}" --format-version 1) |
  jq -r '.packages[] | [.name, .version, (.license // "see its files"), (.manifest_path | sub("[/\\\\]Cargo.toml$"; ""))] | @tsv' > "$tmp/metadata.tsv"
mkdir -p "$tmp/licenses"
while IFS=$'\t' read -r name version licence dir; do
  grep -qx "$name $version" "$tmp/crates.txt" || continue
  command -v cygpath > /dev/null && dir=$(cygpath -u "$dir")
  mkdir -p "$tmp/licenses/$name-$version"
  find "$dir" -maxdepth 1 -type f \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' -o -iname 'COPYRIGHT*' \) \
    -exec cp {} "$tmp/licenses/$name-$version/" \;
  if [ -z "$(ls "$tmp/licenses/$name-$version")" ]; then
    case "$licence" in
      *Apache-2.0*) licence="$licence (the crate ships no licence file; the Apache-2.0 text is montage/LICENSES/Apache-2.0.txt)" ;;
      *) licence="$licence (the crate ships no licence file)" ;;
    esac
  fi
  printf '%s %s: %s\n' "$name" "$version" "$licence" >> "$tmp/licenses/INDEX.txt"
done < "$tmp/metadata.tsv"
[ -s "$tmp/licenses/INDEX.txt" ] || { echo "no crate licences were found" >&2; exit 1; }

SUDO=""
if ! mkdir -p "$PREFIX" 2> /dev/null || [ ! -w "$PREFIX" ]; then SUDO="sudo"; $SUDO mkdir -p "$PREFIX"; fi
$SUDO mkdir -p "$PREFIX/include" "$PREFIX/lib"
$SUDO cp "$src/target/release/c2pa.h" "$PREFIX/include/"
$SUDO cp "$lib" "$PREFIX/lib/libc2pa_c.a"
echo "$native" | $SUDO tee "$PREFIX/lib/c2pa-native-libs.txt" > /dev/null
$SUDO rm -rf "$PREFIX/licenses"
$SUDO cp -R "$tmp/licenses" "$PREFIX/licenses"
echo "$VERSION" | $SUDO tee "$PREFIX/VERSION" > /dev/null
echo "C2PA $VERSION installed in $PREFIX (system libraries: $native)"
