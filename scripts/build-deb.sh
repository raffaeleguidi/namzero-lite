#!/usr/bin/env bash
# Costruisce il pacchetto Debian `namzero-lite`.
#
# Layout prodotto:
#   /opt/namzero-lite/      binario, web/, scripts/, ble-bridge-py/, models/, docs/
#                           config -> /etc/namzero, irs -> /var/lib/namzero/irs
#   /etc/namzero/           configurazione factory, registrata come conffiles
#   /var/lib/namzero/       dati e stato (creata dal postinst)
#   /lib/systemd/system/    unit riscritte per /opt/namzero-lite
#   /usr/local/sbin/        namzero-ctl
#   /usr/bin/               namzero-lite-gui (launcher WebKitGTK)
#   /usr/share/applications/ + icons/hicolor/scalable/apps/
#   /etc/default/namzero    conffile con l'utente autorizzato a namzero-ctl
#   DEBIAN/templates+config debconf: modalita' desktop o service
#
# Uso:
#   ./scripts/build-deb.sh                       # arch dell'host (amd64 sul laptop)
#   ./scripts/build-deb.sh 0.2.0                 # versione esplicita
#   ARCH=arm64 ./scripts/build-deb.sh            # .deb per il Pi
#   ARCH=arm64 NAM_DIR=~/nam ./scripts/build-deb.sh
#
# Il binario viene compilato in un container della stessa architettura del
# pacchetto: amd64 su ubuntu:24.04 (nativo su un host x86_64), arm64 su
# debian:bookworm (nativo su Apple Silicon, qemu altrove). Per arm64, se
# build-arm64/namzero-lite esiste gia' e non si chiede una ricompilazione, si
# riusa (e' quello prodotto da scripts/cross-build.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO="$PWD"

# Anche `docker run` puo' scrivere nella config di Docker: la si tiene nel
# workspace come fa scripts/cross-build.sh.
export DOCKER_CONFIG="${DOCKER_CONFIG:-$REPO/.docker}"

PKG=namzero-lite
ARCH="${ARCH:-amd64}"
STAGE="$REPO/build-deb/root"
OUTDIR="$REPO/dist"

# Immagine e directory di build per architettura. La base segue la
# distribuzione di destinazione: Ubuntu 24.04 per il laptop, Debian bookworm
# per Raspberry Pi OS.
case "$ARCH" in
amd64)
  BASE_IMAGE="ubuntu:24.04"
  BUILD_DIR="build-amd64"
  # Su Ubuntu 24.04 ALSA si chiama libasound2-dev, come su Debian.
  BUILD_DEPS="build-essential cmake libasound2-dev"
  ;;
arm64)
  BASE_IMAGE="debian:bookworm"
  BUILD_DIR="build-arm64"
  BUILD_DEPS="build-essential cmake libasound2-dev"
  ;;
*)
  echo "errore: architettura non supportata: $ARCH (amd64, arm64)" >&2
  exit 2
  ;;
esac

BIN="$BUILD_DIR/namzero-lite"

VERSION="${1:-${VERSION:-}}"
if [ -z "$VERSION" ]; then
  VERSION="$(tr -d '[:space:]' <VERSION)"
fi
VERSION="${VERSION#v}"
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9][A-Za-z0-9.+:~-]*$'; then
  echo "errore: versione Debian non valida: $VERSION" >&2
  exit 2
fi

# ── binario ────────────────────────────────────────────────────────────────
# SKIP_BUILD=1 usa quello che c'e' gia' in $BUILD_DIR (utile per ripacchettare
# senza ricompilare). Su arm64, se manca, si passa dal cross-build storico.
if [ "${SKIP_BUILD:-0}" = "1" ] && [ -x "$BIN" ]; then
  echo "== SKIP_BUILD=1: uso $BIN"
elif [ -x "$BIN" ] && [ "${FORCE_BUILD:-0}" != "1" ]; then
  echo "== $BIN gia' presente (FORCE_BUILD=1 per ricompilare)"
elif [ "$ARCH" = "arm64" ] && [ ! -x "$BIN" ]; then
  echo "== $BIN assente: avvio la cross-build"
  ./scripts/cross-build.sh
else
  echo "== compilo il motore in container $BASE_IMAGE ($ARCH)"
  # NAM_DIR opzionale: se c'e' un checkout del core NAM lo si monta in /nam,
  # cosi' lo slot ampli resta abilitato anche nel pacchetto.
  NAM_MOUNT=()
  NAM_ARG=()
  if [ -n "${NAM_DIR:-}" ] && [ -d "${NAM_DIR}" ]; then
    NAM_MOUNT=(-v "$(cd "$NAM_DIR" && pwd):/nam:ro")
    NAM_ARG=(-DNAM_DIR=/nam)
  elif [ -d "$REPO/.nam" ]; then
    NAM_MOUNT=(-v "$REPO/.nam:/nam:ro")
    NAM_ARG=(-DNAM_DIR=/nam)
  fi
  docker run --rm --platform "linux/$ARCH" \
    -v "$REPO":/src \
    "${NAM_MOUNT[@]}" \
    -w /src \
    "$BASE_IMAGE" bash -ec "
      export DEBIAN_FRONTEND=noninteractive
      apt-get update -qq >/dev/null
      apt-get install -y -qq $BUILD_DEPS >/dev/null
      cmake -S . -B $BUILD_DIR -DCMAKE_BUILD_TYPE=Release ${NAM_ARG[*]} >/tmp/cfg.log 2>&1 \
        || { tail -30 /tmp/cfg.log; exit 1; }
      cmake --build $BUILD_DIR -j\"\$(nproc)\" >/tmp/build.log 2>&1 \
        || { tail -40 /tmp/build.log; exit 1; }
      echo '   build ok'
      # I test girano solo se possono farlo (niente ALSA reale richiesta:
      # la suite e' di sola DSP). Un fallimento qui ferma il pacchetto.
      (cd $BUILD_DIR && ctest --output-on-failure >/tmp/ctest.log 2>&1) \
        || { tail -40 /tmp/ctest.log; exit 1; }
      echo '   ctest ok'
    "
fi

if [ ! -x "$BIN" ]; then
  echo "errore: $BIN non eseguibile" >&2
  exit 1
fi

# ── staging ────────────────────────────────────────────────────────────────
echo "== staging $PKG $VERSION ($ARCH)"
rm -rf "$REPO/build-deb"
mkdir -p \
  "$STAGE/DEBIAN" \
  "$STAGE/opt/$PKG" \
  "$STAGE/etc/namzero/presets" \
  "$STAGE/usr/local/sbin" \
  "$STAGE/usr/bin" \
  "$STAGE/usr/share/applications" \
  "$STAGE/usr/share/icons/hicolor/scalable/apps" \
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

# ── launcher desktop ───────────────────────────────────────────────────────
# La GUI sta in /usr/bin e parla con il motore via la socket di controllo;
# il .desktop e l'icona la fanno comparire nel menu applicazioni.
if [ -f packaging/launcher/namzero-lite-gui.py ]; then
  install -m 0755 packaging/launcher/namzero-lite-gui.py \
    "$STAGE/usr/bin/namzero-lite-gui"
else
  echo "ATTENZIONE: packaging/launcher/namzero-lite-gui.py assente: niente GUI" >&2
fi
if [ -f packaging/desktop/namzero-lite.desktop ]; then
  install -m 0644 packaging/desktop/namzero-lite.desktop \
    "$STAGE/usr/share/applications/namzero-lite.desktop"
else
  echo "ATTENZIONE: packaging/desktop/namzero-lite.desktop assente" >&2
fi
if [ -f packaging/desktop/namzero-lite.svg ]; then
  install -m 0644 packaging/desktop/namzero-lite.svg \
    "$STAGE/usr/share/icons/hicolor/scalable/apps/namzero-lite.svg"
else
  echo "ATTENZIONE: packaging/desktop/namzero-lite.svg assente" >&2
fi

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

# Symlink: la config factory sta in /etc, le IR (non ridistribuibili) in
# /var/lib. In modalita' desktop il launcher copia /etc/namzero nella home;
# questi symlink servono alla modalita' servizio.
ln -s /etc/namzero "$STAGE/opt/$PKG/config"
ln -s /var/lib/namzero/irs "$STAGE/opt/$PKG/irs"

# Documentazione utile a bordo e referenziata dalle unit.
install -m 0644 README.md "$STAGE/opt/$PKG/README.md"
mkdir -p "$STAGE/opt/$PKG/docs"
for f in ble.md presets.md rt-setup.md web.md midi.md g-board.md installer.md; do
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
# Nota sul campo Depends in control.in: libasound2 esiste su Debian bookworm
# (Raspberry Pi OS) ma su Ubuntu 24.04 e' stato rinominato libasound2t64,
# quindi si dichiara l'alternativa. GTK/WebKit (desktop) e bluez/NetworkManager
# (servizio) stanno nei Recommends: metterli in Depends obbligherebbe il laptop
# a installare dnsmasq e il Pi a installare WebKit. I file control non
# ammettono commenti, per questo la spiegazione sta qui.
install -m 0755 packaging/postinst "$STAGE/DEBIAN/postinst"
install -m 0755 packaging/preinst "$STAGE/DEBIAN/preinst"
install -m 0755 packaging/prerm "$STAGE/DEBIAN/prerm"
install -m 0755 packaging/postrm "$STAGE/DEBIAN/postrm"
# debconf: templates + config. Il config decide la modalita' (e accetta
# NAMZERO_INSTALL_MODE dall'ambiente, che e' il gancio per install.sh).
install -m 0644 packaging/debconf/namzero-lite.templates "$STAGE/DEBIAN/templates"
install -m 0755 packaging/debconf/namzero-lite.config "$STAGE/DEBIAN/config"

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

echo "== dpkg-deb in container $BASE_IMAGE ($ARCH)"
docker run --rm --platform "linux/$ARCH" \
  -v "$REPO":/src \
  -w /src \
  "$BASE_IMAGE" \
  dpkg-deb --root-owner-group --build build-deb/root "dist/${PKG}_${VERSION}_${ARCH}.deb"

# Controlli minimi sul pacchetto creato.
docker run --rm --platform "linux/$ARCH" \
  -v "$REPO/dist":/dist:ro \
  "$BASE_IMAGE" \
  sh -ec "
    dpkg-deb --info /dist/${PKG}_${VERSION}_${ARCH}.deb
    echo '--- conffiles ---'
    dpkg-deb --ctrl-tarfile /dist/${PKG}_${VERSION}_${ARCH}.deb | tar -xO ./conffiles
    echo '--- templates (domanda debconf) ---'
    dpkg-deb --ctrl-tarfile /dist/${PKG}_${VERSION}_${ARCH}.deb | tar -xO ./templates | head -8
    echo '--- layout (estratto) ---'
    dpkg-deb --contents /dist/${PKG}_${VERSION}_${ARCH}.deb | sed -n '1,30p'
  "

echo
echo "pronto: $DEB"
echo "installa con: sudo apt install ./$(basename "$DEB")"
echo "modalita' desktop:  sudo NAMZERO_INSTALL_MODE=desktop apt install ./$(basename "$DEB")"
echo "modalita' servizio: sudo NAMZERO_INSTALL_MODE=service apt install ./$(basename "$DEB")"
echo "cambia dopo:        sudo dpkg-reconfigure $PKG"
