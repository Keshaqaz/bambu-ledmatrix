#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT_FILE=${1:-"$ROOT_DIR/src/index_html_gz.h"}
TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ledmatrix-web-gzip.XXXXXX")
trap 'rm -rf "$TMP_DIR"' EXIT HUP INT TERM

HTML_FILE="$ROOT_DIR/web/index.html"
GZIP_FILE="$TMP_DIR/index.html.gz"

sh "$ROOT_DIR/tools/export_web_ui.sh" >/dev/null

LC_ALL=C gzip -9 -n -c "$HTML_FILE" > "$GZIP_FILE"
gzip -dc "$GZIP_FILE" | cmp -s - "$HTML_FILE"

{
  printf '#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n'
  printf 'static const uint8_t INDEX_HTML_GZ[] PROGMEM = {\n'
  xxd -i < "$GZIP_FILE"
  printf '};\n'
  printf 'static const size_t INDEX_HTML_GZ_LEN = sizeof(INDEX_HTML_GZ);\n'
} > "$OUT_FILE"

printf 'Generated deterministic gzip asset %s\n' "$OUT_FILE"
