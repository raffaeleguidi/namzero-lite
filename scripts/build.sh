#!/usr/bin/env bash
# Build sul Pi + capability per girare senza sudo.
#
# Girare senza root e' preferibile: il processo non ha privilegi generali, solo
# i due che servono (priorita' FIFO e blocco della memoria).
set -euo pipefail

cd "$(dirname "$0")/.."

cmake -B build -DCMAKE_BUILD_TYPE=Release
# Un job solo: su Zero 2W (415 MB) due traduzioni pesanti in parallelo, con
# json.hpp di mezzo, fanno terminare cc1plus dal kernel per memoria esaurita.
# Con JOBS=2 si puo' forzare, se la macchina e' libera.
cmake --build build -j"${JOBS:-1}"

BIN="build/namzero-lite"

# setcap e' idempotente: si riapplica dopo ogni build, perche' un eseguibile
# ricompilato perde le capability.
echo "applico le capability a $BIN"
sudo /usr/sbin/setcap cap_sys_nice,cap_ipc_lock+ep "$BIN"

# getcap sta in libcap2-bin, che non e' sempre installato: verifica via xattr.
python3 - "$BIN" <<'PYEOF'
import os, sys
path = sys.argv[1]
raw = os.getxattr(path, "security.capability")
print("security.capability:", raw.hex())
PYEOF

echo "eseguibile pronto: ./$BIN"
