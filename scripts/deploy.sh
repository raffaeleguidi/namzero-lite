#!/usr/bin/env bash
# Copia il binario cross-compilato (build-arm64) sul Pi e riapplica setcap.
# Uso: ./scripts/cross-build.sh && ./scripts/deploy.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build-arm64/namzero-lite"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_namzero}"
PI="${PI:-pi@namzero-lite}"

[ -x "$BIN" ] || { echo "manca $BIN: prima ./scripts/cross-build.sh" >&2; exit 1; }

echo "== fermo il motore sul Pi (se attivo)"
# Se systemd e' attivo ma `sudo systemctl stop` non e' permesso, NON si ferma
# subito: il binario si sostituisce con rename (il processo tiene il vecchio
# inode) e a fine copia si manda SIGKILL. La unit ha Restart=on-failure, quindi
# systemd rilancia da solo il binario nuovo senza bisogno di sudo.
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" '
restart_kill=0
if systemctl is-active --quiet namzero-lite 2>/dev/null; then
  if sudo -n systemctl stop namzero-lite 2>/dev/null; then
    echo "servizio namzero-lite fermato con systemctl"
  else
    echo "systemctl senza sudo: copio il binario e poi forzo il riavvio (SIGKILL)"
    restart_kill=1
  fi
fi
if [ "$restart_kill" = 0 ]; then
  pkill -x namzero-lite 2>/dev/null || true
  for i in $(seq 1 20); do
    pgrep -x namzero-lite >/dev/null 2>&1 || break
    sleep 0.2
  done
  if pgrep -x namzero-lite >/dev/null 2>&1; then
    echo "ATTENZIONE: il motore e ancora in esecuzione (rilanciato da systemd col binario vecchio)."
    echo "  Al termine del deploy serve: sudo systemctl restart namzero-lite"
  fi
fi
echo "$restart_kill" > /tmp/namzero-deploy-restart-kill
'

# La config viva (preset, presets.json, wifi.json, tone3000.json) si modifica
# sul Pi via web UI: il deploy la salta e sincronizza il resto (midi.json ecc.).
echo "== sincronizzo config/ (tranne preset e segreti vivi)"
rsync -a -e "ssh -i $SSH_KEY -o BatchMode=yes" \
    --exclude 'presets.json' --exclude 'presets/' --exclude 'wifi.json' \
    --exclude 'tone3000.json' \
    config/ "$PI:~/namzero-lite/config/"

echo "== sincronizzo la web UI"
rsync -a -e "ssh -i $SSH_KEY -o BatchMode=yes" web/ "$PI:~/namzero-lite/web/"

echo "== sincronizzo gli script WiFi (reti note + AP di fallback)"
rsync -a -e "ssh -i $SSH_KEY -o BatchMode=yes" \
    scripts/wifi-connect.sh scripts/wifi_manager.py \
    "$PI:~/namzero-lite/scripts/"

echo "== copio il binario (rename: se il motore e' in esecuzione non da' ETXTBSY)"
scp -i "$SSH_KEY" -o BatchMode=yes "$BIN" "$PI:~/namzero-lite/build/namzero-lite.new"
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'mv -f ~/namzero-lite/build/namzero-lite.new ~/namzero-lite/build/namzero-lite'
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'sudo -n setcap cap_sys_nice,cap_ipc_lock+ep ~/namzero-lite/build/namzero-lite 2>/dev/null && echo setcap ok || echo "setcap non applicato: per il servizio bastano le AmbientCapabilities; per l\047uso manuale rilancia scripts/install.sh"'
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" '
kill_it=$(cat /tmp/namzero-deploy-restart-kill 2>/dev/null || echo 0)
rm -f /tmp/namzero-deploy-restart-kill
if [ "$kill_it" = 1 ]; then
  pkill -KILL -x namzero-lite 2>/dev/null || true
  for i in $(seq 1 40); do
    if systemctl is-active --quiet namzero-lite 2>/dev/null && pgrep -x namzero-lite >/dev/null 2>&1; then
      echo "motore riavviato da systemd col binario nuovo (PID $(pgrep -x namzero-lite | head -1))"
      exit 0
    fi
    sleep 0.25
  done
  echo "ATTENZIONE: systemd non ha ancora rilanciato il motore"
  echo "  avvia a mano: cd ~/namzero-lite && nohup ./build/namzero-lite >>/tmp/namzero-lite-manual.log 2>&1 &"
fi
'
echo "fatto: se il servizio non e' stato riavviato da systemd usare: sudo systemctl restart namzero-lite"
