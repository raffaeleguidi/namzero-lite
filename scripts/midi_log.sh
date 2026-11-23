#!/usr/bin/env bash
# Log persistente dei segnali MIDI ricevuti dal motore (debug).
# Uso:
#   scripts/midi_log.sh                          # motore sotto systemd -> journal
#   scripts/midi_log.sh --file /tmp/engine.log   # motore a mano -> tail del log
#   scripts/midi_log.sh --out /home/pi/midi.log  # file di destinazione
set -u
OUT=""
SRC=""
while [ $# -gt 0 ]; do
  case "$1" in
    --out) OUT="${2:-}"; shift 2 ;;
    --file) SRC="${2:-}"; shift 2 ;;
    *) shift ;;
  esac
done
FILTER='^(midi |  preset: slot|  mod:|  drive:|  boost:|  looper:|  filter:|  eq:)'
stamp() { while IFS= read -r l; do printf '%s %s\n' "$(date '+%F %T')" "$l"; done; }
sink() { if [ -n "$OUT" ]; then mkdir -p "$(dirname "$OUT")"; tee -a "$OUT"; else cat; fi; }
if [ -n "$SRC" ]; then
  echo "[midi_log] tail $SRC${OUT:+ -> $OUT}" >&2
  tail -n 0 -F "$SRC" 2>/dev/null | grep --line-buffered -E "$FILTER" | stamp | sink
else
  echo "[midi_log] journalctl namzero-lite${OUT:+ -> $OUT}" >&2
  journalctl -u namzero-lite -f -o cat 2>/dev/null | grep --line-buffered -E "$FILTER" | stamp | sink
fi
