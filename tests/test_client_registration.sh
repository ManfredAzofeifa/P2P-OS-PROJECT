#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${P2P_TEST_CLIENT_SERVER_PORT:-39092}"
PARTIAL_PORT="${P2P_TEST_PARTIAL_PEER_PORT:-43000}"
RANGE_PORT_A="${P2P_TEST_RANGE_PEER_A_PORT:-43011}"
RANGE_PORT_B="${P2P_TEST_RANGE_PEER_B_PORT:-43012}"
RANGE_PORT_C="${P2P_TEST_RANGE_PEER_C_PORT:-43013}"
SHARED_DIR="$(mktemp -d)"
SERVER_LOG="$(mktemp)"
CLIENT_LOG="$(mktemp)"
PARTIAL_LOG="$(mktemp)"
RANGE_LOG_A="$(mktemp)"
RANGE_LOG_B="$(mktemp)"
RANGE_LOG_C="$(mktemp)"
PARTIAL_BIN="$(mktemp)"
RANGE_BIN="$(mktemp)"
HASH_BIN="$(mktemp)"
CLIENT_FIFO="$(mktemp -u)"
CLIENT_PID=""
SERVER_PID=""
PARTIAL_PID=""
RANGE_PID_A=""
RANGE_PID_B=""
RANGE_PID_C=""
trap 'if [ -n "$RANGE_PID_C" ]; then kill "$RANGE_PID_C" 2>/dev/null || true; fi; if [ -n "$RANGE_PID_B" ]; then kill "$RANGE_PID_B" 2>/dev/null || true; fi; if [ -n "$RANGE_PID_A" ]; then kill "$RANGE_PID_A" 2>/dev/null || true; fi; if [ -n "$PARTIAL_PID" ]; then kill "$PARTIAL_PID" 2>/dev/null || true; fi; if [ -n "$CLIENT_PID" ]; then kill "$CLIENT_PID" 2>/dev/null || true; fi; if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; fi; rm -rf "$SHARED_DIR"; rm -f "$SERVER_LOG" "$CLIENT_LOG" "$PARTIAL_LOG" "$RANGE_LOG_A" "$RANGE_LOG_B" "$RANGE_LOG_C" "$PARTIAL_BIN" "$RANGE_BIN" "$HASH_BIN" "$CLIENT_FIFO"' EXIT

printf 'alpha contents\n' > "$SHARED_DIR/alpha.txt"
printf 'beta contents\n' > "$SHARED_DIR/beta.txt"
gcc -Wall -Wextra -o "$PARTIAL_BIN" "$ROOT_DIR/tests/partial_peer.c"
gcc -Wall -Wextra -o "$RANGE_BIN" "$ROOT_DIR/tests/range_peer.c"
gcc -Wall -Wextra -I"$ROOT_DIR" -o "$HASH_BIN" "$ROOT_DIR/tests/hash_file.c" "$ROOT_DIR/server/hash.c"

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
SEARCH_DIR="$SHARED_DIR/search-client"
SEARCH_LOG="$SHARED_DIR/search-client.log"
mkdir "$SEARCH_DIR"
printf 'find -d alpha.txt\nfind -d missing.txt\nquit\n' |
    "$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42003 "$SEARCH_DIR" \
    > "$SEARCH_LOG" 2>&1

grep -q "received 1 neighbors" "$SEARCH_LOG"
grep -q "distributed matches for alpha.txt: 1" "$SEARCH_LOG"
grep -q "$ALPHA_SIZE $ALPHA_HASH alpha.txt 127.0.0.1 42001" "$SEARCH_LOG"
grep -q "no distributed matches for missing.txt" "$SEARCH_LOG"


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

printf 'request %s %s\nrequest %s %s\nrequest 999 %s\n' \
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

SEGMENT_FILE="$SHARED_DIR/segmented-source.bin"
printf 'segmented download data from two peers\nwith multiple ranges\n' > "$SEGMENT_FILE"
SEGMENT_SIZE="$(wc -c < "$SEGMENT_FILE")"
SEGMENT_HASH="$("$HASH_BIN" "$SEGMENT_FILE")"

"$RANGE_BIN" "$RANGE_PORT_A" "$SEGMENT_FILE" 1 > "$RANGE_LOG_A" 2>&1 &
RANGE_PID_A=$!
"$RANGE_BIN" "$RANGE_PORT_B" "$SEGMENT_FILE" 1 > "$RANGE_LOG_B" 2>&1 &
RANGE_PID_B=$!
for _ in $(seq 1 30); do
    if grep -q "range peer listening" "$RANGE_LOG_A" &&
       grep -q "range peer listening" "$RANGE_LOG_B"; then
        break
    fi
    sleep 0.1
done

exec 5<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER %s 1\nFILE %s %s segmented-a.bin\nEND\n' "$RANGE_PORT_A" "$SEGMENT_SIZE" "$SEGMENT_HASH" >&5
read -r line <&5
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&5
done
exec 5<&-
exec 5>&-

exec 6<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER %s 1\nFILE %s %s segmented-b.bin\nEND\n' "$RANGE_PORT_B" "$SEGMENT_SIZE" "$SEGMENT_HASH" >&6
read -r line <&6
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&6
done
exec 6<&-
exec 6>&-

printf 'request %s %s\n' "$SEGMENT_SIZE" "$SEGMENT_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "downloaded $SEGMENT_SIZE $SEGMENT_HASH to $SHARED_DIR/$SEGMENT_HASH using 2 peers" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for $SEGMENT_SIZE $SEGMENT_HASH: 2" "$CLIENT_LOG"
grep -q "downloaded $SEGMENT_SIZE $SEGMENT_HASH to $SHARED_DIR/$SEGMENT_HASH using 2 peers" "$CLIENT_LOG"
cmp -s "$SEGMENT_FILE" "$SHARED_DIR/$SEGMENT_HASH"
wait "$RANGE_PID_A"
RANGE_PID_A=""
wait "$RANGE_PID_B"
RANGE_PID_B=""

FAILOVER_FILE="$SHARED_DIR/failover-source.bin"
printf 'segmented failover data served by one surviving peer\n' > "$FAILOVER_FILE"
FAILOVER_SIZE="$(wc -c < "$FAILOVER_FILE")"
FAILOVER_HASH="$("$HASH_BIN" "$FAILOVER_FILE")"

"$RANGE_BIN" "$RANGE_PORT_C" "$FAILOVER_FILE" 2 > "$RANGE_LOG_C" 2>&1 &
RANGE_PID_C=$!
for _ in $(seq 1 30); do
    if grep -q "range peer listening" "$RANGE_LOG_C"; then
        break
    fi
    sleep 0.1
done

exec 7<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 42999 1\nFILE %s %s failover-dead.bin\nEND\n' "$FAILOVER_SIZE" "$FAILOVER_HASH" >&7
read -r line <&7
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&7
done
exec 7<&-
exec 7>&-

exec 9<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER %s 1\nFILE %s %s failover-live.bin\nEND\n' "$RANGE_PORT_C" "$FAILOVER_SIZE" "$FAILOVER_HASH" >&9
read -r line <&9
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&9
done
exec 9<&-
exec 9>&-

printf 'request %s %s\n' "$FAILOVER_SIZE" "$FAILOVER_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "downloaded $FAILOVER_SIZE $FAILOVER_HASH to $SHARED_DIR/$FAILOVER_HASH using 2 peers" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for $FAILOVER_SIZE $FAILOVER_HASH: 2" "$CLIENT_LOG"
grep -q "downloaded $FAILOVER_SIZE $FAILOVER_HASH to $SHARED_DIR/$FAILOVER_HASH using 2 peers" "$CLIENT_LOG"
cmp -s "$FAILOVER_FILE" "$SHARED_DIR/$FAILOVER_HASH"
wait "$RANGE_PID_C"
RANGE_PID_C=""

UNAVAILABLE_HASH="11111111111111111111111111111111"
PARTIAL_HASH="22222222222222222222222222222222"
SEGMENT_FAIL_HASH="33333333333333333333333333333333"

exec 10<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 42997 1\nFILE 12 %s segment-fail-a.bin\nEND\n' "$SEGMENT_FAIL_HASH" >&10
read -r line <&10
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&10
done
exec 10<&-
exec 10>&-

exec 11<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 42998 1\nFILE 12 %s segment-fail-b.bin\nEND\n' "$SEGMENT_FAIL_HASH" >&11
read -r line <&11
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&11
done
exec 11<&-
exec 11>&-

printf 'request 12 %s\n' "$SEGMENT_FAIL_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "download failed for 12 $SEGMENT_FAIL_HASH" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for 12 $SEGMENT_FAIL_HASH: 2" "$CLIENT_LOG"
grep -q "download failed for 12 $SEGMENT_FAIL_HASH" "$CLIENT_LOG"
[ ! -e "$SHARED_DIR/$SEGMENT_FAIL_HASH" ]
[ ! -e "$SHARED_DIR/$SEGMENT_FAIL_HASH.part" ]

exec 12<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 42996 1\nFILE 12 %s unavailable.bin\nEND\n' "$UNAVAILABLE_HASH" >&12
read -r line <&12
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&12
done
exec 12<&-
exec 12>&-

printf 'request 12 %s\n' "$UNAVAILABLE_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "download failed for 12 $UNAVAILABLE_HASH" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for 12 $UNAVAILABLE_HASH: 1" "$CLIENT_LOG"
grep -q "download failed for 12 $UNAVAILABLE_HASH" "$CLIENT_LOG"
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

exec 13<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER %s 1\nFILE 12 %s partial.bin\nEND\n' "$PARTIAL_PORT" "$PARTIAL_HASH" >&13
read -r line <&13
[ "$line" = "OK registered 1 files" ]
while [ "$line" != "END" ]; do
    read -r line <&13
done
exec 13<&-
exec 13>&-

printf 'request 12 %s\n' "$PARTIAL_HASH" >&8
for _ in $(seq 1 30); do
    if grep -q "download failed for 12 $PARTIAL_HASH" "$CLIENT_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "candidate peers for 12 $PARTIAL_HASH: 1" "$CLIENT_LOG"
grep -q "download failed for 12 $PARTIAL_HASH" "$CLIENT_LOG"
[ ! -e "$SHARED_DIR/$PARTIAL_HASH" ]
[ ! -e "$SHARED_DIR/$PARTIAL_HASH.part" ]
wait "$PARTIAL_PID"
PARTIAL_PID=""

printf 'quit\n' >&8
exec 8>&-
wait "$CLIENT_PID"
CLIENT_PID=""

MISSING_DIR="$SHARED_DIR/removed-device"
printf 'quit\n' | "$ROOT_DIR/bin/client" 127.0.0.1 "$PORT" 42002 "$MISSING_DIR" > "$CLIENT_LOG" 2>&1
grep -q "cannot open shared folder" "$CLIENT_LOG"
grep -q "registered 0 files" "$CLIENT_LOG"

printf 'client registration ok\n'
