#!/usr/bin/env bash
# Passi privilegiati, da rifare dopo ogni build. E' l'unico script che usa sudo.
#
#   1. setcap cap_sys_nice,cap_ipc_lock sul binario (SCHED_FIFO + mlockall):
#      un eseguibile ricompilato le perde;
#   2. governor performance, reso persistente con la unit cpu-performance;
#   3. venv e dipendenze del bridge BLE (dbus-fast);
#   4. unit del bridge installata/aggiornata e riavviata;
#   5. verifica finale: capability, governor, connessione al pedale.
#
# Uso:  ./scripts/permissions.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build/namzero-lite"
VENV="${VENV:-$HOME/ble-venv}"
BRIDGE_DIR="$PWD/ble-bridge-py"
SERVICE="namzero-ble-bridge"
GOV_SERVICE="cpu-performance"

say() { printf "\n== %s\n" "$*"; }

if [ ! -x "$BIN" ]; then
  echo "errore: $BIN non esiste. Prima compila: ./scripts/build.sh" >&2
  exit 1
fi

say "capability (realtime + mlockall)"
sudo setcap cap_sys_nice,cap_ipc_lock+ep "$BIN"

say "governor performance (persistente)"
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
