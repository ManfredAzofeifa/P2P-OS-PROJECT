#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${P2P_TEST_CLIENT_SERVER_PORT:-39092}"
PARTIAL_PORT="${P2P_TEST_PARTIAL_PEER_PORT:-43000}"
SHARED_DIR="$(mktemp -d)"
SERVER_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
PARTIAL_LOG="$(mktemp)"
PARTIAL_BIN="$(mktemp)"
CLIENT_FIFO="$(mktemp -u)"
CLIENT_PID=""
SERVER_PID=""
PARTIAL_PID=""
trap 'if [ -n "$PARTIAL_PID" ]; then kill "$PARTIAL_PID" 2>/dev/null || true; fi; if [ -n "$CLIENT_PID" ]; then kill "$CLIENT_PID" 2>/dev/null || true; fi; if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; fi; rm -rf "$SHARED_DIR"; rm -f "$SERVER_LOG" "$CLIENT_LOG" "$PARTIAL_LOG" "$PARTIAL_BIN" "$CLIENT_FIFO"' EXIT

printf 'alpha contents
' > "$SHARED_DIR/alpha.txt"
printf 'beta contents
' > "$SHARED_DIR/beta.txt"
gcc -Wall -Wextra -o "$PARTIAL_BIN" "$ROOT_DIR/tests/partial_peer.c"

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

printf 'files
' >&8

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

printf 'find -s alpha.txt
find -s missing.txt
' >&8

for _ in $(seq 1 30); do
    if grep -q "no peers found for missing.txt" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done

grep -q "peers for alpha.txt: 1" "$CLIENT_LOG"
grep -q "127.0.0.1 42001" "$CLIENT_LOG"
grep -q "no peers found for missing.txt" "$CLIENT_LOG"

printf 'request %s %s
request %s %s
request 999 %s
' \
    "$ALPHA_SIZE" "$ALPHA_HASH" "$ALPHA_SIZE" "$ALPHA_HASH" "$ALPHA_HASH" >&8

for _ in $(seq 1 30); do
    if grep -q "no peers found for 999 $ALPHA_HASH" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done

grep -q "candidate peers for $ALPHA_SIZE $ALPHA_HASH: 1" "$CLIENT_LOG"
grep -q "127.0.0.1 42001" "$CLIENT_LOG"
grep -q "downloaded $ALPHA_SIZE $ALPHA_HASH to $SHARED_DIR/$ALPHA_HASH" "$CLIENT_LOG"
grep -q "download already present at $SHARED_DIR/$ALPHA_HASH" "$CLIENT_LOG"
grep -q "no peers found for 999 $ALPHA_HASH" "$CLIENT_LOG"
cmp -s "$SHARED_DIR/alpha.txt" "$SHARED_DIR/$ALPHA_HASH"

exec 4<>"/dev/tcp/127.0.0.1/42001"
printf 'GET %s %s 0 5
' "$ALPHA_SIZE" "$ALPHA_HASH" >&4
read -r line <&4
[ "$line" = "DATA 5" ]
IFS= read -r -N 5 payload <&4
[ "$payload" = "alpha" ]
exec 4<&-
exec 4>&-

exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'FIND alpha.txt
' >&3
read -r line <&3
[ "$line" = "PEERS 1" ]
read -r line <&3
[ "$line" = "PEER 127.0.0.1 42001" ]
read -r line <&3
[ "$line" = "END" ]
exec 3<&-
exec 3>&-

UNAVAILABLE_HASH="11111111111111111111111111111111"
PARTIAL_HASH="22222222222222222222222222222222"

exec 5<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 42999 1
FILE 12 %s unavailable.bin
END
' "$UNAVAILABLE_HASH" >&5
read -r line <&5
[ "$line" = "OK registered 1 files" ]
read -r line <&5
[ "${line%% *}" = "NEIGHBORS" ]
while [ "$line" != "END" ]; do
    read -r line <&5
done
exec 5<&-
exec 5>&-

printf 'request 12 %s
' "$UNAVAILABLE_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "download failed from 127.0.0.1 42999" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for 12 $UNAVAILABLE_HASH: 1" "$CLIENT_LOG"
grep -q "download failed from 127.0.0.1 42999" "$CLIENT_LOG"
[ ! -e "$SHARED_DIR/$UNAVAILABLE_HASH" ]
[ ! -e "$SHARED_DIR/$UNAVAILABLE_HASH.part" ]

"$PARTIAL_BIN" "$PARTIAL_PORT" > "$PARTIAL_LOG" 2>&1 &
PARTIAL_PID=$!
for _ in $(seq 1 30); do
    if grep -q "partial peer listening" "$PARTIAL_LOG"; then
        break
    fi
    sleep 0.1
done

exec 6<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER %s 1
FILE 12 %s partial.bin
END
' "$PARTIAL_PORT" "$PARTIAL_HASH" >&6
read -r line <&6
[ "$line" = "OK registered 1 files" ]
read -r line <&6
[ "${line%% *}" = "NEIGHBORS" ]
while [ "$line" != "END" ]; do
    read -r line <&6
done
exec 6<&-
exec 6>&-

printf 'request 12 %s
' "$PARTIAL_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "download failed from 127.0.0.1 $PARTIAL_PORT" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for 12 $PARTIAL_HASH: 1" "$CLIENT_LOG"
grep -q "download failed from 127.0.0.1 $PARTIAL_PORT" "$CLIENT_LOG"
[ ! -e "$SHARED_DIR/$PARTIAL_HASH" ]
[ ! -e "$SHARED_DIR/$PARTIAL_HASH.part" ]
wait "$PARTIAL_PID"
PARTIAL_PID=""

printf 'quit
' >&8
exec 8>&-
wait "$CLIENT_PID"
CLIENT_PID=""

MISSING_DIR="$SHARED_DIR/removed-device"
printf 'quit
' | "$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42002 "$MISSING_DIR" > "$CLIENT_LOG" 2>&1
grep -q "cannot open shared folder" "$CLIENT_LOG"
grep -q "registered 0 files" "$CLIENT_LOG"

printf 'client registration ok
'
