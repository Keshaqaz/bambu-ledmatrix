#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_FILE="$ROOT_DIR/src/index_html.h"
OUT_FILE="$ROOT_DIR/web/index.html"
TMP_FILE=$(mktemp "${TMPDIR:-/tmp}/ledmatrix-web.XXXXXX")
TMP_GZIP_HEADER=$(mktemp "${TMPDIR:-/tmp}/ledmatrix-web-gzip.XXXXXX")
trap 'rm -f "$TMP_FILE" "$TMP_GZIP_HEADER"' EXIT HUP INT TERM

awk '
BEGIN { started = 0 }
{
  if (!started) {
    if (sub(/^static const char INDEX_HTML\[\] PROGMEM = R"HTML\(/, "")) {
      started = 1
      print
    }
    next
  }
  if (sub(/\)HTML";$/, "")) {
    print
    exit
  }
  print
}
' "$SRC_FILE" > "$TMP_FILE"

if ! cmp -s "$TMP_FILE" "$OUT_FILE"; then
  printf "web/index.html is out of date; run: sh tools/export_web_ui.sh\n" >&2
  if command -v diff >/dev/null 2>&1; then
    diff -u "$OUT_FILE" "$TMP_FILE" | sed -n '1,120p' >&2 || true
  fi
  exit 1
fi

sh "$ROOT_DIR/tools/generate_web_ui_gzip.sh" "$TMP_GZIP_HEADER" >/dev/null
if ! cmp -s "$TMP_GZIP_HEADER" "$ROOT_DIR/src/index_html_gz.h"; then
  printf "src/index_html_gz.h is out of date; run: sh tools/generate_web_ui_gzip.sh\n" >&2
  exit 1
fi

printf "web/index.html and deterministic gzip asset are up to date\n"
