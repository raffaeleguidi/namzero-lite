#!/usr/bin/env bash
# Costruisce il pacchetto Debian arm64 `namzero-lite`.
#
# Layout prodotto (MVP, senza debhelper):
#   /opt/namzero-lite/      binario, web/, scripts/, ble-bridge-py/, models/, docs/
#                           config -> /etc/namzero, irs -> /var/lib/namzero/irs
#   /etc/namzero/           configurazione condivisa, registrata come conffiles
#   /var/lib/namzero/       dati utente (creata dal postinst)
#   /lib/systemd/system/    unit riscritte per /opt/namzero-lite
#   /usr/local/sbin/        namzero-ctl
#   /etc/default/namzero    conffile con l'utente autorizzato a namzero-ctl
#
# Uso:
#   ./scripts/build-deb.sh                # versione da VERSION
#   ./scripts/build-deb.sh 0.2.0          # versione esplicita
#   VERSION=0.2.0 ./scripts/build-deb.sh
#
# Il binario arm64 deve esistere in build-arm64/; se manca viene chiamato
# scripts/cross-build.sh. Il .deb viene creato da dpkg-deb dentro un container
# debian:bookworm arm64 (nativo su Raspberry Pi/Apple Silicon, qemu su x86_64).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

# Anche `docker run` puo' scrivere nella config di Docker: la si tiene nel
# workspace come fa scripts/cross-build.sh.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$REPO/.docker}"

PKG=namzero-lite
ARCH=arm64
BIN="build-arm64/namzero-lite"
STAGE="$REPO/build-deb/root"
OUTDIR="$REPO/dist"

VERSION="${1:-${VERSION:-}}"
if [ -z "$VERSION" ]; then
  VERSION="$(tr -d '[:space:]' <VERSION)"
fi
VERSION="${VERSION#v}"
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9][A-Za-z0-9.+:~-]*$'; then
  echo "errore: versione Debian non valida: $VERSION" >&2
  exit 2
fi

if [ ! -x "$BIN" ]; then
  echo "== $BIN assente: avvio la cross-build"
  ./scripts/cross-build.sh
fi
if [ ! -x "$BIN" ]; then
  echo "errore: $BIN non eseguibile" >&2
  exit 1
fi

echo "== staging $PKG $VERSION ($ARCH)"
rm -rf "$REPO/build-deb"
mkdir -p \
  "$STAGE/DEBIAN" \
  "$STAGE/opt/$PKG" \
  "$STAGE/etc/namzero/presets" \
  "$STAGE/usr/local/sbin" \
  "$STAGE/etc/default" \
  "$STAGE/lib/systemd/system"

install -m 0755 "$BIN" "$STAGE/opt/$PKG/namzero-lite"

# Web UI e bridge Python: niente cache, niente file di sviluppo.
rsync -a --exclude '.DS_Store' --exclude '__pycache__' \
  web/ "$STAGE/opt/$PKG/web/"
rsync -a --exclude '.DS_Store' --exclude '__pycache__' \
  ble-bridge-py/ "$STAGE/opt/$PKG/ble-bridge-py/"

# Script runtime (quelli di build/deploy restano nel repo, non sul dispositivo).
mkdir -p "$STAGE/opt/$PKG/scripts"
for f in namzero-ctl wifi-connect.sh wifi_manager.py verify_presets.py \
         gboard_led_test.py notify.sh; do
  install -m 0755 "scripts/$f" "$STAGE/opt/$PKG/scripts/$f"
done
install -m 0755 scripts/namzero-ctl "$STAGE/usr/local/sbin/namzero-ctl"

# Modelli NAM di fabbrica: si prendono quelli presenti nel checkout locale
# (non sono in git per dimensione/licenza). Se mancano il pacchetto si crea
# comunque, ma il preset che li cita avvisa e va avanti.
mkdir -p "$STAGE/opt/$PKG/models"
if [ -d models ]; then
  rsync -a --exclude '.DS_Store' models/ "$STAGE/opt/$PKG/models/"
fi
if ! find "$STAGE/opt/$PKG/models" -type f -name '*.nam' | grep -q .; then
  echo "ATTENZIONE: nessun modello .nam in models/: il pacchetto avra' solo i preset" >&2
fi

# Config factory: solo i file richiesti, tutti registrati come conffiles.
cp config/presets.json config/midi.json config/wifi.json config/tone3000.json \
  "$STAGE/etc/namzero/"
cp config/presets/*.json "$STAGE/etc/namzero/presets/"

# Symlink: la config viva sta in /etc, le IR (non ridistribuibili) in /var/lib.
ln -s /etc/namzero "$STAGE/opt/$PKG/config"
ln -s /var/lib/namzero/irs "$STAGE/opt/$PKG/irs"

# Documentazione utile a bordo e referenziata dalle unit.
install -m 0644 README.md "$STAGE/opt/$PKG/README.md"
mkdir -p "$STAGE/opt/$PKG/docs"
for f in ble.md presets.md rt-setup.md web.md midi.md g-board.md; do
  if [ -f "docs/$f" ]; then
    install -m 0644 "docs/$f" "$STAGE/opt/$PKG/docs/$f"
  fi
done

# Wheel opzionali per installazioni offline (cartella non presente di default).
if [ -d packaging/wheels ]; then
  mkdir -p "$STAGE/opt/$PKG/wheels"
  cp -a packaging/wheels/*.whl "$STAGE/opt/$PKG/wheels/" 2>/dev/null || true
fi

# Unit systemd riscritte per /opt/namzero-lite.
cp packaging/systemd/*.service "$STAGE/lib/systemd/system/"

# /etc/default con l'utente di namzero-ctl.
install -m 0644 packaging/namzero.default "$STAGE/etc/default/namzero"

# ── DEBIAN/ ────────────────────────────────────────────────────────────────
install -m 0755 packaging/postinst "$STAGE/DEBIAN/postinst"
install -m 0755 packaging/preinst "$STAGE/DEBIAN/preinst"
install -m 0755 packaging/prerm "$STAGE/DEBIAN/prerm"
install -m 0755 packaging/postrm "$STAGE/DEBIAN/postrm"

INSTALLED_SIZE="$(du -sk "$STAGE" | awk '{print $1}')"
sed -e "s/@VERSION@/$VERSION/g" \
    -e "s/@ARCH@/$ARCH/g" \
    -e "s/@INSTALLED_SIZE@/$INSTALLED_SIZE/g" \
    packaging/control.in >"$STAGE/DEBIAN/control"

{
  echo /etc/default/namzero
  echo /etc/namzero/midi.json
  echo /etc/namzero/presets.json
  echo /etc/namzero/tone3000.json
  echo /etc/namzero/wifi.json
  find "$STAGE/etc/namzero/presets" -maxdepth 1 -type f -name '*.json' \
    | sed "s#^$STAGE##" | sort
} >"$STAGE/DEBIAN/conffiles"

# Permessi uniformi nella staging (alcuni file del repo sono 0600).
find "$STAGE/etc" -type f -exec chmod 0644 {} +
find "$STAGE/etc" -type d -exec chmod 0755 {} +
find "$STAGE/lib" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/opt/$PKG/namzero-lite" "$STAGE/usr/local/sbin/namzero-ctl"
find "$STAGE/opt/$PKG/scripts" -type f -exec chmod 0755 {} +
chmod 0755 "$STAGE/opt/$PKG/ble-bridge-py/run.sh" 2>/dev/null || true
find "$STAGE/opt/$PKG/web" "$STAGE/opt/$PKG/docs" -type f -exec chmod 0644 {} + 2>/dev/null || true
# Il bridge gira come utente namzero: ogni file dell'app deve essere leggibile
# (diversi .py nel repo sono 0600). Questo apre solo la lettura, non la
# scrittura; config e segreti stanno in /etc/namzero e non sono toccati.
find "$STAGE/opt/$PKG" -type f -exec chmod a+r {} +
find "$STAGE/opt/$PKG" -type d -exec chmod a+rx {} +
chmod 0644 "$STAGE/opt/$PKG/README.md"

# Coerenza: ogni conffile deve esistere nello staging.
while IFS= read -r c; do
  [ -f "$STAGE$c" ] || { echo "errore: conffile mancante $c" >&2; exit 1; }
done <"$STAGE/DEBIAN/conffiles"

mkdir -p "$OUTDIR"
DEB="$OUTDIR/${PKG}_${VERSION}_${ARCH}.deb"
rm -f "$DEB"

echo "== dpkg-deb in container debian:bookworm $ARCH"
docker run --rm --platform "linux/$ARCH" \
  -v "$REPO":/src \
  -w /src \
  debian:bookworm \
  dpkg-deb --root-owner-group --build build-deb/root "dist/${PKG}_${VERSION}_${ARCH}.deb"

# Controlli minimi sul pacchetto creato.
docker run --rm --platform "linux/$ARCH" \
  -v "$REPO/dist":/dist:ro \
  debian:bookworm \
  sh -ec "
    dpkg-deb --info /dist/${PKG}_${VERSION}_${ARCH}.deb
    echo '--- conffiles ---'
    dpkg-deb --ctrl-tarfile /dist/${PKG}_${VERSION}_${ARCH}.deb | tar -xO ./conffiles
    echo '--- layout (estratto) ---'
    dpkg-deb --contents /dist/${PKG}_${VERSION}_${ARCH}.deb | sed -n '1,25p'
  "

echo
echo "pronto: $DEB"
echo "installa con: sudo apt install $DEB"
