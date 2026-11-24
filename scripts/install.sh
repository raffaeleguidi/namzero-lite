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
if sudo -n setcap cap_sys_nice,cap_ipc_lock+ep "$BIN" 2>/dev/null; then
  echo "   setcap ok"
else
  echo "   ATTENZIONE: setcap non applicato (sudo non disponibile per setcap)."
  echo "   Il servizio usa comunque AmbientCapabilities; per l'uso manuale:"
  echo "     sudo setcap cap_sys_nice,cap_ipc_lock+ep $BIN"
fi

say "governor performance (persistente)"
if sudo -n sh -c 'for f in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$f"; done' 2>/dev/null; then
  echo "   governor impostato"
else
  echo "   ATTENZIONE: governor non modificato (serve sudo)."
fi

# I comandi systemd possono essere negati anche quando setcap e' permesso.
# In quel caso si saltano unit e restart: le unit esistenti restano valide e
# il motore viene lasciato attivo (il deploy lo riavvia o si avvia a mano).
SYSTEMD_OK=0
if sudo -n systemctl daemon-reload 2>/dev/null; then SYSTEMD_OK=1; fi

if [ "$SYSTEMD_OK" = 1 ]; then
  sudo -n tee "/etc/systemd/system/${GOV_SERVICE}.service" >/dev/null <<'UNIT'
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
  sudo -n systemctl daemon-reload
  sudo -n systemctl enable --now "$GOV_SERVICE" >/dev/null 2>&1 || true
  sudo -n systemctl restart "$GOV_SERVICE" >/dev/null 2>&1 || true
fi

say "venv e dipendenze del bridge"
if [ ! -x "$VENV/bin/python" ]; then
  python3 -m venv "$VENV"
fi
"$VENV/bin/pip" install -q -r ble-bridge-py/requirements.txt

say "unit del bridge, del motore e della web UI"
if [ "$SYSTEMD_OK" = 1 ]; then
  sudo -n cp "systemd/${BRIDGE}.service" /etc/systemd/system/
  sudo -n cp "systemd/${SERVICE}.service" /etc/systemd/system/
  sudo -n cp "systemd/namzero-web.service" /etc/systemd/system/
  sudo -n cp "systemd/namzero-wifi.service" /etc/systemd/system/
  sudo -n systemctl daemon-reload
  sudo -n systemctl enable "$BRIDGE" "$SERVICE" namzero-web namzero-wifi >/dev/null 2>&1 || true
  sudo -n systemctl restart "$BRIDGE" || true
  sudo -n systemctl restart "$SERVICE" || true
  sudo -n systemctl restart namzero-web || true
  say "drop-in DNS per il nome namzero-lite nell'AP"
  sudo -n mkdir -p /etc/NetworkManager/dnsmasq-shared.d 2>/dev/null || true
  printf 'address=/namzero-lite/10.42.0.1\n' | sudo -n tee /etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf >/dev/null 2>&1 || true
  sudo -n systemctl restart namzero-wifi || true
else
  echo "   ATTENZIONE: sudo systemctl non disponibile: unit non copiate/riavviate."
  echo "   Le unit gia' installate restano valide e il motore resta attivo."
  echo "   Per applicarle a mano:"
  echo "     sudo cp systemd/*.service /etc/systemd/system/"
  echo "     sudo systemctl daemon-reload"
  echo "     sudo systemctl enable --now namzero-ble-bridge namzero-lite namzero-web namzero-wifi"
  echo "     sudo systemctl restart namzero-lite"
fi

say "namzero-ctl: controllo servizi senza password di root"
if [ "$SYSTEMD_OK" = 1 ]; then
  sudo -n install -m 0755 scripts/namzero-ctl /usr/local/sbin/namzero-ctl
  printf 'pi ALL=(root) NOPASSWD: /usr/local/sbin/namzero-ctl\n' \
    | sudo -n tee /etc/sudoers.d/namzero >/dev/null
  sudo -n chmod 0440 /etc/sudoers.d/namzero
  sudo -n visudo -cf /etc/sudoers.d/namzero >/dev/null
  echo "   ok: sudo namzero-ctl restart lite (senza password)"
else
  echo "   salta: sudo non disponibile"
fi

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
if [ "$SYSTEMD_OK" = 1 ]; then
  echo "Servizio attivo. Per usare il motore a mano: sudo systemctl stop $SERVICE"
else
  echo "Install completato senza sudo systemctl: verifica che il motore sia attivo"
  echo "(pgrep -x namzero-lite); per gestirlo con systemd:"
  echo "  sudo systemctl restart $SERVICE"
fi
