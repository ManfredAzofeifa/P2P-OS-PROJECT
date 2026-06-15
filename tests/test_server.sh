#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${P2P_TEST_SERVER_PORT:-39091}"
SERVER_LOG="$(mktemp)"
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT

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

exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 41001 1\nFILE 12 abcdefabcdefabcdefabcdefabcdefab sample.txt\nEND\n' >&3
read -r line <&3
[ "$line" = "OK registered 1 files" ]
read -r line <&3
[ "$line" = "NEIGHBORS 0" ]
read -r line <&3
[ "$line" = "END" ]
exec 3<&-
exec 3>&-

exec 4<>"/dev/tcp/127.0.0.1/$PORT"
printf 'FIND sample.txt\n' >&4
read -r line <&4
[ "$line" = "PEERS 1" ]
read -r line <&4
[ "$line" = "PEER 127.0.0.1 41001" ]
read -r line <&4
[ "$line" = "END" ]
exec 4<&-
exec 4>&-

exec 5<>"/dev/tcp/127.0.0.1/$PORT"
printf 'LOOKUP 12 abcdefabcdefabcdefabcdefabcdefab\n' >&5
read -r line <&5
[ "$line" = "PEERS 1" ]
read -r line <&5
[ "$line" = "PEER 127.0.0.1 41001" ]
read -r line <&5
[ "$line" = "END" ]
exec 5<&-
exec 5>&-

printf 'server protocol ok\n'
