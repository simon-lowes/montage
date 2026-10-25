#!/usr/bin/env bash
# Fetches the W3C's XML Schema for TTML2 (W3C Software and Document License), which the IMSC test checks Montage's
# IMSC 1.1 documents against with xmllint. Used only by tests; nothing of it ships with Montage.
#
#   scripts/fetch-ttml-schema.sh [folder]     (default /opt/ttml2-xsd)
#
# Leaves <folder>/ttml2.xsd and the schema documents it includes. A folder that already has this version is left as it is.
set -euo pipefail

COMMIT=2f57a97d5a010961545e991c1744d17d2b5f35b1
DIR=${1:-/opt/ttml2-xsd}
FILES="ttml2.xsd ttml2-animation.xsd ttml2-audio-attribs.xsd ttml2-content.xsd ttml2-core-attribs.xsd ttml2-datatypes.xsd
  ttml2-document.xsd ttml2-embedded.xsd ttml2-head.xsd ttml2-layout.xsd ttml2-metadata-attribs.xsd ttml2-metadata-items.xsd
  ttml2-metadata.xsd ttml2-parameter-attribs.xsd ttml2-parameter-items.xsd ttml2-parameters.xsd ttml2-styling-attribs.xsd
  ttml2-styling.xsd ttml2-timing-attribs.xsd xlink.xsd xml.xsd"

if [ "$(cat "$DIR/VERSION" 2>/dev/null || true)" = "$COMMIT" ] && [ -s "$DIR/ttml2.xsd" ]; then
  echo "The TTML2 schema at $COMMIT is already in $DIR"
  exit 0
fi
mkdir -p "$DIR"
for f in $FILES; do
  curl -fsSL --retry 3 -o "$DIR/$f" "https://raw.githubusercontent.com/w3c/ttml2/$COMMIT/spec/xsd/$f"
done
echo "$COMMIT" > "$DIR/VERSION"
echo "The TTML2 schema is in $DIR"
