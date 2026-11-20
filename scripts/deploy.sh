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

echo "== copio il binario"
scp -i "$SSH_KEY" -o BatchMode=yes "$BIN" "$PI:~/namzero-lite/build/namzero-lite"
ssh -i "$SSH_KEY" -o BatchMode=yes "$PI" 'sudo -n setcap cap_sys_nice,cap_ipc_lock+ep ~/namzero-lite/build/namzero-lite && echo setcap ok'
echo "fatto: avvia il motore (o systemctl start namzero-lite)"
