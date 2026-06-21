#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BASE_PORT=$((45000 + ($$ % 1000)))
SERVER_PORT="${P2P_TEST_TOPOLOGY_SERVER_PORT:-$BASE_PORT}"
A_PORT="${P2P_TEST_TOPOLOGY_A_PORT:-$((BASE_PORT + 1))}"
B_PORT="${P2P_TEST_TOPOLOGY_B_PORT:-$((BASE_PORT + 2))}"
C_PORT="${P2P_TEST_TOPOLOGY_C_PORT:-$((BASE_PORT + 3))}"
D_PORT="${P2P_TEST_TOPOLOGY_D_PORT:-$((BASE_PORT + 4))}"
E_PORT="${P2P_TEST_TOPOLOGY_E_PORT:-$((BASE_PORT + 5))}"
F_PORT="${P2P_TEST_TOPOLOGY_F_PORT:-$((BASE_PORT + 6))}"
ORIGIN_PORT="${P2P_TEST_QUERY_ORIGIN_PORT:-$((BASE_PORT + 20))}"
TMP_DIR="$(mktemp -d)"
TOPOLOGY_BIN="$TMP_DIR/topology_server"
QUERY_BIN="$TMP_DIR/query_origin"
HASH_BIN="$TMP_DIR/hash_file"
TOPOLOGY_LOG="$TMP_DIR/topology.log"
TOPOLOGY_PID=""
PIDS=()

cleanup() {
    for pid in "${PIDS[@]}"; do
        kill "$pid" 2>/dev/null || true
    done
    if [ -n "$TOPOLOGY_PID" ]; then
        kill "$TOPOLOGY_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_DIR"
}
trap cleanup EXIT

for client in a b c d e f; do
    mkdir "$TMP_DIR/$client"
done
printf 'near topology data\n' > "$TMP_DIR/c/near.txt"
printf 'beyond ttl data\n' > "$TMP_DIR/f/far.txt"

gcc -Wall -Wextra -o "$TOPOLOGY_BIN" "$ROOT_DIR/tests/topology_server.c"
gcc -Wall -Wextra -o "$QUERY_BIN" "$ROOT_DIR/tests/query_origin.c"
gcc -Wall -Wextra -I"$ROOT_DIR" -o "$HASH_BIN" \
    "$ROOT_DIR/tests/hash_file.c" "$ROOT_DIR/server/hash.c"

"$TOPOLOGY_BIN" "$SERVER_PORT" "$A_PORT" "$B_PORT" "$C_PORT" \
    "$D_PORT" "$E_PORT" "$F_PORT" > "$TOPOLOGY_LOG" 2>&1 &
TOPOLOGY_PID=$!
for _ in $(seq 1 30); do
    if grep -q "servidor de topologia escuchando" "$TOPOLOGY_LOG"; then
        break
    fi
    sleep 0.1
done
grep -q "servidor de topologia escuchando" "$TOPOLOGY_LOG"

start_client() {
    local name="$1"
    local port="$2"
    local fifo="$TMP_DIR/$name.fifo"
    local fd

    mkfifo "$fifo"
    exec {fd}<>"$fifo"
    "$ROOT_DIR/bin/client" 127.0.0.1 "$SERVER_PORT" "$port" \
        "$TMP_DIR/$name" <&"$fd" > "$TMP_DIR/$name.log" 2>&1 &
    PIDS+=("$!")
    if [ "$name" = "a" ]; then
        A_FD="$fd"
    fi
}

A_FD=""
start_client a "$A_PORT"
start_client b "$B_PORT"
start_client c "$C_PORT"
start_client d "$D_PORT"
start_client e "$E_PORT"
start_client f "$F_PORT"

for _ in $(seq 1 50); do
    ready=1
    for client in a b c d e f; do
        if ! grep -q "servidor de transferencia escuchando" "$TMP_DIR/$client.log"; then
            ready=0
        fi
    done
    if [ "$ready" -eq 1 ]; then
        break
    fi
    sleep 0.1
done
for client in a b c d e f; do
    grep -q "servidor de transferencia escuchando" "$TMP_DIR/$client.log"
done

grep -q "recibidos 1 vecinos" "$TMP_DIR/a.log"
grep -q "recibidos 1 vecinos" "$TMP_DIR/b.log"
grep -q "recibidos 2 vecinos" "$TMP_DIR/c.log"
grep -q "recibidos 1 vecinos" "$TMP_DIR/d.log"
grep -q "recibidos 1 vecinos" "$TMP_DIR/e.log"
grep -q "recibidos 0 vecinos" "$TMP_DIR/f.log"

NEAR_SIZE="$(wc -c < "$TMP_DIR/c/near.txt")"
NEAR_HASH="$("$HASH_BIN" "$TMP_DIR/c/near.txt")"

"$QUERY_BIN" "$A_PORT" "$ORIGIN_PORT" 777777 near.txt \
    > "$TMP_DIR/query.log"
grep -q "DRESULT 777777 $NEAR_SIZE $NEAR_HASH near.txt 127.0.0.1 $C_PORT" \
    "$TMP_DIR/query.log"
grep -q "^resultados 1$" "$TMP_DIR/query.log"

printf 'find -d near.txt\nfind -d far.txt\n' >&"$A_FD"
for _ in $(seq 1 40); do
    if grep -q "no hay resultados distribuidos para far.txt" "$TMP_DIR/a.log"; then
        break
    fi
    sleep 0.1
done

grep -q "resultados distribuidos para near.txt: 1" "$TMP_DIR/a.log"
grep -q "$NEAR_SIZE $NEAR_HASH near.txt 127.0.0.1 $C_PORT" "$TMP_DIR/a.log"
grep -q "no hay resultados distribuidos para far.txt" "$TMP_DIR/a.log"

printf 'integracion multi-cliente ok\n'
