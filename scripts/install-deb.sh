#!/usr/bin/env bash
# Installazione interattiva del pacchetto .deb namzero-lite.
#
# Non e' scripts/install.sh (quello serve a un checkout git: capability,
# governor, unit da sorgente). Questo script installa il .deb gia' costruito
# e chiede la modalita' prima di farlo, cosi' la scelta non dipende dal
# frontend di debconf (che con il doppio click in Files non farebbe domande).
#
# Uso:
#   ./scripts/install-deb.sh                    # sceglie il .deb piu' recente in dist/
#   ./scripts/install-deb.sh dist/foo.deb
#   ./scripts/install-deb.sh --mode desktop     # senza domande
#   ./scripts/install-deb.sh --purge            # disinstalla tutto
#
# Note sul perche' di `sh -c`: `sudo VAR=x comando` NON passa VAR al comando,
# perche' sudo fa env_reset. La variabile va impostata *dentro* la shell che
# gira come root, altrimenti il config script di debconf non la vede.
set -euo pipefail
cd "$(dirname "$0")/.."

PKG=namzero-lite
MODE=""
DEB=""
PURGE=0

usage() {
  sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
  --mode) MODE="${2:-}"; shift 2 ;;
  --mode=*) MODE="${1#*=}"; shift ;;
  --purge | --remove) PURGE=1; shift ;;
  -h | --help) usage 0 ;;
  -*) echo "opzione sconosciuta: $1" >&2; usage 2 ;;
  *) DEB="$1"; shift ;;
  esac
done

if [ "$(id -u)" -eq 0 ]; then
  echo "errore: non lanciare come root; lo script usa sudo dove serve." >&2
  exit 1
fi

for c in sudo apt-get dpkg; do
  command -v "$c" >/dev/null 2>&1 || { echo "errore: serve $c" >&2; exit 1; }
done

# ── disinstallazione ───────────────────────────────────────────────────────
if [ "$PURGE" = "1" ]; then
  echo "Rimozione di $PKG."
  echo "  - 'remove' conserva configurazione (/etc/namzero) e dati (/var/lib/namzero)"
  echo "  - 'purge' elimina anche quelli"
  printf 'Rimuovere anche configurazione e dati? [s/N] '
  read -r ans || ans=""
  if [ "$ans" = "s" ] || [ "$ans" = "S" ]; then
    sudo apt-get -y purge "$PKG"
  else
    sudo apt-get -y remove "$PKG"
  fi
  echo "fatto."
  exit 0
fi

# ── scelta del .deb ────────────────────────────────────────────────────────
if [ -z "$DEB" ]; then
  DEB="$(ls -t dist/${PKG}_*.deb 2>/dev/null | head -1 || true)"
  if [ -z "$DEB" ]; then
    echo "errore: nessun .deb in dist/. Costruiscilo con ./scripts/build-deb.sh" >&2
    exit 1
  fi
fi
if [ ! -f "$DEB" ]; then
  echo "errore: $DEB non esiste" >&2
  exit 1
fi

DEB_ARCH="$(dpkg-deb -f "$DEB" Architecture 2>/dev/null || echo '?')"
DEB_VER="$(dpkg-deb -f "$DEB" Version 2>/dev/null || echo '?')"
HOST_ARCH="$(dpkg --print-architecture 2>/dev/null || uname -m)"

echo "pacchetto: $(basename "$DEB")  ($PKG $DEB_VER, $DEB_ARCH)"
if [ "$DEB_ARCH" != "all" ] && [ "$DEB_ARCH" != "$HOST_ARCH" ]; then
  echo "ATTENZIONE: il pacchetto e' $DEB_ARCH ma questa macchina e' $HOST_ARCH." >&2
  echo "            Potrebbe non essere installabile. Continuo comunque." >&2
fi

# ── scelta della modalita' ─────────────────────────────────────────────────
if [ -z "$MODE" ]; then
  # Default coerente con quello del pacchetto: service su arm, desktop altrove.
  case "$DEB_ARCH" in
  arm64 | armhf) DEF=2 ;;
  *) DEF=1 ;;
  esac
  echo
  echo "Come vuoi installare namzero-lite?"
  echo "  1) desktop  - si apre dal menu applicazioni; config e preset nella tua"
  echo "                home. Adatto al laptop con scheda audio USB."
  echo "  2) service  - parte al boot come servizio, web UI sulla porta 8080,"
  echo "                config condivisa in /etc/namzero. Adatto al Raspberry Pi."
  echo
  printf 'Scelta [%s]: ' "$DEF"
  read -r sel || sel=""
  [ -z "$sel" ] && sel="$DEF"
  case "$sel" in
  1 | desktop) MODE=desktop ;;
  2 | service | servizio) MODE=service ;;
  *)
    echo "scelta non valida: $sel" >&2
    exit 2
    ;;
  esac
fi

case "$MODE" in
desktop | service) ;;
*) echo "errore: modalita' non valida: $MODE (desktop, service)" >&2; exit 2 ;;
esac

echo
echo "== installo in modalita' $MODE"
# DEBIAN_FRONTEND=noninteractive: la domanda l'abbiamo gia' fatta qui e la
# passiamo come NAMZERO_INSTALL_MODE, che il config script di debconf legge
# per prima cosa. Senza noninteractive, apt aprirebbe un secondo dialogo.
sudo sh -c "NAMZERO_INSTALL_MODE='$MODE' DEBIAN_FRONTEND=noninteractive \
  apt-get install -y '$(cd "$(dirname "$DEB")" && pwd)/$(basename "$DEB")'"

echo
case "$MODE" in
desktop)
  echo "installato. Apri 'namzero-lite' dal menu applicazioni."
  echo "config: ~/.config/namzero   dati: ~/.local/share/namzero"
  ;;
*)
  IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
  echo "installato. Web UI: http://${IP:-<ip>}:8080"
  echo "stato servizi: systemctl status namzero-lite namzero-web"
  ;;
esac
echo
echo "per cambiare modalita': sudo dpkg-reconfigure $PKG"
