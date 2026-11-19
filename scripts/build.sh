#!/usr/bin/env bash
# Build + tuning del motore sul Pi, da ripetere dopo ogni modifica.
#
# Fa, in ordine:
#   1. controlla che il motore non sia in esecuzione (ETXTBSY e contesa CPU)
#   2. ferma il bridge BLE: libera ~67 MB e la build non finisce in swap
#   3. compila con UN job (415 MB di RAM: -j2 fa uccidere cc1plus dall'OOM)
#   4. lancia i test
#   5. applica le capability: SCHED_FIFO + mlockall senza essere root
#   6. mette il governor della CPU su performance
#   7. riavvia il bridge se era acceso
#
# Uso:  ./scripts/build.sh            (chiede la password per sudo)
#       JOBS=2 ./scripts/build.sh     (solo se la macchina e' libera)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build/namzero-lite"
NAM_DIR="${NAM_DIR:-/home/pi/nam}"
BRIDGE_RUN="$PWD/ble-bridge-py/run.sh"
BRIDGE_WAS_RUNNING=0

if pgrep -f "namzero-lite -p" >/dev/null 2>&1; then
  echo "errore: il motore e' gia' in esecuzione. Fermalo (q o Ctrl+C) prima di compilare." >&2
  exit 1
fi

if pgrep -f "ble-bridge-py/main.py" >/dev/null 2>&1; then
  BRIDGE_WAS_RUNNING=1
fi
if pgrep -x ble-midi-bridge >/dev/null 2>&1; then
  BRIDGE_WAS_RUNNING=1
fi
if [ "$BRIDGE_WAS_RUNNING" = 1 ]; then
  echo "== fermo il bridge BLE (libera memoria per la build)"
  pkill -f "ble-bridge-py/main.py" || true
  pkill -x ble-midi-bridge || true
  sleep 1
fi

echo "== memoria disponibile"
free -m | head -2

echo "== configure"
cmake -B build -DCMAKE_BUILD_TYPE=Release ${NAM_DIR:+-DNAM_DIR="$NAM_DIR"}

echo "== build (-j${JOBS:-1})"
cmake --build build -j"${JOBS:-1}"

echo "== test"
(cd build && ctest --output-on-failure)

echo "== capability (realtime + mlockall)"
sudo setcap cap_sys_nice,cap_ipc_lock+ep "$BIN"

echo "== governor performance"
sudo sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f"; done'

echo "== verifica"
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  printf "   %s -> %s\n" "$f" "$(cat "$f")"
done
python3 - "$BIN" <<'PY'
import os
import sys

path = sys.argv[1]
try:
    caps = os.getxattr(path, "security.capability")
    print(f"   capability: {caps.hex()}")
except OSError:
    print("   capability: ASSENTI")
PY

if [ "$BRIDGE_WAS_RUNNING" = 1 ] && [ -x "$BRIDGE_RUN" ]; then
  echo "== riavvio il bridge BLE (Python)"
  setsid nohup "$BRIDGE_RUN" >/tmp/ble.log 2>&1 </dev/null &
  sleep 1
fi

echo
echo "pronto. Esempio:"
echo "   ./$BIN -p config/presets/vox_wah.json"
