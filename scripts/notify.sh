#!/usr/bin/env bash
# Invia un aggiornamento su Telegram tramite il bot del gateway DSH.
# Token e destinatario si leggono dal profilo: non sono scritti qui.
#
#   ./scripts/notify.sh "messaggio"
set -euo pipefail

PROFILE="${DSH_PROFILE_CONFIG:-$HOME/.dsh/profiles/web/cordis.patch.yml}"

[ -f "$PROFILE" ] || { echo "profilo non trovato: $PROFILE" >&2; exit 1; }

TOKEN=$(grep -oE 'botToken:[[:space:]]*[^[:space:]]+' "$PROFILE" | head -1 \
        | sed -E 's/botToken:[[:space:]]*//' | tr -d "\"'")
CHAT=$(grep -oE 'allowedUserIds: \[[0-9, ]+\]' "$PROFILE" | head -1 \
       | grep -oE '[0-9]+' | head -1)

[ -n "$TOKEN" ] || { echo "botToken non trovato nel profilo" >&2; exit 1; }
[ -n "$CHAT" ]  || { echo "allowedUserIds non trovato nel profilo" >&2; exit 1; }

MSG="${1:-(nessun messaggio)}"

curl -s -m 15 -X POST "https://api.telegram.org/bot$TOKEN/sendMessage" \
    -d chat_id="$CHAT" --data-urlencode "text=$MSG" \
    | python3 -c "import sys,json; d=json.load(sys.stdin); sys.exit(0 if d.get('ok') else 1)" \
    && echo "notifica inviata"
