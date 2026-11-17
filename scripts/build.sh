#!/usr/bin/env bash
# Build sul Pi + capability per girare senza sudo.
#
# Girare senza root e' preferibile: il processo non ha privilegi generali, solo
# i due che servono (priorita' FIFO e blocco della memoria).
set -euo pipefail

cd "$(dirname "$0")/.."

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2

BIN="build/namzero-lite"
if ! getcap "$BIN" 2>/dev/null | grep -q cap_sys_nice; then
    echo "applico le capability a $BIN (serve sudo, una volta per build)"
    sudo /usr/sbin/setcap cap_sys_nice,cap_ipc_lock+ep "$BIN"
fi

echo
getcap "$BIN"
echo "eseguibile pronto: ./$BIN"
