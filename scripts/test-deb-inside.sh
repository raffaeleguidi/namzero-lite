#!/usr/bin/env bash
# Check eseguiti DENTRO il container arm64 di test (systemd come PID 1).
# Non si lancia a mano sull'host: lo invoca scripts/test-container.sh.
#
# Uso: test-deb-inside.sh <base.deb> <upgrade.deb>
set -uo pipefail

BASE_DEB="/pkg/$1"
UPGRADE_DEB="/pkg/$2"
FAILED=0

say() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ok() { printf '  OK   %s\n' "$*"; }
bad() {
  printf '  FAIL %s\n' "$*"
  FAILED=$((FAILED + 1))
}
check() {
  # check <descrizione> <comando...>
  local desc="$1"
  shift
  if "$@"; then ok "$desc"; else bad "$desc"; fi
}
check_eq() {
  # check_eq <descrizione> <atteso> <comando...>
  local desc="$1" want="$2"
  shift 2
  local got
  got="$("$@" 2>/dev/null || true)"
  if [ "$got" = "$want" ]; then
    ok "$desc ($got)"
  else
    bad "$desc: atteso '$want', ottenuto '$got'"
  fi
}
wait_active() {
  # wait_active <unit> <secondi>
  local unit="$1" secs="${2:-90}" i=0
  while [ "$i" -lt "$secs" ]; do
    if systemctl is-active --quiet "$unit"; then
      return 0
    fi
    i=$((i + 1))
    sleep 1
  done
  return 1
}
json_check() {
  # json_check <descrizione> <python-expr> ; il JSON e' in stdin
  local desc="$1" expr="$2"
  if python3 -c "
import json, sys
st = json.load(sys.stdin)
assert $expr, st
" ; then
    ok "$desc"
  else
    bad "$desc"
  fi
}
dump_logs() {
  echo
  echo '--- journal (motore/web/bridge/wifi) ---'
  journalctl -u namzero-lite -u namzero-web -u namzero-ble-bridge \
      -u namzero-wifi --no-pager -n 120 2>&1 || true
  echo '--- stato unit ---'
  systemctl status namzero-lite namzero-web namzero-ble-bridge \
      namzero-wifi --no-pager -l 2>&1 || true
}

say "installazione del pacchetto base"
export DEBIAN_FRONTEND=noninteractive
if ! apt-get update -qq; then
  bad "apt-get update"
fi
if ! apt-get install -y "$BASE_DEB"; then
  bad "apt-get install $BASE_DEB"
  dump_logs
  exit 1
fi
ok "apt install ./base.deb"

say "versione e layout"
BASE_VERSION="$(dpkg-query -W -f '${Version}' namzero-lite)"
echo "  versione installata: $BASE_VERSION"
check "binario in /opt/namzero-lite" test -x /opt/namzero-lite/namzero-lite
check "web/server.py in /opt" test -f /opt/namzero-lite/web/server.py
check "ble-bridge-py in /opt" test -f /opt/namzero-lite/ble-bridge-py/main.py
check "venv del bridge" test -x /opt/namzero-lite/venv/bin/python
check "namzero-ctl" test -x /usr/local/sbin/namzero-ctl
check "symlink config -> /etc/namzero" \
  test "$(readlink -f /opt/namzero-lite/config)" = /etc/namzero
check "symlink irs -> /var/lib/namzero/irs" \
  test "$(readlink -f /opt/namzero-lite/irs)" = /var/lib/namzero/irs
check "dati utente /var/lib/namzero" test -d /var/lib/namzero
check "config factory /etc/namzero" test -f /etc/namzero/presets.json
check "preset di default (>= 8)" \
  test "$(find /etc/namzero/presets -name '*.json' | wc -l)" -ge 8
check "modello NAM di fabbrica" \
  test "$(find /opt/namzero-lite/models -name '*.nam' | wc -l)" -ge 1
check "sudoers" test -f /etc/sudoers.d/namzero
check "drop-in dnsmasq" test -f /etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf
check "unit lite in /lib/systemd/system" test -f /lib/systemd/system/namzero-lite.service
check "unit web in /lib/systemd/system" test -f /lib/systemd/system/namzero-web.service
check "unit bridge in /lib/systemd/system" test -f /lib/systemd/system/namzero-ble-bridge.service
check "unit wifi in /lib/systemd/system" test -f /lib/systemd/system/namzero-wifi.service
check "conffiles registrati" sh -c \
  "dpkg-query -W -f '\${Conffiles}' namzero-lite | grep -q '/etc/namzero/presets.json'"
check "file del bridge leggibili da namzero" \
  su -s /bin/sh namzero -c \
  'test -r /opt/namzero-lite/ble-bridge-py/ble_midi.py && test -r /opt/namzero-lite/ble-bridge-py/midi_socket.py && test -r /opt/namzero-lite/ble-bridge-py/main.py'

say "servizi"
wait_active namzero-lite 90 && ok "namzero-lite attivo" || bad "namzero-lite non attivo"
wait_active namzero-web 90 && ok "namzero-web attivo" || bad "namzero-web non attivo"
wait_active namzero-ble-bridge 90 && ok "namzero-ble-bridge attivo" || bad "namzero-ble-bridge non attivo"
wait_active cpu-performance 30 && ok "cpu-performance attivo" || bad "cpu-performance non attivo"
if journalctl -u namzero-ble-bridge --no-pager 2>/dev/null |
    grep -qE 'ModuleNotFoundError|ImportError|Permission denied'; then
  bad "il bridge ha errori di import/permessi nel journal"
else
  ok "bridge senza errori di import/permessi"
fi
# Il WiFi non e' testabile in container (niente wlan0): attivo o inattivo va
# bene, ma non deve essere in failed.
WIFI_STATE="$(systemctl is-active namzero-wifi 2>/dev/null || true)"
case "$WIFI_STATE" in
active | activating | inactive)
  ok "namzero-wifi: $WIFI_STATE (hardware assente, accettato)"
  ;;
*)
  bad "namzero-wifi: $WIFI_STATE"
  ;;
esac

say "web UI e API"
HTTP_CODE="$(curl -s -o /tmp/index.html -w '%{http_code}' http://127.0.0.1:8080/ || true)"
check_eq "GET / -> 200" "200" printf '%s' "$HTTP_CODE"
check "GET / contiene il titolo" grep -qi 'namzero' /tmp/index.html

STATE=""
for _ in $(seq 1 60); do
  STATE="$(curl -sS http://127.0.0.1:8080/api/state 2>/dev/null || true)"
  if printf '%s' "$STATE" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
    break
  fi
  sleep 1
done
json_check "GET /api/state JSON valido" 'isinstance(st, dict)' <<<"$STATE"
json_check "audio.open == false senza /dev/snd" \
  'st.get("audio", {}).get("open") is False' <<<"$STATE"
json_check "audio.error spiega il motivo" \
  'bool(st.get("audio", {}).get("error"))' <<<"$STATE"
json_check "preset/stato motore presente" \
  'isinstance(st.get("slots"), dict)' <<<"$STATE"

say "namzero-ctl senza password (utente pi)"
if su - pi -c 'sudo -n /usr/local/sbin/namzero-ctl is-active lite' >/tmp/ctl.out 2>/tmp/ctl.err; then
  if grep -qx 'active' /tmp/ctl.out; then
    ok "sudo -n namzero-ctl is-active lite -> active"
  else
    bad "namzero-ctl ha risposto: $(cat /tmp/ctl.out)"
  fi
else
  bad "sudo -n namzero-ctl fallito: $(cat /tmp/ctl.err)"
fi
check "namzero-ctl via PATH" \
  su - pi -c 'sudo -n namzero-ctl is-active web'

say "upgrade e conffiles"
# Marker nei file di config: l'upgrade deve conservarli (conffiles).
python3 - <<'PY'
import json
for path in ("/etc/namzero/presets.json",
             "/etc/namzero/presets/vox_clean.json"):
    with open(path) as f:
        data = json.load(f)
    data["_installer_test"] = "keep-me"
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
PY
if ! apt-get install -y -o Dpkg::Options::=--force-confold "$UPGRADE_DEB"; then
  bad "apt install upgrade"
  dump_logs
else
  ok "apt install ./upgrade.deb"
fi
NEW_VERSION="$(dpkg-query -W -f '${Version}' namzero-lite 2>/dev/null || true)"
if [ "$NEW_VERSION" != "$BASE_VERSION" ] && [ -n "$NEW_VERSION" ]; then
  ok "versione aggiornata: $BASE_VERSION -> $NEW_VERSION"
else
  bad "versione non aggiornata ($NEW_VERSION)"
fi
check "presets.json conservato dopo upgrade" sh -c \
  "python3 -c 'import json; d=json.load(open(\"/etc/namzero/presets.json\")); assert d[\"_installer_test\"]==\"keep-me\"'"
check "preset slot conservato dopo upgrade" sh -c \
  "python3 -c 'import json; d=json.load(open(\"/etc/namzero/presets/vox_clean.json\")); assert d[\"_installer_test\"]==\"keep-me\"'"
wait_active namzero-lite 60 && ok "motore riavviato dall'upgrade" || bad "motore non attivo dopo upgrade"
wait_active namzero-web 60 && ok "web riavviata dall'upgrade" || bad "web non attiva dopo upgrade"

say "remove: config e dati utente restano"
if ! apt-get remove -y namzero-lite; then
  bad "apt remove"
else
  ok "apt remove"
fi
check "/var/lib/namzero resta su remove" test -d /var/lib/namzero
check "/etc/namzero resta su remove" test -f /etc/namzero/presets.json
check "marker conffile resta su remove" sh -c \
  "python3 -c 'import json; d=json.load(open(\"/etc/namzero/presets.json\")); assert d[\"_installer_test\"]==\"keep-me\"'"

say "purge: pulizia completa"
if ! apt-get purge -y namzero-lite; then
  bad "apt purge"
  dump_logs
else
  ok "apt purge"
fi
check "/var/lib/namzero rimosso" test ! -e /var/lib/namzero
check "/etc/namzero rimosso" test ! -e /etc/namzero
check "sudoers rimosso" test ! -e /etc/sudoers.d/namzero
check "drop-in dnsmasq rimosso" test ! -e /etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf
check "/opt/namzero-lite rimosso" test ! -e /opt/namzero-lite
check "namzero-ctl rimosso" test ! -e /usr/local/sbin/namzero-ctl
check "unit lite rimossa" test ! -e /lib/systemd/system/namzero-lite.service
check "pacchetto non installato" sh -c '! dpkg-query -W namzero-lite >/dev/null 2>&1'

echo
if [ "$FAILED" -ne 0 ]; then
  echo "RISULTATO: $FAILED check falliti"
  dump_logs
  exit 1
fi
echo "RISULTATO: tutti i check superati"
