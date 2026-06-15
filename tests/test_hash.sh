#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

printf 'same payload\n' > "$TMP_DIR/original.txt"
cp "$TMP_DIR/original.txt" "$TMP_DIR/renamed.bin"
printf 'different payload\n' > "$TMP_DIR/changed.txt"

gcc -Wall -Wextra -I"$ROOT_DIR" -o "$TMP_DIR/test_hash" \
    "$ROOT_DIR/tests/test_hash.c" "$ROOT_DIR/server/hash.c"

"$TMP_DIR/test_hash" "$TMP_DIR/original.txt" "$TMP_DIR/renamed.bin" "$TMP_DIR/changed.txt"
