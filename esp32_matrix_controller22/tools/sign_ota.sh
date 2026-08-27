#!/bin/sh
set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KEY_FILE=${OTA_SIGNING_KEY:-"$ROOT_DIR/.secrets/ota_signing_private.pem"}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  printf 'usage: %s firmware.bin [firmware.sig]\n' "$0" >&2
  exit 2
fi

IMAGE_FILE=$1
SIGNATURE_FILE=${2:-"$IMAGE_FILE.sig"}
if [ ! -f "$IMAGE_FILE" ]; then
  printf 'firmware image not found: %s\n' "$IMAGE_FILE" >&2
  exit 1
fi
if [ ! -f "$KEY_FILE" ]; then
  printf 'OTA signing key not found: %s\n' "$KEY_FILE" >&2
  exit 1
fi

TMP_PUBLIC=$(mktemp "${TMPDIR:-/tmp}/ledmatrix-ota-public.XXXXXX")
trap 'rm -f "$TMP_PUBLIC"' EXIT HUP INT TERM

openssl dgst -sha256 -sign "$KEY_FILE" -out "$SIGNATURE_FILE" "$IMAGE_FILE"
openssl ec -in "$KEY_FILE" -pubout -out "$TMP_PUBLIC" >/dev/null 2>&1
openssl dgst -sha256 -verify "$TMP_PUBLIC" -signature "$SIGNATURE_FILE" "$IMAGE_FILE" >/dev/null
chmod 600 "$SIGNATURE_FILE"
printf 'Signed %s -> %s\n' "$IMAGE_FILE" "$SIGNATURE_FILE"
