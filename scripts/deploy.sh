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
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'sudo -n systemctl stop namzero-lite 2>/dev/null || true; pkill -x namzero-lite 2>/dev/null || true'

# La config viva sta sul Pi (l'utente salva da li'): si aggiungono solo i file
# nuovi, senza sovrascrivere preset, presets.json e midi.json. Per forzare un
# push della config del repo: PUSH_CONFIG=1 ./scripts/deploy.sh
if [ "${PUSH_CONFIG:-0}" = "1" ]; then
    echo "== sincronizzo config/ (FORZATO)"
    rsync -a -e "ssh -i $SSH_KEY -o BatchMode=yes" config/ "$PI:~/namzero-lite/config/"
else
    echo "== aggiungo i file nuovi in config/ (senza sovrascrivere)"
    rsync -a --ignore-existing -e "ssh -i $SSH_KEY -o BatchMode=yes" config/ "$PI:~/namzero-lite/config/"
fi

echo "== copio il binario (rename: se il motore e' in esecuzione non da' ETXTBSY)"
scp -i "$SSH_KEY" -o BatchMode=yes "$BIN" "$PI:~/namzero-lite/build/namzero-lite.new"
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'mv -f ~/namzero-lite/build/namzero-lite.new ~/namzero-lite/build/namzero-lite'
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'sudo -n setcap cap_sys_nice,cap_ipc_lock+ep ~/namzero-lite/build/namzero-lite 2>/dev/null && echo setcap ok || echo "setcap non applicato: per il servizio bastano le AmbientCapabilities; per l\047uso manuale rilancia scripts/install.sh"'
echo "fatto: riavvia il servizio (sudo systemctl restart namzero-lite) o avvia a mano"
