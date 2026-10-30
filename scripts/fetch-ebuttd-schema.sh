#!/usr/bin/env bash
# Fetches the EBU's XML Schema for EBU-TT-D (BSD 3-Clause), which the EBU-TT-D test checks Montage's documents against
# with xmllint, and the W3C's xml.xsd it imports (kept beside it, so xmllint needs no network). Used only by tests;
# nothing of it ships with Montage.
#
#   scripts/fetch-ebuttd-schema.sh [folder]     (default /opt/ebuttd-xsd)
#
# Leaves <folder>/ebutt_d.xsd and the schema documents it imports. A folder that already has this version is left as it is.
set -euo pipefail

COMMIT=86d1404266fde99d38b9061f7fb34ac6c5f69faf
TTML2=2f57a97d5a010961545e991c1744d17d2b5f35b1  # (the W3C's TTML2 repository, for xml.xsd)
DIR=${1:-/opt/ebuttd-xsd}
FILES="ebutt_d.xsd ebutt_datatypes.xsd ebutt_metadata.xsd ebutt_styling.xsd metadata.xsd parameter.xsd styling.xsd"

if [ "$(cat "$DIR/VERSION" 2>/dev/null || true)" = "$COMMIT" ] && [ -s "$DIR/ebutt_d.xsd" ]; then
  echo "The EBU-TT-D schema at $COMMIT is already in $DIR"
  exit 0
fi
mkdir -p "$DIR"
for f in $FILES; do
  curl -fsSL --retry 3 -o "$DIR/$f" "https://raw.githubusercontent.com/ebu/ebu-tt-d-xsd/$COMMIT/$f"
done
curl -fsSL --retry 3 -o "$DIR/xml.xsd" "https://raw.githubusercontent.com/w3c/ttml2/$TTML2/spec/xsd/xml.xsd"
sed -i.orig 's#schemaLocation="http://www.w3.org/2001/xml.xsd"#schemaLocation="xml.xsd"#' "$DIR/ebutt_d.xsd"
rm -f "$DIR/ebutt_d.xsd.orig"
echo "$COMMIT" > "$DIR/VERSION"
echo "The EBU-TT-D schema is in $DIR"
