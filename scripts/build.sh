#!/usr/bin/env bash
# Build + tuning + deploy del bridge, tutto in un comando.
#
# Da lanciare DALLA CONSOLE del Pi (chiede la password per sudo). Fa:
#   1. si rifiuta di compilare se il motore e' in esecuzione (ETXTBSY + contesa CPU)
#   2. ferma il bridge BLE: libera ~25 MB e la build non finisce in swap
#   3. compila con UN job (415 MB di RAM: -j2 fa uccidere cc1plus dall'OOM)
#   4. lancia i test
#   5. applica le capability cap_sys_nice,cap_ipc_lock (realtime + mlockall)
#   6. mette il governor su performance E lo rende persistente (unit systemd)
#   7. prepara il venv e le dipendenze del bridge (dbus-fast)
#   8. installa/aggiorna la unit del bridge e la (ri)avvia
#   9. verifica: capability, governor, connessione al pedale
#
# Uso:  ./scripts/build.sh
#       JOBS=2 ./scripts/build.sh          (solo se la macchina e' libera)
#       NAM_DIR=/altro/nam ./scripts/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build/namzero-lite"
NAM_DIR="${NAM_DIR:-/home/pi/nam}"
VENV="${VENV:-$HOME/ble-venv}"
BRIDGE_DIR="$PWD/ble-bridge-py"
SERVICE="namzero-ble-bridge"
GOV_SERVICE="cpu-performance"

say() { printf "\n== %s\n" "$*"; }

if pgrep -f "namzero-lite -p" >/dev/null 2>&1; then
  echo "errore: il motore e' gia' in esecuzione. Fermalo (q o Ctrl+C) prima di compilare." >&2
  exit 1
fi

say "fermo il bridge BLE (libera memoria per la build)"
sudo systemctl stop "$SERVICE" 2>/dev/null || true
pkill -f "ble-bridge-py/main.py" 2>/dev/null || true
sleep 1

say "memoria disponibile"
free -m | head -2

say "configure"
cmake -B build -DCMAKE_BUILD_TYPE=Release ${NAM_DIR:+-DNAM_DIR="$NAM_DIR"}

say "build (-j${JOBS:-1})"
cmake --build build -j"${JOBS:-1}"

say "test"
(cd build && ctest --output-on-failure)

say "capability (realtime + mlockall)"
sudo setcap cap_sys_nice,cap_ipc_lock+ep "$BIN"

say "governor performance"
sudo sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f"; done'
sudo tee "/etc/systemd/system/${GOV_SERVICE}.service" >/dev/null <<'EOF'
[Unit]
Description=CPU governor performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f"; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
sudo systemctl daemon-reload
sudo systemctl enable "$GOV_SERVICE" >/dev/null 2>&1 || true
sudo systemctl restart "$GOV_SERVICE" >/dev/null 2>&1 || true

say "venv e dipendenze del bridge"
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install -q -r "$BRIDGE_DIR/requirements.txt"

say "unit del bridge"
sudo cp "systemd/${SERVICE}.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable "$SERVICE" >/dev/null 2>&1 || true
sudo systemctl restart "$SERVICE"

say "verifica"
for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  printf "   %s -> %s\n" "$f" "$(cat "$f")"
done
python3 - "$BIN" <<'PY'
import os
import sys

try:
    print("   capability:", os.getxattr(sys.argv[1], "security.capability").hex())
except OSError:
    print("   capability: ASSENTI")
PY

connected=0
for _ in $(seq 1 20); do
  if journalctl -u "$SERVICE" --since "30 seconds ago" --no-pager 2>/dev/null \
      | grep -q "notifiche BLE MIDI attive"; then
    connected=1
    break
  fi
  sleep 1
done
if [ "$connected" = 1 ]; then
  echo "   bridge: connesso al pedale"
else
  echo "   bridge: attivo ma non ancora connesso (pedale spento o non accoppiato):"
  journalctl -u "$SERVICE" -n 5 --no-pager 2>/dev/null | sed 's/^/     /'
fi

echo
echo "pronto. Motore:"
echo "   ./$BIN -p config/presets/<preset>.json"
echo "Per vedere i CC: aggiungi --midi-log"
