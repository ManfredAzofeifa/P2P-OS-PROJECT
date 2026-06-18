#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${P2P_TEST_CLIENT_SERVER_PORT:-39092}"
SHARED_DIR="$(mktemp -d)"
SERVER_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
CLIENT_FIFO="$(mktemp -u)"
CLIENT_PID=""
SERVER_PID=""
trap 'if [ -n "$CLIENT_PID" ]; then kill "$CLIENT_PID" 2>/dev/null || true; fi; if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; fi; rm -rf "$SHARED_DIR"; rm -f "$SERVER_LOG" "$CLIENT_LOG" "$CLIENT_FIFO"' EXIT

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

mkfifo "$CLIENT_FIFO"
"$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42001 "$SHARED_DIR" < "$CLIENT_FIFO" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
exec 8>"$CLIENT_FIFO"

printf 'files\n' >&8

for _ in $(seq 1 30); do
    if grep -q "transfer server listening on port 42001" "$CLIENT_LOG" &&
       grep -q "alpha.txt" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done

grep -q "registered 2 files" "$CLIENT_LOG"
grep -q "received 0 neighbors" "$CLIENT_LOG"
grep -q "transfer server listening on port 42001" "$CLIENT_LOG"

ALPHA_SIZE="$(awk '$NF == "alpha.txt" { print $(NF - 2); exit }' "$CLIENT_LOG")"
ALPHA_HASH="$(awk '$NF == "alpha.txt" { print $(NF - 1); exit }' "$CLIENT_LOG")"

printf 'find -s alpha.txt\nfind -s missing.txt\n' >&8

for _ in $(seq 1 30); do
    if grep -q "no peers found for missing.txt" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done

grep -q "peers for alpha.txt: 1" "$CLIENT_LOG"
grep -q "127.0.0.1 42001" "$CLIENT_LOG"
grep -q "no peers found for missing.txt" "$CLIENT_LOG"

printf 'request %s %s\nrequest 999 %s\n' "$ALPHA_SIZE" "$ALPHA_HASH" "$ALPHA_HASH" >&8

for _ in $(seq 1 30); do
    if grep -q "no peers found for 999 $ALPHA_HASH" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done

grep -q "candidate peers for $ALPHA_SIZE $ALPHA_HASH: 1" "$CLIENT_LOG"
grep -q "127.0.0.1 42001" "$CLIENT_LOG"
grep -q "download not implemented in this component yet" "$CLIENT_LOG"
grep -q "no peers found for 999 $ALPHA_HASH" "$CLIENT_LOG"

exec 4<>"/dev/tcp/127.0.0.1/42001"
printf 'GET %s %s 0 5\n' "$ALPHA_SIZE" "$ALPHA_HASH" >&4
read -r line <&4
[ "$line" = "DATA 5" ]
IFS= read -r -N 5 payload <&4
[ "$payload" = "alpha" ]
exec 4<&-
exec 4>&-

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

printf 'quit\n' >&8
exec 8>&-
wait "$CLIENT_PID"
CLIENT_PID=""

MISSING_DIR="$SHARED_DIR/removed-device"
printf 'quit\n' | "$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42002 "$MISSING_DIR" > "$CLIENT_LOG" 2>&1
grep -q "cannot open shared folder" "$CLIENT_LOG"
grep -q "registered 0 files" "$CLIENT_LOG"

printf 'client registration ok\n'
