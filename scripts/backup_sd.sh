#!/usr/bin/env bash
# Backup immagine di una scheda SD su macOS.
#
# Uso:  ./scripts/backup_sd.sh disk2 [cartella_destinazione]
#
# Produce <cartella>/sdcard-AAAA-MM-GG.img.gz + relativo .sha256.
# Non esegue né lo script né dd come root: chiede sudo solo per leggere il
# dispositivo grezzo.
set -euo pipefail

DISK="${1:-disk2}"
OUT_DIR="${2:-$HOME}"
STAMP="$(date +%Y-%m-%d)"
OUT="$OUT_DIR/sdcard-$STAMP.img.gz"
DEV="/dev/$DISK"
RAW="/dev/r$DISK"

die() { echo "errore: $*" >&2; exit 1; }

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
echo "disco:      $DEV (${SIZE_GB} GB, rimovibile USB)"

mkdir -p "$OUT_DIR"
FREE_KB="$(df -k "$OUT_DIR" | awk 'NR==2 {print $4}')"
FREE_GB=$(( FREE_KB / 1024 / 1024 ))
echo "spazio:     ${FREE_GB} GB liberi in $OUT_DIR"
echo "immagine:   $OUT (compressa)"

if [[ -e "$OUT" ]]; then
  die "$OUT esiste già: rinominalo o spostalo prima di procedere"
fi

# --- conferma esplicita ---------------------------------------------------

echo
read -r -p "Procedo con il backup di $DEV? [s/N] " REPLY
[[ "$REPLY" =~ ^[sSyY]$ ]] || { echo "annullato"; exit 0; }

# --- backup ---------------------------------------------------------------

echo
echo "smonto $DEV (senza espellerlo)..."
diskutil unmountDisk "$DEV"

echo "lettura e compressione in corso (può durare 15-25 minuti)..."
start=$(date +%s)
sudo dd if="$RAW" bs=4m 2>/dev/null | gzip -1 > "$OUT"
end=$(date +%s)

echo
echo "verifica dell'archivio..."
gzip -t "$OUT" || die "archivio corrotto, rifai il backup"

SIZE_IMG="$(du -h "$OUT" | awk '{print $1}')"
echo "ok: $OUT ($SIZE_IMG, $(( end - start )) secondi)"

shasum -a 256 "$OUT" | tee "$OUT.sha256"

echo
echo "per ripristinare:"
echo "  gzip -dc \"$OUT\" | sudo dd of=$RAW bs=4m"
