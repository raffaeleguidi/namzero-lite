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
# Senza sudo il servizio non si puo' fermare con systemctl: si usa pkill
# (SIGTERM, gestito: il motore esce pulito). Se systemd lo rilanciasse, il
# processo resterebbe col binario vecchio: lo si rileva e lo si segnala.
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" '
if systemctl is-active --quiet namzero-lite 2>/dev/null; then
  if sudo -n systemctl stop namzero-lite 2>/dev/null; then
    echo "servizio namzero-lite fermato"
  else
    echo "ATTENZIONE: namzero-lite.service e attivo e sudo non e disponibile: uso pkill"
  fi
fi
pkill -x namzero-lite 2>/dev/null || true
for i in $(seq 1 20); do
  pgrep -x namzero-lite >/dev/null 2>&1 || break
  sleep 0.2
done
if pgrep -x namzero-lite >/dev/null 2>&1; then
  echo "ATTENZIONE: il motore e ancora in esecuzione (rilanciato da systemd col binario vecchio)."
  echo "  Al termine del deploy serve: sudo systemctl restart namzero-lite"
fi
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

echo "== copio il binario (rename: se il motore e' in esecuzione non da' ETXTBSY)"
scp -i "$SSH_KEY" -o BatchMode=yes "$BIN" "$PI:~/namzero-lite/build/namzero-lite.new"
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'mv -f ~/namzero-lite/build/namzero-lite.new ~/namzero-lite/build/namzero-lite'
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'sudo -n setcap cap_sys_nice,cap_ipc_lock+ep ~/namzero-lite/build/namzero-lite 2>/dev/null && echo setcap ok || echo "setcap non applicato: per il servizio bastano le AmbientCapabilities; per l\047uso manuale rilancia scripts/install.sh"'
echo "fatto: riavvia il servizio (sudo systemctl restart namzero-lite) o avvia a mano"
