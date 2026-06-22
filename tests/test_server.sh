#!/bin/bash
set -eu

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PORT="${P2P_TEST_SERVER_PORT:-39091}"
SERVER_LOG="$(mktemp)"
trap 'kill "$SERVER_PID" 2>/dev/null || true; rm -f "$SERVER_LOG"' EXIT

"$ROOT_DIR/bin/server" "$PORT" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 30); do
    if grep -q "escuchando" "$SERVER_LOG"; then
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
[ "$line" = "OK registrado 1 archivos" ]
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

exec 6<>"/dev/tcp/127.0.0.1/$PORT"
printf 'REGISTER 41002 1 192.0.2.10\nFILE 9 11111111111111111111111111111111 vm.txt\nEND\n' >&6
read -r line <&6
[ "$line" = "OK registrado 1 archivos" ]
while [ "$line" != "END" ]; do
    read -r line <&6
done
exec 6<&-
exec 6>&-

exec 7<>"/dev/tcp/127.0.0.1/$PORT"
printf 'FIND vm.txt\n' >&7
read -r line <&7
[ "$line" = "PEERS 1" ]
read -r line <&7
[ "$line" = "PEER 192.0.2.10 41002" ]
read -r line <&7
[ "$line" = "END" ]
exec 7<&-
exec 7>&-

printf 'protocolo del servidor ok\n'
