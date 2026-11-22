#!/usr/bin/env python3
"""WiFi del Pi: reti note + access point di fallback.

Regole:
  - una sola radio: AP *oppure* STA, mai entrambe;
  - all'avvio si prova a collegare una rete nota per ~10 s; se non c'e' e
    l'AP e' abilitato, si avvia l'hotspot condiviso (DHCP di NetworkManager);
  - in AP si scansiona ogni ~50 s: appena compare una rete nota si chiude
    l'AP e si torna in STA; se il collegamento STA si perde e non ci sono
    reti note, si torna all'AP dopo due controlli (isteresi).
  - --dry-run non esegue mai comandi che modificano NetworkManager;
  - --test esegue i test delle decisioni senza toccare il sistema.

I log hanno il tag [wifi] e finiscono nel journal del servizio:
    journalctl -u namzero-wifi -f
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from datetime import datetime
from typing import Any, Callable, Dict, List, Optional, Sequence, Tuple

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG = os.path.join(ROOT, "config/wifi.json")
IFACE = "wlan0"
AP_CON = "namzero-lite-ap"
DEFAULT_AP = {"enabled": True, "ssid": "namzero-lite", "password": "namzero-lite"}
WEB_PORT = 8080

STARTUP_WINDOW = 10.0          # secondi per trovare una rete nota all'avvio
STA_CHECK_INTERVAL = 30.0      # controllo dello stato STA
AP_SCAN_INTERVAL = 50.0        # in AP: ogni quanto cercare reti note
NO_LINK_INTERVAL = 15.0        # senza link: ogni quanto ritentare
STA_FAILURES_BEFORE_AP = 2     # isteresi prima di passare da STA persa ad AP
AP_VERIFY_TIMEOUT = 20.0

STOP = False
VERBOSE = False


# ── utilita' ────────────────────────────────────────────────────────────────

def log(msg: str, *args: Any) -> None:
    if args:
        msg = msg % args
    print("[wifi] %s %s" % (datetime.now().strftime("%H:%M:%S"), msg), flush=True)


def as_bool(value: Any, default: bool = False) -> bool:
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on", "si", "sì")
    return default


def first_line(value: str) -> str:
    return value.strip().splitlines()[0].strip() if value and value.strip() else ""


def split_terse(line: str) -> List[str]:
    """Divide una riga di `nmcli -t` rispettando i ':' e '\\' escapati."""
    fields: List[str] = []
    cur: List[str] = []
    i = 0
    while i < len(line):
        ch = line[i]
        if ch == "\\" and i + 1 < len(line):
            cur.append(line[i + 1])
            i += 2
            continue
        if ch == ":":
            fields.append("".join(cur))
            cur = []
            i += 1
            continue
        cur.append(ch)
        i += 1
    fields.append("".join(cur))
    return fields


def redact_cmd(cmd: Sequence[str]) -> str:
    """Comando leggibile senza password nei log."""
    out: List[str] = []
    secret_next = False
    for arg in cmd:
        if secret_next:
            out.append("***")
            secret_next = False
            continue
        out.append(arg)
        if arg in ("password", "wifi-sec.psk"):
            secret_next = True
    return shlex.join(out)


def sleep_s(seconds: float) -> None:
    end = time.time() + max(0.0, seconds)
    while not STOP and time.time() < end:
        time.sleep(min(1.0, max(0.0, end - time.time())))


def wait_for(fn: Callable[[], Optional[Any]], timeout: float,
             interval: float = 0.5) -> Optional[Any]:
    end = time.time() + timeout
    while time.time() < end:
        value = fn()
        if value:
            return value
        if STOP:
            break
        time.sleep(interval)
    return fn()


def wait_status(predicate: Callable[[Dict[str, str]], bool],
                timeout: float) -> Optional[Dict[str, str]]:
    def check() -> Optional[Dict[str, str]]:
        status = device_status()
        return status if predicate(status) else None

    return wait_for(check, timeout)


# ── config ──────────────────────────────────────────────────────────────────

def load_config(path: str) -> Optional[Dict[str, Any]]:
    try:
        with open(path) as f:
            raw = json.load(f)
    except Exception as exc:  # file mancante, JSON rotto, permessi...
        log("config %s non leggibile: %s", path, exc)
        return None
    if not isinstance(raw, dict):
        log("config %s: atteso un oggetto JSON", path)
        return None

    networks: List[Dict[str, Any]] = []
    for item in raw.get("networks") or []:
        if not isinstance(item, dict):
            continue
        ssid = str(item.get("ssid", "")).strip()
        if not ssid:
            continue
        try:
            priority = int(item.get("priority", 0))
        except (TypeError, ValueError):
            priority = 0
        networks.append({"ssid": ssid,
                         "password": str(item.get("password", "")),
                         "priority": priority})

    ap = dict(DEFAULT_AP)
    raw_ap = raw.get("ap")
    if isinstance(raw_ap, dict):
        ap["enabled"] = as_bool(raw_ap.get("enabled", ap["enabled"]), ap["enabled"])
        ap["ssid"] = str(raw_ap.get("ssid", ap["ssid"])).strip() or ap["ssid"]
        ap["password"] = str(raw_ap.get("password", ap["password"]))
    elif raw_ap is not None:
        log("config: blocco ap non valido, uso i default")

    return {"networks": networks, "ap": ap}


def validate_ap(ap: Dict[str, Any]) -> Optional[str]:
    if not as_bool(ap.get("enabled"), False):
        return None
    ssid = str(ap.get("ssid", "")).strip()
    if not ssid:
        return "SSID AP vuoto"
    if len(ssid.encode("utf-8")) > 32:
        return "SSID AP troppo lungo (max 32 byte)"
    password = str(ap.get("password", ""))
    if not 8 <= len(password) <= 63:
        return "password AP: servono 8-63 caratteri"
    return None


def ap_fingerprint(ap: Dict[str, Any]) -> str:
    raw = "%s\x00%s\x00%s" % (bool(ap.get("enabled")), ap.get("ssid", ""),
                              ap.get("password", ""))
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


# ── NetworkManager ──────────────────────────────────────────────────────────

def run_cmd(args: Sequence[Any], timeout: float = 15.0, mutate: bool = False,
            dry_run: bool = False) -> subprocess.CompletedProcess:
    cmd = ["nmcli"] + [str(a) for a in args]
    if mutate:
        if dry_run:
            log("[dry-run] %s", redact_cmd(cmd))
            return subprocess.CompletedProcess(cmd, 0, "", "")
        log("$ %s", redact_cmd(cmd))
    elif VERBOSE:
        log("$ %s", redact_cmd(cmd))
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        return subprocess.CompletedProcess(cmd, 127, "", "nmcli non trovato")
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", "timeout")


def last_err(proc: subprocess.CompletedProcess) -> str:
    text = (proc.stderr or "").strip() or (proc.stdout or "").strip()
    return text.splitlines()[-1] if text else "errore sconosciuto"


def nm_get(obj_args: Sequence[str], field: str, timeout: float = 4.0) -> str:
    proc = run_cmd(["-g", field] + list(obj_args), timeout=timeout, mutate=False)
    return proc.stdout.strip() if proc.returncode == 0 else ""


def nm_get_secret(obj_args: Sequence[str], field: str,
                  timeout: float = 4.0) -> str:
    """Come nm_get, ma con -s: serve solo a confrontare la PSK dell'AP."""
    proc = run_cmd(["-s", "-g", field] + list(obj_args), timeout=timeout,
                   mutate=False)
    # Non usare strip(): una passphrase WPA può iniziare/finire con spazi.
    return proc.stdout.rstrip("\r\n") if proc.returncode == 0 else ""


def device_status() -> Dict[str, str]:
    """Stato della radio da nmcli, senza grep fragili."""
    state = nm_get(["device", "show", IFACE], "GENERAL.STATE")
    code = state.split()[0] if state.split() else ""
    con = first_line(nm_get(["device", "show", IFACE], "GENERAL.CONNECTION"))
    ip = first_line(nm_get(["device", "show", IFACE], "IP4.ADDRESS"))
    if ip and "/" in ip:
        ip = ip.split("/", 1)[0]

    mode = "none"
    ssid = ""
    if code == "100" and con:
        profile_mode = first_line(nm_get(["connection", "show", con],
                                         "802-11-wireless.mode"))
        ssid = first_line(nm_get(["connection", "show", con],
                                 "802-11-wireless.ssid")) or con
        mode = "ap" if profile_mode == "ap" or con == AP_CON else "sta"
    return {"mode": mode, "connection": con if mode != "none" else "",
            "ssid": ssid, "ip": ip if mode != "none" else ""}


def scan_wifi(timeout: float = 30.0) -> Dict[str, Dict[str, Any]]:
    base = ["-t", "-f", "IN-USE,SSID,SIGNAL,SECURITY", "device", "wifi", "list"]
    proc = run_cmd(base + ["--rescan", "yes"], timeout=timeout)
    if proc.returncode != 0:
        if proc.stderr.strip():
            log("scansione --rescan fallita: %s", last_err(proc))
        proc = run_cmd(base, timeout=8.0)
        if proc.returncode != 0:
            log("scansione wifi fallita: %s", last_err(proc))
            return {}

    found: Dict[str, Dict[str, Any]] = {}
    for line in proc.stdout.splitlines():
        if not line.strip():
            continue
        fields = split_terse(line)
        if len(fields) < 4:
            continue
        ssid = fields[1].strip()
        if not ssid:
            continue
        try:
            signal = int(fields[2])
        except ValueError:
            signal = 0
        old = found.get(ssid)
        if old is None or signal > old["signal"]:
            found[ssid] = {"ssid": ssid, "signal": signal,
                           "security": fields[3],
                           "in_use": fields[0].strip() == "*"}
    return found


def visible_known(networks: Sequence[Dict[str, Any]],
                  visible: Dict[str, Dict[str, Any]]) -> List[Tuple[Dict[str, Any],
                                                                    Dict[str, Any]]]:
    out: List[Tuple[Dict[str, Any], Dict[str, Any]]] = []
    for net in networks:
        seen = visible.get(net["ssid"])
        if seen:
            out.append((net, seen))
    out.sort(key=lambda item: (-int(item[0].get("priority", 0)),
                               -int(item[1].get("signal", 0))))
    return out


def connection_rows() -> List[Tuple[str, str]]:
    proc = run_cmd(["-t", "-f", "NAME,TYPE", "connection", "show"],
                   timeout=8.0, mutate=False)
    rows: List[Tuple[str, str]] = []
    if proc.returncode != 0:
        return rows
    for line in proc.stdout.splitlines():
        fields = split_terse(line)
        if len(fields) >= 2:
            rows.append((fields[0], fields[1]))
    return rows


def connection_exists(name: str) -> bool:
    return any(row[0] == name for row in connection_rows())


def find_connection(ssid: str) -> Optional[str]:
    rows = connection_rows()
    for name, kind in rows:
        if kind == "802-11-wireless" and name == ssid:
            return name
    for name, kind in rows:
        if kind != "802-11-wireless" or name == AP_CON:
            continue
        if first_line(nm_get(["connection", "show", name],
                             "802-11-wireless.ssid")) == ssid:
            return name
    return None


def ensure_known_connection(net: Dict[str, Any],
                            dry_run: bool = False) -> Optional[str]:
    con = find_connection(net["ssid"])
    if con:
        return con
    con_name = net["ssid"]
    if con_name == AP_CON:
        con_name = con_name + "-sta"
    args: List[str] = ["connection", "add", "type", "wifi", "ifname", IFACE,
                       "con-name", con_name, "ssid", net["ssid"]]
    if net.get("password"):
        args += ["wifi-sec.key-mgmt", "wpa-psk",
                 "wifi-sec.psk", net["password"]]
    proc = run_cmd(args, timeout=20.0, mutate=True, dry_run=dry_run)
    if proc.returncode != 0:
        log("creazione connessione per %s fallita: %s", net["ssid"],
            last_err(proc))
        return None
    return con_name


def connect_network(net: Dict[str, Any], deadline: float,
                    dry_run: bool = False) -> bool:
    con = ensure_known_connection(net, dry_run)
    if not con:
        return False
    if dry_run:
        log("[dry-run] collegherei la rete nota %s (connessione %s)",
            net["ssid"], con)
        return True

    remaining = max(4.0, deadline - time.time())
    proc = run_cmd(["connection", "up", con], timeout=remaining, mutate=True)
    if proc.returncode != 0:
        log("collegamento a %s fallito: %s", net["ssid"], last_err(proc))
        return False

    status = wait_status(lambda st: st["mode"] == "sta", timeout=8.0)
    if status:
        log("collegato a %s (IP %s)", status["ssid"] or net["ssid"],
            status["ip"] or "?")
        return True
    log("nmcli connection up %s ok, ma wlan0 non risulta in STA", con)
    return False


def try_known(candidates: Sequence[Tuple[Dict[str, Any], Dict[str, Any]]],
              deadline: float, dry_run: bool = False) -> Optional[Dict[str, Any]]:
    for net, _seen in candidates:
        if time.time() >= deadline:
            break
        if connect_network(net, deadline, dry_run):
            return net
    return None


def wait_not_ap(timeout: float) -> Dict[str, str]:
    status = wait_status(lambda st: st["mode"] != "ap", timeout=timeout)
    return status or device_status()


def stop_ap(dry_run: bool = False) -> bool:
    status = device_status()
    if status["mode"] != "ap":
        return True
    con = status["connection"] or AP_CON
    log("chiudo l'AP %s", con)
    run_cmd(["connection", "down", con], timeout=20.0, mutate=True,
            dry_run=dry_run)
    if dry_run:
        return True
    status = wait_not_ap(15.0)
    if status["mode"] == "ap":
        log("AP ancora attivo dopo il down (%s)", con)
        return False
    if connection_exists(AP_CON):
        run_cmd(["connection", "modify", AP_CON, "connection.autoconnect", "no"],
                timeout=8.0, mutate=True)
    log("AP chiuso (wlan0 ora in %s)", status["mode"])
    return True


def verify_web(ip: str) -> bool:
    """Verifica la web su 127.0.0.1 e sull'IP dell'AP (0.0.0.0:8080)."""
    targets: List[Tuple[str, int]] = [("127.0.0.1", WEB_PORT)]
    if ip:
        targets.append((ip, WEB_PORT))
    ok = False
    for host, port in targets:
        conn: Optional[http.client.HTTPConnection] = None
        try:
            conn = http.client.HTTPConnection(host, port, timeout=3.0)
            conn.request("GET", "/")
            response = conn.getresponse()
            response.read(1)
            log("web http://%s:%d raggiungibile (HTTP %d)", host, port,
                response.status)
            if host == ip:
                ok = True
        except Exception as exc:
            log("web http://%s:%d NON raggiungibile: %s", host, port, exc)
        finally:
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
    if not ip:
        log("attenzione: AP senza IP IPv4, verifica DHCP/shared non possibile")
    return ok


def start_ap(cfg: Dict[str, Any], dry_run: bool = False) -> bool:
    ap = cfg["ap"]
    if not as_bool(ap.get("enabled"), False):
        log("AP non avviato: disabilitato in config")
        return False
    error = validate_ap(ap)
    if error:
        log("AP non avviato: %s", error)
        return False

    status = device_status()
    if (status["mode"] == "ap" and status["connection"] == AP_CON
            and status["ssid"] == ap["ssid"]):
        if dry_run:
            log("AP gia' attivo: SSID=%s IP=%s", status["ssid"],
                status["ip"] or "?")
            return True
        active_psk = nm_get_secret(["connection", "show", AP_CON],
                                   "802-11-wireless-security.psk")
        if not active_psk or active_psk == ap["password"]:
            log("AP gia' attivo: SSID=%s IP=%s", status["ssid"],
                status["ip"] or "?")
            verify_web(status["ip"])
            return True
        log("AP attivo ma con password diversa dalla config: riavvio")

    if status["mode"] == "ap":
        if not stop_ap(dry_run):
            return False
    elif status["mode"] == "sta" and status["connection"]:
        log("disattivo la STA %s per passare all'AP", status["connection"])
        run_cmd(["connection", "down", status["connection"]], timeout=15.0,
                mutate=True, dry_run=dry_run)
        if not dry_run:
            wait_not_ap(12.0)

    if not dry_run and connection_exists(AP_CON):
        proc = run_cmd(["connection", "delete", AP_CON], timeout=10.0,
                       mutate=True)
        if proc.returncode != 0:
            log("delete %s: %s", AP_CON, last_err(proc))

    log("avvio AP: SSID=%s", ap["ssid"])
    add_args: List[str] = [
        "connection", "add", "type", "wifi", "ifname", IFACE,
        "con-name", AP_CON, "autoconnect", "no", "ssid", ap["ssid"],
        "802-11-wireless.mode", "ap",
        "802-11-wireless.band", "bg",
        "802-11-wireless.channel", "6",
        "wifi-sec.key-mgmt", "wpa-psk",
        "wifi-sec.psk", ap["password"],
        "ipv4.method", "shared",
        "ipv6.method", "ignore",
    ]
    proc = run_cmd(add_args, timeout=20.0, mutate=True, dry_run=dry_run)
    if proc.returncode == 0:
        proc = run_cmd(["connection", "up", AP_CON], timeout=30.0,
                       mutate=True, dry_run=dry_run)

    if proc.returncode != 0:
        log("connection add/up AP fallita: %s; provo nmcli device wifi hotspot",
            last_err(proc))
        if not dry_run and connection_exists(AP_CON):
            run_cmd(["connection", "delete", AP_CON], timeout=10.0, mutate=True)
        proc = run_cmd(["device", "wifi", "hotspot", "ifname", IFACE,
                        "con-name", AP_CON, "ssid", ap["ssid"],
                        "password", ap["password"]],
                       timeout=35.0, mutate=True, dry_run=dry_run)

    if proc.returncode != 0:
        log("avvio hotspot fallito: %s", last_err(proc))
        return False
    if dry_run:
        log("[dry-run] avrei avviato l'AP %s (nessun cambiamento reale)",
            ap["ssid"])
        return True

    run_cmd(["connection", "modify", AP_CON, "connection.autoconnect", "no"],
            timeout=8.0, mutate=True)
    method = first_line(nm_get(["connection", "show", AP_CON], "ipv4.method"))
    if method and method != "shared":
        log("AP: ipv4.method=%s, lo imposto a shared", method)
        run_cmd(["connection", "modify", AP_CON, "ipv4.method", "shared"],
                timeout=8.0, mutate=True)
        run_cmd(["connection", "down", AP_CON], timeout=15.0, mutate=True)
        run_cmd(["connection", "up", AP_CON], timeout=25.0, mutate=True)

    status = wait_status(lambda st: st["mode"] == "ap", timeout=AP_VERIFY_TIMEOUT)
    if not status:
        log("AP non attivo dopo l'avvio")
        return False
    log("AP attivo: SSID=%s iface=%s IP=%s", status["ssid"], IFACE,
        status["ip"] or "?")
    verify_web(status["ip"])
    return True


def switch_to_sta(candidates: Sequence[Tuple[Dict[str, Any], Dict[str, Any]]],
                  cfg: Dict[str, Any]) -> bool:
    log("provo a tornare in STA: %s", ", ".join(net["ssid"] for net, _ in candidates))
    if not stop_ap():
        log("non riesco a chiudere l'AP: resto in AP")
        return False
    status = device_status()
    if status["mode"] == "sta":
        log("gia' in STA su %s", status["ssid"] or status["connection"])
        return True
    net = try_known(candidates, deadline=time.time() + 35.0)
    if net:
        log("tornato in STA sulla rete nota %s", net["ssid"])
        return True
    log("nessuna rete nota collegabile: riavvio l'AP")
    if as_bool(cfg["ap"].get("enabled"), False):
        start_ap(cfg)
    return False


# ── decisioni pure (testabili senza nmcli) ──────────────────────────────────

def decide_startup(link_ok: bool, known_visible: bool, ap_enabled: bool) -> str:
    if link_ok:
        return "keep_sta"
    if known_visible:
        return "connect_sta"
    if ap_enabled:
        return "start_ap"
    return "wait"


def decide_ap(known_visible: bool) -> str:
    return "switch_sta" if known_visible else "stay_ap"


def decide_sta(link_ok: bool, known_visible: bool, ap_enabled: bool,
               failures: int) -> str:
    if link_ok:
        return "stay_sta"
    if known_visible:
        return "reconnect_sta"
    if ap_enabled and failures >= STA_FAILURES_BEFORE_AP:
        return "start_ap"
    return "wait"


# ── dry-run ─────────────────────────────────────────────────────────────────

def run_dry(cfg: Dict[str, Any], simulate: str) -> int:
    log("dry-run: nessun comando modifica NetworkManager")
    log("config: %d reti note; AP %s (ssid=%s)", len(cfg["networks"]),
        "abilitato" if cfg["ap"]["enabled"] else "disabilitato",
        cfg["ap"]["ssid"])

    if simulate == "no-link":
        status = {"mode": "none", "connection": "", "ssid": "", "ip": ""}
        known: List[Tuple[Dict[str, Any], Dict[str, Any]]] = []
        log("simulazione: nessun link e nessuna rete nota visibile")
    elif simulate == "known":
        status = {"mode": "none", "connection": "", "ssid": "", "ip": ""}
        nets = cfg["networks"]
        known = [(nets[0], {"ssid": nets[0]["ssid"], "signal": 0})] if nets else []
        if known:
            log("simulazione: rete nota visibile = %s", nets[0]["ssid"])
        else:
            log("simulazione: nessuna rete nota in config")
    else:
        status = device_status()
        visible = scan_wifi()
        known = visible_known(cfg["networks"], visible)
        log("stato: mode=%s connessione=%s ssid=%s ip=%s", status["mode"],
            status["connection"] or "-", status["ssid"] or "-",
            status["ip"] or "-")
        log("reti note visibili: %s",
            ", ".join(net["ssid"] for net, _ in known) or "nessuna")

    if status["mode"] == "ap":
        if decide_ap(bool(known)) == "switch_sta":
            log("decisione: chiuderei l'AP e passerei in STA su %s",
                known[0][0]["ssid"])
        else:
            log("decisione: resterei in AP (nessuna rete nota visibile)")
    elif status["mode"] == "sta":
        log("decisione: resto in STA su %s (l'AP non serve)",
            status["ssid"] or status["connection"])
    else:
        decision = decide_startup(False, bool(known), cfg["ap"]["enabled"])
        if decision == "connect_sta":
            log("decisione: proverei la rete nota %s (STA)", known[0][0]["ssid"])
        elif decision == "start_ap":
            error = validate_ap(cfg["ap"])
            if error:
                log("decisione: AP richiesto ma non avviabile: %s", error)
            else:
                log("decisione: avvierei l'AP %s (nessuna rete nota)",
                    cfg["ap"]["ssid"])
        else:
            log("decisione: resto in attesa (AP disabilitato)")
    return 0


# ── servizio ────────────────────────────────────────────────────────────────

def initial_connect(cfg: Dict[str, Any]) -> bool:
    deadline = time.time() + STARTUP_WINDOW
    log("nessun link: cerco reti note per ~%.0f s", STARTUP_WINDOW)
    while not STOP and time.time() < deadline:
        visible = scan_wifi(timeout=12.0)
        known = visible_known(cfg["networks"], visible)
        if known:
            log("reti note visibili: %s",
                ", ".join(net["ssid"] for net, _ in known))
            if try_known(known, deadline):
                return True
        remaining = deadline - time.time()
        if remaining > 0:
            sleep_s(min(2.0, remaining))
    return False


def run_daemon(config_path: str, once: bool = False) -> int:
    global STOP

    def on_signal(signum: int, _frame: Any) -> None:
        global STOP
        STOP = True
        log("ricevuto segnale %s: esco", signum)

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    cfg = load_config(config_path)
    if cfg is None:
        return 1

    status = device_status()
    log("avvio: mode=%s connessione=%s ssid=%s ip=%s", status["mode"],
        status["connection"] or "-", status["ssid"] or "-", status["ip"] or "-")

    if status["mode"] == "ap":
        if not as_bool(cfg["ap"].get("enabled"), False):
            log("AP attivo ma disabilitato in config: lo chiudo")
            stop_ap()
        else:
            error = validate_ap(cfg["ap"])
            if error:
                log("AP attivo ma config non valida: %s (lo lascio attivo)",
                    error)
            else:
                log("AP attivo: verifico che corrisponda alla config")
                start_ap(cfg)
    elif status["mode"] == "sta":
        log("gia' collegato a %s: resto in STA", status["ssid"] or status["connection"])
    else:
        if initial_connect(cfg):
            status = device_status()
            log("connessione iniziale riuscita: %s", status["ssid"])
        elif as_bool(cfg["ap"].get("enabled"), False):
            log("nessuna rete nota raggiungibile: avvio l'AP di fallback")
            start_ap(cfg)
        else:
            log("nessuna rete nota raggiungibile e AP disabilitato: attendo")

    if once:
        return 0

    failures = 0
    ap_fp = ap_fingerprint(cfg["ap"]) if device_status()["mode"] == "ap" else None

    while not STOP:
        cfg = load_config(config_path)
        if cfg is None:
            sleep_s(NO_LINK_INTERVAL)
            continue
        status = device_status()

        if status["mode"] == "ap":
            if not as_bool(cfg["ap"].get("enabled"), False):
                log("AP disabilitato da config: chiudo l'AP")
                stop_ap()
                sleep_s(5.0)
                continue
            error = validate_ap(cfg["ap"])
            if error:
                log("config AP non valida (%s): lascio l'AP attivo", error)
            elif ap_fp is None:
                ap_fp = ap_fingerprint(cfg["ap"])
            elif ap_fp != ap_fingerprint(cfg["ap"]):
                log("config AP cambiata: riavvio l'AP")
                new_fp = ap_fingerprint(cfg["ap"])
                if stop_ap() and start_ap(cfg):
                    ap_fp = new_fp
                elif device_status()["mode"] == "ap":
                    # AP ancora attivo con la vecchia config: riprovo al
                    # prossimo giro invece di perdere il riferimento.
                    log("riavvio AP non riuscito, riprovo tra %.0f s",
                        AP_SCAN_INTERVAL)
                else:
                    ap_fp = None
                continue

            log("AP attivo (SSID=%s IP=%s): controllo reti note tra %.0f s",
                status["ssid"], status["ip"] or "?", AP_SCAN_INTERVAL)
            sleep_s(AP_SCAN_INTERVAL)
            if STOP:
                break
            cfg = load_config(config_path)
            if cfg is None:
                continue
            visible = scan_wifi()
            known = visible_known(cfg["networks"], visible)
            if known:
                log("in AP: rete nota visibile %s", known[0][0]["ssid"])
                if switch_to_sta(known, cfg):
                    ap_fp = None
                else:
                    ap_fp = ap_fingerprint(cfg["ap"])
            else:
                log("in AP: nessuna rete nota visibile (%d reti note)",
                    len(cfg["networks"]))

        elif status["mode"] == "sta":
            failures = 0
            if VERBOSE:
                log("STA attiva su %s (IP %s)", status["ssid"] or status["connection"],
                    status["ip"] or "?")
            sleep_s(STA_CHECK_INTERVAL)

        else:
            sleep_s(NO_LINK_INTERVAL)
            if STOP:
                break
            cfg = load_config(config_path)
            if cfg is None:
                continue
            visible = scan_wifi()
            known = visible_known(cfg["networks"], visible)
            if known:
                log("nessun link: provo le reti note %s",
                    ", ".join(net["ssid"] for net, _ in known))
                if try_known(known, deadline=time.time() + 25.0):
                    failures = 0
                    continue
            failures += 1
            log("nessun link (controllo %d/%d)", failures,
                STA_FAILURES_BEFORE_AP)
            decision = decide_sta(False, bool(known), cfg["ap"]["enabled"],
                                  failures)
            if decision == "start_ap":
                if start_ap(cfg):
                    ap_fp = ap_fingerprint(cfg["ap"])
                failures = 0
    return 0


# ── test delle decisioni (--test) ───────────────────────────────────────────

def run_selftests() -> int:
    failures: List[str] = []

    def check(name: str, condition: bool) -> None:
        print("  %s %s" % ("OK  " if condition else "FAIL", name))
        if not condition:
            failures.append(name)

    print("[wifi] test delle decisioni (nessun comando nmcli)")
    check("avvio senza link ne' reti note -> AP",
          decide_startup(False, False, True) == "start_ap")
    check("avvio con rete nota -> STA",
          decide_startup(False, True, True) == "connect_sta")
    check("avvio con link gia' attivo -> resta STA",
          decide_startup(True, False, True) == "keep_sta")
    check("AP disabilitato e nessuna rete -> attesa",
          decide_startup(False, False, False) == "wait")
    check("AP con rete nota visibile -> STA",
          decide_ap(True) == "switch_sta")
    check("AP senza reti note -> resta AP",
          decide_ap(False) == "stay_ap")
    check("STA persa, prima assenza -> attesa (isteresi)",
          decide_sta(False, False, True, 1) == "wait")
    check("STA persa, seconda assenza -> AP",
          decide_sta(False, False, True, 2) == "start_ap")
    check("STA persa ma rete nota -> riconnessione",
          decide_sta(False, True, True, 1) == "reconnect_sta")
    check("parsing nmcli con ':' escapato",
          split_terse(r"*:my\:ssid:80:WPA2") == ["*", "my:ssid", "80", "WPA2"])
    check("parsing nmcli con '\\' escapato",
          split_terse(r" :a\\b:70:WPA2") == [" ", "a\\b", "70", "WPA2"])
    check("AP: password corta rifiutata",
          validate_ap({"enabled": True, "ssid": "x", "password": "short"}) is not None)
    check("AP: SSID vuoto rifiutato",
          validate_ap({"enabled": True, "ssid": "", "password": "12345678"}) is not None)
    check("AP: configurazione valida accettata",
          validate_ap({"enabled": True, "ssid": "namzero-lite",
                       "password": "namzero-lite"}) is None)
    check("AP disabilitato non richiede password",
          validate_ap({"enabled": False, "ssid": "", "password": ""}) is None)

    nets = [{"ssid": "a", "password": "", "priority": 1},
            {"ssid": "b", "password": "", "priority": 5},
            {"ssid": "c", "password": "", "priority": 5}]
    visible = {"a": {"signal": 90}, "b": {"signal": 40}, "c": {"signal": 70}}
    ordered = [net["ssid"] for net, _ in visible_known(nets, visible)]
    check("priorita' e segnale ordinano le reti", ordered == ["c", "b", "a"])

    if failures:
        print("[wifi] %d test falliti" % len(failures))
        return 1
    print("[wifi] tutti i test delle decisioni passati")
    return 0


# ── main ────────────────────────────────────────────────────────────────────

def main(argv: Optional[Sequence[str]] = None) -> int:
    global VERBOSE
    parser = argparse.ArgumentParser(
        description="WiFi noto + access point di fallback per namzero-lite")
    parser.add_argument("--config", default=DEFAULT_CONFIG,
                        help="file wifi.json (default: config/wifi.json)")
    parser.add_argument("--dry-run", action="store_true",
                        help="stampa le decisioni senza modificare NetworkManager")
    parser.add_argument("--simulate", choices=("auto", "no-link", "known"),
                        default="auto",
                        help="con --dry-run: forza nessun link o una rete nota")
    parser.add_argument("--test", action="store_true",
                        help="esegue i test delle decisioni ed esce")
    parser.add_argument("--once", action="store_true",
                        help="esegue solo la fase di avvio, poi esce "
                             "(può avviare l'AP: usare --dry-run nei test)")
    parser.add_argument("--verbose", action="store_true",
                        help="logga anche i comandi nmcli di sola lettura")
    args = parser.parse_args(argv)
    VERBOSE = args.verbose

    if args.test:
        return run_selftests()

    cfg = load_config(args.config)
    if cfg is None:
        return 1
    if args.dry_run:
        return run_dry(cfg, args.simulate)
    return run_daemon(args.config, once=args.once)


if __name__ == "__main__":
    sys.exit(main())
