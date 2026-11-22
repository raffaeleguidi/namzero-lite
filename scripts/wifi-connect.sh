#!/usr/bin/env bash
# Entry point del servizio WiFi (systemd/namzero-wifi.service).
#
# La logica sta in scripts/wifi_manager.py: reti note con priorita', AP di
# fallback con DHCP condiviso, una sola radio (AP oppure STA) e isteresi.
# Opzioni utili:
#   ./scripts/wifi-connect.sh --dry-run          # decisione senza toccare NM
#   ./scripts/wifi-connect.sh --dry-run --simulate no-link
#   ./scripts/wifi-connect.sh --dry-run --simulate known
#   ./scripts/wifi-connect.sh --test             # test delle decisioni
#   journalctl -u namzero-wifi -f                # log con tag [wifi]
set -u
cd "$(dirname "$0")/.." || exit 1
exec python3 scripts/wifi_manager.py "$@"
