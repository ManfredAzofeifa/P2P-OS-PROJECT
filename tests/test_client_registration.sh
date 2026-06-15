#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${P2P_TEST_CLIENT_SERVER_PORT:-39092}"
SHARED_DIR="$(mktemp -d)"
SERVER_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -rf "$SHARED_DIR"; rm -f "$SERVER_LOG" "$CLIENT_LOG"' EXIT

printf 'alpha contents\n' > "$SHARED_DIR/alpha.txt"
printf 'beta contents\n' > "$SHARED_DIR/beta.txt"

"$ROOT_DIR/bin/server" "$PORT" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 30); do
    if grep -q "listening" "$SERVER_LOG"; then
        break
    fi
    sleep 0.1
done

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    cat "$SERVER_LOG"
    exit 1
fi

printf 'quit\n' | "$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42001 "$SHARED_DIR" > "$CLIENT_LOG" 2>&1

grep -q "registered 2 files" "$CLIENT_LOG"
grep -q "received 0 neighbors" "$CLIENT_LOG"

exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'FIND alpha.txt\n' >&3
read -r line <&3
[ "$line" = "PEERS 1" ]
read -r line <&3
[ "$line" = "PEER 127.0.0.1 42001" ]
read -r line <&3
[ "$line" = "END" ]
exec 3<&-
exec 3>&-

MISSING_DIR="$SHARED_DIR/removed-device"
printf 'quit\n' | "$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42002 "$MISSING_DIR" > "$CLIENT_LOG" 2>&1
grep -q "cannot open shared folder" "$CLIENT_LOG"
grep -q "registered 0 files" "$CLIENT_LOG"

printf 'client registration ok\n'
