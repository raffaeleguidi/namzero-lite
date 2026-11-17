#!/usr/bin/env bash
# Backup immagine di una scheda SD su macOS.
#
# Uso:
#   ./scripts/backup_sd.sh [--raw] [--speed] [disk] [cartella]
#
#   --raw     immagine non compressa (.img): più veloce, occupa quanto la card
#   --speed   misura solo la velocità di lettura della card (1 GiB) e esce
#
# Produce <cartella>/sdcard-AAAA-MM-GG.img[.gz] + relativo .sha256.
set -euo pipefail

die() { echo "errore: $*" >&2; exit 1; }

RAW_MODE=0
SPEED_ONLY=0
POSITIONAL=()

for arg in "$@"; do
  case "$arg" in
    --raw)   RAW_MODE=1 ;;
    --speed) SPEED_ONLY=1 ;;
    -h|--help)
      sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    -*) die "opzione sconosciuta: $arg" ;;
    *)  POSITIONAL+=("$arg") ;;
  esac
done

DISK="${POSITIONAL[0]:-disk2}"
OUT_DIR="${POSITIONAL[1]:-$HOME}"
STAMP="$(date +%Y-%m-%d)"
DEV="/dev/$DISK"
RAW="/dev/r$DISK"
BS=8m

# --- controlli preliminari -------------------------------------------------

command -v diskutil >/dev/null || die "diskutil non trovato: serve macOS"
[[ -e "$DEV" ]] || die "$DEV non esiste. Controlla con: diskutil list"

INFO="$(diskutil info "$DEV")"
echo "$INFO" | grep -Eq "Removable Media: *(Yes|Removable)" || \
  die "$DEV non è un'unità rimovibile: mi rifiuto di continuare"
echo "$INFO" | grep -q "Protocol: *USB" || \
  die "$DEV non è su USB: mi rifiuto di continuare"

SIZE_BYTES="$(echo "$INFO" | awk -F'[()]' '/Disk Size/ {gsub(/[^0-9]/,"",$2); print $2}')"
[[ -n "$SIZE_BYTES" ]] || die "impossibile leggere la dimensione di $DEV"
SIZE_GB=$(( SIZE_BYTES / 1000000000 ))
SIZE_GIB=$(( SIZE_BYTES / 1073741824 ))

echo "disco:    $DEV (${SIZE_GB} GB)"

# --- solo misura di velocità ----------------------------------------------

if [[ "$SPEED_ONLY" -eq 1 ]]; then
  echo "misuro 1 GiB in lettura (serve sudo)..."
  diskutil unmountDisk "$DEV" >/dev/null 2>&1 || true
  start=$(date +%s)
  sudo dd if="$RAW" of=/dev/null bs=$BS count=128
  end=$(date +%s)
  secs=$(( end - start ))
  [[ "$secs" -gt 0 ]] || secs=1
  mbs=$(( 1024 / secs ))
  echo
  echo "velocità: circa ${mbs} MB/s"
  if   [[ "$mbs" -ge 20 ]]; then echo "ok: card e lettore adeguati"
  elif [[ "$mbs" -ge 10 ]]; then echo "lenta ma usabile: il backup richiederà ~$(( SIZE_GIB * 1024 / mbs / 60 )) minuti"
  else                          echo "molto lenta: problema di card o lettore (vedi note)"
  fi
  exit 0
fi

# --- spazio e destinazione -------------------------------------------------

if [[ "$RAW_MODE" -eq 1 ]]; then
  OUT="$OUT_DIR/sdcard-$STAMP.img"
else
  OUT="$OUT_DIR/sdcard-$STAMP.img.gz"
fi

mkdir -p "$OUT_DIR"
FREE_KB="$(df -k "$OUT_DIR" | awk 'NR==2 {print $4}')"
FREE_GIB=$(( FREE_KB / 1024 / 1024 ))
echo "spazio:   ${FREE_GIB} GiB liberi in $OUT_DIR"
echo "immagine: $OUT"

if [[ -e "$OUT" ]]; then
  die "$OUT esiste già: rinominalo o spostalo prima di procedere"
fi

NEED_GIB=$SIZE_GIB
[[ "$RAW_MODE" -eq 0 ]] && NEED_GIB=$(( SIZE_GIB / 2 ))
if [[ "$FREE_GIB" -lt "$NEED_GIB" ]]; then
  die "spazio insufficiente: servono almeno ${NEED_GIB} GiB, liberi ${FREE_GIB}"
fi

echo

# --- conferma esplicita ---------------------------------------------------

read -r -p "Procedo con il backup di $DEV? [s/N] " REPLY
[[ "$REPLY" =~ ^[sSyY]$ ]] || { echo "annullato"; exit 0; }

# --- backup ---------------------------------------------------------------

echo
echo "smonto $DEV (senza espellerlo)..."
diskutil unmountDisk "$DEV"

echo "copia in corso. Per vedere l'avanzamento, in un altro terminale:"
echo "   sudo kill -INFO \$(pgrep -x dd)"
echo

start=$(date +%s)
if [[ "$RAW_MODE" -eq 1 ]]; then
  sudo dd if="$RAW" of="$OUT" bs=$BS
else
  sudo dd if="$RAW" bs=$BS | gzip -1 > "$OUT"
  gzip -t "$OUT" || die "archivio corrotto, rifai il backup"
fi
end=$(date +%s)
secs=$(( end - start ))
[[ "$secs" -gt 0 ]] || secs=1

echo
SIZE_IMG="$(du -h "$OUT" | awk '{print $1}')"
echo "ok: $OUT ($SIZE_IMG, ${secs}s, ~$(( SIZE_GIB * 1024 / secs )) MB/s)"

shasum -a 256 "$OUT" | tee "$OUT.sha256"

echo
echo "per ripristinare:"
if [[ "$RAW_MODE" -eq 1 ]]; then
  echo "  sudo dd if=\"$OUT\" of=$RAW bs=$BS"
else
  echo "  gzip -dc \"$OUT\" | sudo dd of=$RAW bs=$BS"
fi
