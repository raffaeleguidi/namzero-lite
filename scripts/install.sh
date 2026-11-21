#!/usr/bin/env bash
# Installa tutto sul Pi, in modo idempotente. E' l'unico script che usa sudo.
#
#   - capability per l'uso MANUALE (il servizio le riceve da systemd)
#   - governor performance persistente (unit cpu-performance)
#   - venv + dipendenze del bridge BLE
#   - unit del bridge e del motore, abilitate e avviate
#   - verifica finale
#
# Le capability del file sopravvivono ai riavvii ma NON alle ricompilazioni:
# il servizio usa AmbientCapabilities, quindi non gli serve nulla; dopo una
# build rilancia questo script per l'uso manuale.
#
# Uso: ./scripts/build.sh && ./scripts/install.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build/namzero-lite"
VENV="${VENV:-$HOME/ble-venv}"
SERVICE="namzero-lite"
BRIDGE="namzero-ble-bridge"
GOV_SERVICE="cpu-performance"

say() { printf "\n== %s\n" "$*"; }

if [ ! -x "$BIN" ]; then
  echo "errore: $BIN non esiste. Prima compila: ./scripts/build.sh" >&2
  exit 1
fi

say "capability per l'uso manuale"
sudo setcap cap_sys_nice,cap_ipc_lock+ep "$BIN"

say "governor performance (persistente)"
sudo sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f"; done'
sudo tee "/etc/systemd/system/${GOV_SERVICE}.service" >/dev/null <<'UNIT'
[Unit]
Description=CPU governor performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f"; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload
sudo systemctl enable --now "$GOV_SERVICE" >/dev/null 2>&1 || true
sudo systemctl restart "$GOV_SERVICE" >/dev/null 2>&1 || true

say "venv e dipendenze del bridge"
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install -q -r ble-bridge-py/requirements.txt

say "unit del bridge, del motore e della web UI"
sudo cp "systemd/${BRIDGE}.service" /etc/systemd/system/
sudo cp "systemd/${SERVICE}.service" /etc/systemd/system/
sudo cp "systemd/namzero-web.service" /etc/systemd/system/
sudo cp "systemd/namzero-wifi.service" /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable "$BRIDGE" "$SERVICE" namzero-web namzero-wifi >/dev/null 2>&1 || true
sudo systemctl restart "$BRIDGE"
sudo systemctl restart "$SERVICE"
sudo systemctl restart namzero-web
sudo systemctl restart namzero-wifi

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
for unit in "$BRIDGE" "$SERVICE"; do
  printf "   %s: %s\n" "$unit" "$(systemctl is-active "$unit" 2>/dev/null || true)"
done
sleep 3
echo
echo "--- motore ---"
journalctl -u "$SERVICE" -n 12 --no-pager 2>/dev/null | tail -10
echo
echo "--- bridge ---"
journalctl -u "$BRIDGE" -n 6 --no-pager 2>/dev/null | tail -4
echo
echo "Servizio attivo. Per usare il motore a mano: sudo systemctl stop $SERVICE"
