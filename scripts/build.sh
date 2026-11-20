#!/usr/bin/env bash
# Build + test del motore. NON usa sudo: si puo' lanciare sempre.
#
# Dopo una build l'eseguibile perde le capability (realtime + mlockall): per
# riaverle va lanciato ./scripts/install.sh, che e' l'unico che usa sudo.
#
# Uso:  ./scripts/build.sh
#       JOBS=2 ./scripts/build.sh          (solo se la macchina e' libera)
#       NAM_DIR=/altro/nam ./scripts/build.sh
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="build/namzero-lite"
NAM_DIR="${NAM_DIR:-/home/pi/nam}"

if pgrep -x namzero-lite >/dev/null 2>&1; then
  echo "errore: il motore e' gia' in esecuzione. Fermalo (q o Ctrl+C) prima di compilare." >&2
  exit 1
fi

echo "== memoria disponibile"
free -m | head -2

echo "== configure"
cmake -B build -DCMAKE_BUILD_TYPE=Release ${NAM_DIR:+-DNAM_DIR="$NAM_DIR"}

echo "== build (-j${JOBS:-1})"
cmake --build build -j"${JOBS:-1}"

echo "== test"
(cd build && ctest --output-on-failure)

echo
echo "build ok: $BIN"
echo "ora esegui ./scripts/install.sh (capability, governor, servizi)"
