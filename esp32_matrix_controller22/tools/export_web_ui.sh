#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC_FILE="$ROOT_DIR/src/index_html.h"
OUT_DIR="$ROOT_DIR/web"
OUT_FILE="$OUT_DIR/index.html"

mkdir -p "$OUT_DIR"

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
' "$SRC_FILE" > "$OUT_FILE"

printf "Exported %s\n" "$OUT_FILE"
