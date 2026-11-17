#!/usr/bin/env bash
# Setup di base per namzero-lite su Raspberry Pi OS Lite 64-bit.
# Non tocca il kernel: per quello vedi docs/rt-setup.md.
#
# Uso: ./scripts/setup_pi.sh [--rt]
set -euo pipefail

WITH_RT=0
for arg in "$@"; do
  case "$arg" in
    --rt) WITH_RT=1 ;;
    -h|--help)
      echo "uso: $0 [--rt]"
      exit 0
      ;;
    *)
      echo "argomento sconosciuto: $arg" >&2
      exit 2
      ;;
  esac
done

if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "attenzione: architettura $(uname -m), attesa aarch64" >&2
fi

echo "== pacchetti"
sudo apt-get update
sudo apt-get install -y build-essential cmake git libasound2-dev alsa-utils

echo "== gruppo audio"
sudo usermod -aG audio "$USER"

echo "== limits realtime"
sudo tee /etc/security/limits.d/99-audio.conf >/dev/null <<'EOF'
@audio - rtprio 95
@audio - memlock unlimited
EOF

echo "== usb autosuspend disattivato"
sudo tee /etc/modprobe.d/namzero-usb.conf >/dev/null <<'EOF'
options usbcore autosuspend=-1
EOF

if [[ "$WITH_RT" -eq 1 ]]; then
  echo "== kernel realtime"
  if apt-cache show linux-image-rt-arm64 >/dev/null 2>&1; then
    sudo apt-get install -y linux-image-rt-arm64
  else
    echo "kernel RT non presente nei repo: usa il kernel standard con isolcpus"
    echo "vedi docs/rt-setup.md (sezione 'Fallback senza kernel RT')"
  fi
fi

cat <<'EOF'

== fatto
Prossimi passi:
  1. riavvia (o effettua logout/login per applicare i limiti)
  2. verifica il kernel:  uname -a
  3. build:
       cmake -B build -DCMAKE_BUILD_TYPE=Release
       cmake --build build -j2
  4. verifica l'XTONE:
       aplay -l; arecord -l
       cat /proc/asound/card*/stream0
       amidi -l; aconnect -l
EOF
