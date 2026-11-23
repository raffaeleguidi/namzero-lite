#!/usr/bin/env bash
# Logga su stdout i segnali MIDI ricevuti dal motore. Ctrl-C per fermare.
# Sceglie da solo la sorgente: journal del servizio se attivo, altrimenti il
# log del motore avviato a mano (/tmp/namzero-lite-manual.log).
# Uso: scripts/midi_log.sh [file-log]
set -u
SRC="${1:-}"
if [ -z "$SRC" ]; then
  if systemctl is-active --quiet namzero-lite 2>/dev/null; then SRC="journal"; else SRC="/tmp/namzero-lite-manual.log"; fi
fi
FILTER='^(midi |  preset: slot|  mod:|  drive:|  boost:|  looper:|  filter:|  eq:)'
if [ "$SRC" = "journal" ]; then
  journalctl -u namzero-lite -f -o cat 2>/dev/null | grep --line-buffered -E "$FILTER"
else
  [ -f "$SRC" ] || { echo "[midi_log] log non trovato: $SRC" >&2; exit 1; }
  tail -n 0 -F "$SRC" 2>/dev/null | grep --line-buffered -E "$FILTER"
fi
