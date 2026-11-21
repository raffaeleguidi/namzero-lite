#!/usr/bin/env bash
# Collega il Pi alla prima rete conosciuta raggiungibile (config/wifi.json),
# per priorita' decrescente. Gira come servizio; l'AP non e' gestito qui.
set -u
cd "$(dirname "$0")/.."
while true; do
  python3 - <<'PY'
import json, subprocess
try:
    nets = json.load(open("config/wifi.json"))["networks"]
except Exception:
    nets = []
if not nets:
    raise SystemExit
out = subprocess.run(["nmcli", "-t", "-f", "SSID", "device", "wifi", "list"],
                     capture_output=True, text=True).stdout
seen = {line.strip() for line in out.splitlines()}
for n in sorted(nets, key=lambda x: -int(x.get("priority", 0))):
    ssid = n.get("ssid", "")
    if not ssid or ssid not in seen:
        continue
    subprocess.run(["nmcli", "connection", "add", "type", "wifi", "con-name", ssid,
                    "ifname", "wlan0", "ssid", ssid, "wifi-sec.key-mgmt", "wpa-psk",
                    "wifi-sec.psk", n.get("password", "")], capture_output=True)
    r = subprocess.run(["nmcli", "connection", "up", ssid], capture_output=True)
    print("wifi:", ssid, "->", "ok" if r.returncode == 0 else "fallita", flush=True)
    break
PY
  sleep 30
done
