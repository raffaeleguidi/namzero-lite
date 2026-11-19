#!/usr/bin/env bash
# Avvia il bridge con il Python del venv (bleak non e' nei pacchetti di sistema).
set -euo pipefail
cd "$(dirname "$0")"
exec "${BLE_PYTHON:-$HOME/ble-venv/bin/python}" main.py "$@"
