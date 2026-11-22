#!/usr/bin/env python3
"""Servizio web locale per namzero-lite (nessuna dipendenza esterna).

Serve la pagina, inoltra comandi/stato al socket di controllo del motore,
gestisce file, reti WiFi conosciute e l'import da TONE3000.
"""
import base64
import hashlib
import http.server
import ipaddress
import json
import os
import re
import secrets
import select
import shutil
import socket
import subprocess
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
CTL = "/tmp/namzero-lite-ctl.sock"
# Config viva delle opzioni globali (slot, gate, eq, tuner, audio). L'override
# da ambiente serve solo ai test: in produzione resta config/presets.json.
PRESETS_CFG = (os.environ.get("NAMZERO_PRESETS_CONFIG")
               or os.path.join(ROOT, "config/presets.json"))
# Override solo per test: la config viva resta config/wifi.json.
WIFI = os.environ.get("NAMZERO_WIFI_CONFIG") or os.path.join(ROOT, "config/wifi.json")
WIFI_AP_DEFAULT = {"enabled": True, "ssid": "namzero-lite",
                   "password": "namzero-lite"}
WIFI_AP_CON = "namzero-lite-ap"
T3K_API = "https://www.tone3000.com"
T3K_CFG = os.path.join(ROOT, "config/tone3000.json")
PORT = int(os.environ.get("NAMZERO_WEB_PORT", "8080"))
_pending = {}  # state -> {verifier, redirect_uri}


def _lan_ip():
    """IP dell'interfaccia usata per uscire, non il 127.0.1.1 dell'hostname."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


# ── motore ──────────────────────────────────────────────────────────────────

# Comandi idempotenti: se il re-exec chiude la connessione a meta' richiesta si
# possono ripetere senza applicarli due volte (looper e rename restano fuori).
_RETRYABLE = {"state", "slot", "reload", "param", "set", "file", "save",
              "save_options", "defaults", "boost", "autoengage", "eq", "tuner"}


def _ctl_once(payload):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.5)
    try:
        s.connect(CTL)
        f = s.makefile("rwb")
        if payload is not None:
            f.write((json.dumps(payload) + "\n").encode())
            f.flush()
            if payload.get("cmd") == "looper":
                # il looper applica il comando nel thread audio al blocco dopo:
                # si aspetta un attimo prima di leggere lo stato.
                time.sleep(0.09)
            if (payload.get("cmd") == "set" and payload.get("slot") == "mod"
                    and "position" in payload):
                # Il cambio posizione del mod salva il preset e fa re-exec del
                # motore (~300 ms): la connessione cade. Si riapre e si legge
                # lo stato nuovo, invece di restituire un errore alla UI.
                s.close()
                time.sleep(0.7)
                return ctl(None)
        f.write(b'{"cmd":"state"}\n')
        f.flush()
        line = f.readline()
        # L'engine manda anche gli snapshot in broadcast: il primo messaggio
        # puo' essere quello precedente al comando. Si legge l'ultimo
        # disponibile in una finestra breve e si chiude subito (il motore
        # accetta piu' client, ma la connessione non va tenuta aperta).
        s.settimeout(0.03)
        try:
            while True:
                nxt = f.readline()
                if not nxt:
                    break
                line = nxt
        except (socket.timeout, OSError):
            pass
        if not line:
            raise RuntimeError("motore senza risposta")
        return json.loads(line)
    finally:
        s.close()


def ctl(payload):
    # Il cambio preset rilancia il motore (~300-500 ms): la socket sparisce e
    # ricompare, e la connessione puo' cadere anche dopo il connect se l'exec
    # avviene nel mezzo. Si ripete per ~4 s invece di rispondere 503: la UI
    # non deve mostrare "disconnesso" durante un cambio preset.
    retryable = payload is None or payload.get("cmd") in _RETRYABLE
    deadline = time.time() + 4.0
    while True:
        try:
            return _ctl_once(payload)
        except Exception:
            if not retryable or time.time() >= deadline:
                raise
            time.sleep(0.1)


# ── TONE3000 ────────────────────────────────────────────────────────────────

def t3k_cfg():
    try:
        with open(T3K_CFG) as f:
            return json.load(f)
    except Exception:
        return {"client_id": "", "tokens": None}


def t3k_save(cfg):
    with open(T3K_CFG, "w") as f:
        json.dump(cfg, f, indent=2)
    os.chmod(T3K_CFG, 0o600)


def _b64u(raw):
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def _http(method, url, data=None, headers=None, timeout=60):
    # Cloudflare di TONE3000 blocca lo User-Agent di urllib sui download
    # (403 error 1010): si presenta come browser.
    hdrs = {"User-Agent": "Mozilla/5.0 (X11; Linux aarch64) namzero-lite"}
    hdrs.update(headers or {})
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status, r.read(), dict(r.headers)


def t3k_token():
    cfg = t3k_cfg()
    toks = cfg.get("tokens") or {}
    if not toks or not cfg.get("client_id"):
        return None
    if time.time() > float(toks.get("expires_at", 0)) - 60:
        body = urllib.parse.urlencode({
            "grant_type": "refresh_token",
            "refresh_token": toks.get("refresh_token", ""),
            "client_id": cfg["client_id"],
        }).encode()
        try:
            _, raw, _ = _http("POST", T3K_API + "/api/v1/oauth/token", body,
                              {"Content-Type": "application/x-www-form-urlencoded"})
            d = json.loads(raw)
        except Exception:
            return None
        toks = {"access_token": d["access_token"],
                "refresh_token": d.get("refresh_token", toks.get("refresh_token", "")),
                "expires_at": time.time() + float(d.get("expires_in", 3600))}
        cfg["tokens"] = toks
        t3k_save(cfg)
    return toks["access_token"]


def t3k_get(path, raw=False):
    tok = t3k_token()
    if not tok:
        raise RuntimeError("TONE3000 non connesso")
    _, body, _ = _http("GET", T3K_API + path, headers={"Authorization": "Bearer " + tok})
    return body if raw else json.loads(body)


def presets_list():
    """Elenco dei preset configurati: [{"slot":0,"name":"vox_wah"}, ...].

    Legge la config viva (config/presets.json) e ricava il nome dal path
    "presets/<nome>.json". Gli slot vuoti/null vengono saltati.
    """
    with open(PRESETS_CFG) as f:
        cfg = json.load(f)
    out = []
    for slot, entry in enumerate(cfg.get("slots", [])):
        if not isinstance(entry, str) or not entry:
            continue
        name = os.path.basename(entry)
        if name.endswith(".json"):
            name = name[:-5]
        if name:
            out.append({"slot": slot, "name": name})
    return out


# ── Interfaccia audio ──────────────────────────────────────────────────────

AUDIO_DEFAULTS = {"device": "hw:XTONE", "buffer": 64, "periods": 3, "input": 0,
                  "output": 0}
AUDIO_RATE = 48000  # fisso: NAM/IR/accordatore assumono 48 kHz
AUDIO_LIMITS = {"buffer": (16, 2048), "periods": (2, 8), "input": (0, 1),
                "output": (0, 2)}
AUDIO_INPUTS = [
    {"index": 0, "label": "Canale 1 (L)"},
    {"index": 1, "label": "Canale 2 (R)"},
]
AUDIO_OUTPUTS = [
    {"index": 0, "label": "Stereo L/R"},
    {"index": 1, "label": "Canale 1"},
    {"index": 2, "label": "Canale 2"},
]


def _read_presets():
    """Legge config/presets.json. File assente = oggetto vuoto; JSON rotto = errore."""
    try:
        with open(PRESETS_CFG) as f:
            raw = json.load(f)
    except FileNotFoundError:
        return {}
    if not isinstance(raw, dict):
        raise ValueError("presets.json: la radice deve essere un oggetto")
    return raw


def audio_config():
    """Config audio normalizzata (default se il blocco manca o e' incompleto)."""
    cfg = dict(AUDIO_DEFAULTS)
    raw = _read_presets()
    block = raw.get("audio")
    if not isinstance(block, dict):
        return cfg
    device = block.get("device")
    if isinstance(device, str) and device.strip():
        cfg["device"] = device.strip()
    for key in ("buffer", "periods", "input", "output"):
        value = block.get(key)
        if isinstance(value, int) and not isinstance(value, bool):
            cfg[key] = max(AUDIO_LIMITS[key][0], min(AUDIO_LIMITS[key][1], value))
        elif value is not None:
            # Tipo sbagliato nel file: si resta al default invece di far
            # fallire la UI; il motore fa lo stesso clamp.
            cfg[key] = AUDIO_DEFAULTS[key]
    return cfg


def _validate_audio(data, current):
    """Valida il POST e restituisce il blocco normalizzato.

    Solleva ValueError con un messaggio mostrabile nella UI.
    """
    if not isinstance(data, dict):
        raise ValueError("corpo JSON non valido")
    out = dict(current)

    if "device" in data:
        device = data["device"]
        if not isinstance(device, str):
            raise ValueError("device deve essere una stringa")
        device = device.strip()
        if not device:
            raise ValueError("device non puo' essere vuoto (es. hw:XTONE)")
        if len(device) > 128:
            raise ValueError("device troppo lungo (max 128 caratteri)")
        if any(ch.isspace() or ord(ch) < 32 for ch in device):
            raise ValueError("device non puo' contenere spazi o caratteri di controllo")
        out["device"] = device

    for key in ("buffer", "periods", "input", "output"):
        if key not in data:
            continue
        value = data[key]
        if isinstance(value, bool) or not isinstance(value, int):
            raise ValueError("%s deve essere un intero" % key)
        lo, hi = AUDIO_LIMITS[key]
        if not lo <= value <= hi:
            raise ValueError("%s deve stare fra %d e %d" % (key, lo, hi))
        out[key] = value
    return out


def _write_audio(cfg):
    """Salva il solo blocco audio, in modo atomico, con backup del file vivo."""
    raw = _read_presets()
    raw["audio"] = {"device": cfg["device"],
                    "buffer": cfg["buffer"],
                    "periods": cfg["periods"],
                    "input": cfg["input"],
                    "output": cfg["output"]}
    tmp = PRESETS_CFG + ".tmp"
    try:
        parent = os.path.dirname(PRESETS_CFG)
        if parent:
            os.makedirs(parent, exist_ok=True)
        if os.path.exists(PRESETS_CFG):
            shutil.copy2(PRESETS_CFG, PRESETS_CFG + ".bak")
            mode = os.stat(PRESETS_CFG).st_mode & 0o777
        else:
            mode = 0o644
        with open(tmp, "w") as f:
            json.dump(raw, f, indent=2)
            f.write("\n")
        os.chmod(tmp, mode)
        os.replace(tmp, PRESETS_CFG)
    finally:
        if os.path.exists(tmp):
            try:
                os.unlink(tmp)
            except OSError:
                pass


_APLAY_RE = re.compile(
    r"^\s*card\s+(\d+):\s+(\S+)\s+\[([^\]]*)\],\s*"
    r"device\s+(\d+):\s+(.*?)\s+\[([^\]]*)\]\s*$")


def parse_aplay_l(text):
    """Estrae le card da `aplay -l` senza grep fragili.

    Formato di riferimento:
      card 1: XTONE [XTONE], device 0: USB Audio [USB Audio]
    Restituisce dict con card/device/name/id e i nomi PCM pronti per la UI.
    """
    devices = []
    for line in (text or "").splitlines():
        match = _APLAY_RE.match(line)
        if not match:
            continue
        card, card_id, card_name, dev, dev_name, dev_id = match.groups()
        card = int(card)
        dev = int(dev)
        card_id = card_id.strip()
        name = (dev_name or card_name or card_id).strip()
        pcm = "hw:%s" % card_id if dev == 0 else "hw:%s,%d" % (card_id, dev)
        plughw = ("plughw:%s" % card_id if dev == 0
                  else "plughw:%s,%d" % (card_id, dev))
        devices.append({
            "card": card,
            "device": dev,
            "name": name,
            "id": card_id,
            "pcm": pcm,
            "plughw": plughw,
            "label": "%s — %s (card %d, device %d)" % (card_id, name, card, dev),
        })
    return devices


def audio_devices():
    """Elenco device da `aplay -l`, con `default` in testa."""
    devices = []
    error = ""
    try:
        proc = subprocess.run(["aplay", "-l"], capture_output=True, text=True,
                              timeout=3)
        if proc.returncode == 0:
            devices = parse_aplay_l(proc.stdout)
        else:
            error = (proc.stderr or proc.stdout or "").strip()
    except (OSError, subprocess.SubprocessError) as exc:
        error = str(exc)
    # "default" non compare in aplay -l ma e' un PCM ALSA valido e utile come
    # fallback/alternativa: la UI lo mostra sempre in testa.
    devices.insert(0, {
        "card": None, "device": None,
        "name": "PCM di sistema (ALSA default)",
        "id": "default", "pcm": "default", "plughw": "default",
        "label": "default — PCM di sistema",
    })
    return {"devices": devices, "error": error}


def _audio_status():
    """Stato audio dal motore, senza i retry lunghi pensati per il cambio preset.

    Se il motore non risponde si restituisce open=false con il motivo: la web
    UI resta usabile e mostra l'errore.
    """
    try:
        st = _ctl_once(None)
    except Exception as exc:
        return {"open": False, "error": "motore non raggiungibile: %s" % exc}
    audio = st.get("audio") if isinstance(st, dict) else None
    if not isinstance(audio, dict):
        return {"open": False, "error": "stato audio assente nel motore"}
    return audio


def _audio_reload(cfg):
    """Chiede al motore il reload e aspetta che rinasca con la nuova config.

    Il re-exec mantiene il PID: si usa audio.boot_id per distinguere il nuovo
    processo da quello vecchio. Timeout ~8 s; la config e' gia' salvata anche
    se il reload non riesce.
    """
    before = _audio_status().get("boot_id")
    try:
        ctl({"cmd": "reload"})
    except Exception as exc:
        return {
            "reloaded": False,
            "warning": ("motore non raggiungibile (%s): config salvata, "
                        "sara' applicata al prossimo avvio" % exc),
            "status": _audio_status(),
        }

    deadline = time.time() + 8.0
    last = None
    while time.time() < deadline:
        last = _audio_status()
        same_boot = before is not None and last.get("boot_id") == before
        if (last.get("config_device") == cfg["device"] and not same_boot
                and "open" in last):
            return {"reloaded": True, "status": last}
        time.sleep(0.15)
    return {
        "reloaded": False,
        "warning": "il motore non ha confermato la nuova interfaccia audio",
        "status": last or _audio_status(),
    }


# ── WiFi: config e stato NM ────────────────────────────────────────────────

def _as_bool(value, default=False):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "yes", "on", "si", "sì")
    return default


def _clean_networks(nets):
    clean = []
    for net in nets or []:
        if not isinstance(net, dict):
            continue
        ssid = str(net.get("ssid", "")).strip()
        if not ssid:
            continue
        try:
            priority = int(net.get("priority", 0))
        except (TypeError, ValueError):
            priority = 0
        clean.append({"ssid": ssid,
                      "password": str(net.get("password", "")),
                      "priority": priority})
    return clean


def wifi_config():
    """Config WiFi normalizzata: reti note + blocco ap (default se assente)."""
    try:
        with open(WIFI) as f:
            raw = json.load(f)
    except Exception:
        raw = {}
    if not isinstance(raw, dict):
        raw = {}
    ap = dict(WIFI_AP_DEFAULT)
    raw_ap = raw.get("ap")
    if isinstance(raw_ap, dict):
        ap["enabled"] = _as_bool(raw_ap.get("enabled", ap["enabled"]),
                                 ap["enabled"])
        ap["ssid"] = str(raw_ap.get("ssid", ap["ssid"])).strip() or ap["ssid"]
        ap["password"] = str(raw_ap.get("password", ap["password"]))
    return {"networks": _clean_networks(raw.get("networks")), "ap": ap}


def _clean_ap(raw, current):
    """Valida il blocco ap del POST; solleva ValueError con un messaggio."""
    if not isinstance(raw, dict):
        raise ValueError("blocco ap non valido")
    ap = dict(current)
    if "enabled" in raw:
        ap["enabled"] = _as_bool(raw["enabled"], ap["enabled"])
    if "ssid" in raw:
        ap["ssid"] = str(raw["ssid"]).strip()
    if "password" in raw:
        ap["password"] = str(raw["password"])
    if not ap["ssid"]:
        raise ValueError("SSID AP mancante")
    if len(ap["ssid"].encode("utf-8")) > 32:
        raise ValueError("SSID AP troppo lungo (max 32 byte)")
    if ap["enabled"] and not 8 <= len(ap["password"]) <= 63:
        raise ValueError("password AP: servono 8-63 caratteri")
    return ap


def _write_wifi(cfg):
    tmp = WIFI + ".tmp"
    with open(tmp, "w") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, WIFI)


def _nm_get(obj_args, field, timeout=3):
    """Valore di un campo nmcli con -g, senza parsing di output fragile."""
    try:
        p = subprocess.run(["nmcli", "-g", field] + list(obj_args),
                           capture_output=True, text=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        return ""
    if p.returncode != 0:
        return ""
    return p.stdout.strip()


def _first_line(value):
    return value.strip().splitlines()[0].strip() if value and value.strip() else ""


def wifi_status():
    """Stato della radio per la UI: mode sta|ap|none + ssid/ip/ap_ssid."""
    cfg = wifi_config()
    out = {"mode": "none", "ssid": "", "ip": "",
           "ap_ssid": cfg["ap"]["ssid"]}
    try:
        state = _nm_get(["device", "show", "wlan0"], "GENERAL.STATE")
        code = state.split()[0] if state.split() else ""
        if code != "100":
            return out
        con = _first_line(_nm_get(["device", "show", "wlan0"],
                                  "GENERAL.CONNECTION"))
        if not con:
            return out
        mode = _first_line(_nm_get(["connection", "show", con],
                                   "802-11-wireless.mode"))
        ssid = (_first_line(_nm_get(["connection", "show", con],
                                    "802-11-wireless.ssid")) or con)
        ip = _first_line(_nm_get(["device", "show", "wlan0"], "IP4.ADDRESS"))
        if "/" in ip:
            ip = ip.split("/", 1)[0]
        out.update({"mode": "ap" if mode == "ap" or con == WIFI_AP_CON else "sta",
                    "ssid": ssid, "ip": ip})
    except Exception:
        pass
    return out


# ── HTTP ────────────────────────────────────────────────────────────────────

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WEB, **kwargs)

    def log_message(self, *args):
        pass

    def _json(self, obj, code=200):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.end_headers()

    # ── Server-Sent Events ──────────────────────────────────────────────────
    # Una connessione ctl persistente per ogni client SSE. Il motore manda lo
    # stato in broadcast quando cambia; qui ogni riga diventa "data: <json>".
    # Il bridge resta connesso anche durante il re-exec del motore: se la
    # socket cade si riconnette in background e il browser non se ne accorge.
    # Il keepalive verso il motore ("ping") e' separato dall'heartbeat SSE:
    # il motore chiude i client che non mandano comandi da ~10 s, quindi il
    # ping parte ogni 4 s; il commento ": heartbeat" va al browser ogni 15 s.
    def _sse(self):
        self.close_connection = True
        try:
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream; charset=utf-8")
            self.send_header("Cache-Control", "no-cache, no-transform")
            self.send_header("Connection", "close")
            self.send_header("X-Accel-Buffering", "no")
            self.end_headers()
            self.wfile.write(b"retry: 1500\n\n")
            self.wfile.flush()
        except OSError:
            return

        sock = None
        buf = b""
        last_beat = time.time()
        last_ping = 0.0
        try:
            while True:
                if sock is None:
                    try:
                        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                        sock.settimeout(2.5)
                        sock.connect(CTL)
                        buf = b""
                        last_ping = 0.0
                    except OSError:
                        if sock is not None:
                            sock.close()
                        sock = None
                        # Motore in re-exec o non ancora partito: si riprova
                        # senza chiudere lo stream SSE.
                        time.sleep(0.2)
                        now = time.time()
                        if now - last_beat >= 15.0:
                            self.wfile.write(b": heartbeat\n\n")
                            self.wfile.flush()
                            last_beat = now
                        continue

                try:
                    # Si aspetta sia il motore sia il client: se il browser
                    # chiude la connessione, recv() torna 0 e si esce subito
                    # (senza aspettare l'heartbeat dei 15 s).
                    ready, _, _ = select.select([sock, self.connection], [], [], 0.25)
                except OSError:
                    ready = []
                if self.connection in ready:
                    try:
                        junk = self.connection.recv(4096)
                    except OSError:
                        junk = b""
                    if not junk:
                        return
                    ready = [s for s in ready if s is not self.connection]
                if sock in ready:
                    try:
                        data = sock.recv(65536)
                    except OSError:
                        data = b""
                    if not data:
                        # Motore riavviato (o socket chiusa): si riconnette.
                        sock.close()
                        sock = None
                        continue
                    buf += data
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        line = line.strip()
                        if not line:
                            continue
                        self.wfile.write(b"data: " + line + b"\n\n")
                        self.wfile.flush()

                now = time.time()
                if now - last_ping >= 4.0:
                    try:
                        sock.sendall(b'{"cmd":"ping"}\n')
                    except OSError:
                        sock.close()
                        sock = None
                    last_ping = now
                if now - last_beat >= 15.0:
                    self.wfile.write(b": heartbeat\n\n")
                    self.wfile.flush()
                    last_beat = now
        except (BrokenPipeError, ConnectionResetError, OSError):
            return
        finally:
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass

    def do_GET(self):
        if self.path.split("?", 1)[0] == "/api/events":
            self._sse()
            return
        path = self.path.split("?", 1)[0]
        if path == "/api/audio/devices":
            try:
                self._json(audio_devices())
            except Exception as exc:
                self._json({"error": str(exc)}, 500)
            return
        if path == "/api/audio":
            try:
                cfg = audio_config()
                status = _audio_status()
                self._json({"ok": True, **cfg, "rate": AUDIO_RATE,
                            "inputs": AUDIO_INPUTS, "outputs": AUDIO_OUTPUTS,
                            "status": status})
            except Exception as exc:
                self._json({"error": str(exc)}, 500)
            return
        if self.path.startswith("/api/presets"):
            try:
                self._json(presets_list())
            except Exception as exc:
                self._json({"error": str(exc)}, 500)
            return
        if self.path.startswith("/api/state"):
            try:
                self._json(ctl(None))
            except Exception as exc:
                self._json({"error": str(exc)}, 503)
            return
        if self.path.startswith("/api/files"):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            kind = q.get("kind", ["models"])[0]
            ext = ".nam" if kind == "models" else ".wav"
            d = os.path.join(ROOT, "models" if kind == "models" else "irs")
            files = sorted(f for f in os.listdir(d) if f.lower().endswith(ext)) if os.path.isdir(d) else []
            self._json({"files": files})
            return
        if self.path.split("?", 1)[0] == "/api/wifi/status":
            self._json(wifi_status())
            return
        if self.path.split("?", 1)[0] == "/api/wifi":
            self._json(wifi_config())
            return
        if self.path.startswith("/api/t3k/status"):
            cfg = t3k_cfg()
            self._json({"configured": bool(cfg.get("client_id")),
                        "connected": bool(cfg.get("tokens"))})
            return
        if self.path.startswith("/api/t3k/tone"):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            try:
                tone_id = int(q.get("tone_id", ["0"])[0])
                tone = t3k_get("/api/v1/tones/%d?architecture=2" % tone_id)
                models = t3k_get("/api/v1/models?tone_id=%d&architecture=2" % tone_id)
                self._json({"ok": True, "tone": tone, "models": models.get("data", [])})
            except Exception as exc:
                self._json({"error": str(exc)}, 500)
            return
        if self.path.startswith("/t3k/start"):
            cfg = t3k_cfg()
            if not cfg.get("client_id"):
                self._json({"error": "client_id mancante in config/tone3000.json"}, 500)
                return
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            gear = (q.get("gear", ["amp"])[0] or "amp").lower()
            host_hdr = self.headers.get("Host", "")
            host, _, port = host_hdr.partition(":")
            port = port or "80"
            h = host.strip("[]").lower()
            ok = h in ("localhost", "127.0.0.1", "::1") or h.endswith(".local")
            if not ok:
                try:
                    ok = ipaddress.ip_address(h).version == 4
                except ValueError:
                    ok = False
            if not ok:
                # TONE3000 accetta http solo per IP letterale/.local: se l'utente
                # ha aperto il Pi per nome, si usa l'IP risolto.
                h = _lan_ip()
            redirect_uri = "http://%s:%s/t3k/callback" % (h, port)
            verifier = _b64u(secrets.token_bytes(32))
            challenge = _b64u(hashlib.sha256(verifier.encode()).digest())
            state = _b64u(secrets.token_bytes(16))
            _pending[state] = {"verifier": verifier, "redirect_uri": redirect_uri}
            params = {
                "client_id": cfg["client_id"],
                "redirect_uri": redirect_uri,
                "response_type": "code",
                "code_challenge": challenge,
                "code_challenge_method": "S256",
                "state": state,
                "prompt": "select_tone",
                "architecture": "2",
                "menubar": "true",
                "gears": "ir" if gear == "ir" else "amp_amp-cab",
            }
            if gear != "ir":
                params["format"] = "nam"
            self._redirect(T3K_API + "/api/v1/oauth/authorize?" + urllib.parse.urlencode(params))
            return
        if self.path.startswith("/t3k/callback"):
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            state = q.get("state", [""])[0]
            code = q.get("code", [""])[0]
            err = q.get("error", [""])[0]
            tone_id = q.get("tone_id", [""])[0]
            pending = _pending.pop(state, None)
            if err or not code or not pending:
                self._redirect("/settings.html?t3k=errore")
                return
            cfg = t3k_cfg()
            body = urllib.parse.urlencode({
                "grant_type": "authorization_code",
                "code": code,
                "code_verifier": pending["verifier"],
                "redirect_uri": pending["redirect_uri"],
                "client_id": cfg["client_id"],
            }).encode()
            try:
                _, raw, _ = _http("POST", T3K_API + "/api/v1/oauth/token", body,
                                  {"Content-Type": "application/x-www-form-urlencoded"})
                d = json.loads(raw)
                cfg["tokens"] = {"access_token": d["access_token"],
                                 "refresh_token": d.get("refresh_token", ""),
                                 "expires_at": time.time() + float(d.get("expires_in", 3600))}
                t3k_save(cfg)
            except Exception:
                self._redirect("/settings.html?t3k=errore")
                return
            self._redirect("/t3k.html?tone_id=%s" % tone_id if tone_id else "/settings.html?t3k=ok")
            return
        return super().do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(n) if n else b""
        try:
            if self.path.startswith("/api/cmd"):
                self._json(ctl(json.loads(body)))
                return
            if self.path.split("?", 1)[0] == "/api/audio":
                try:
                    data = json.loads(body or b"{}")
                except ValueError:
                    self._json({"error": "JSON non valido"}, 400)
                    return
                try:
                    cfg = _validate_audio(data, audio_config())
                except ValueError as exc:
                    self._json({"error": str(exc)}, 400)
                    return
                try:
                    _write_audio(cfg)
                except Exception as exc:
                    self._json({"error": "salvataggio presets.json: %s" % exc}, 500)
                    return
                result = _audio_reload(cfg)
                self._json({"ok": True, "audio": cfg, "rate": AUDIO_RATE,
                            **result})
                return
            if self.path.startswith("/api/save"):
                self._json(ctl({"cmd": "save"}))
                return
            if self.path.startswith("/api/t3k/import"):
                data = json.loads(body)
                model_id = int(data.get("model_id", 0))
                if not model_id:
                    self._json({"error": "model_id mancante"}, 400)
                    return
                model = t3k_get("/api/v1/models/%d" % model_id)
                url = model.get("model_url", "")
                if not url.startswith("https://"):
                    self._json({"error": "model_url non valido"}, 400)
                    return
                tok = t3k_token()
                _, blob, _ = _http("GET", url, headers={"Authorization": "Bearer " + tok})
                if len(blob) > 100 * 1024 * 1024:
                    self._json({"error": "file troppo grande"}, 400)
                    return
                storage = urllib.parse.urlparse(url).path.rsplit("/", 1)[-1]
                ext = "." + storage.rsplit(".", 1)[-1] if "." in storage else ""
                kind = "irs" if (str(model.get("format", "")).lower() == "wav"
                                 or ext.lower() == ".wav") else "models"
                if kind == "models" and ext.lower() != ".nam":
                    ext = ".nam"
                raw_name = model.get("name") or ("t3k-%d" % model_id)
                name = "".join(c if c.isalnum() or c in "-_." else "-" for c in raw_name)
                name = name.strip("-.") + ext
                d = os.path.join(ROOT, kind)
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, name), "wb") as f:
                    f.write(blob)
                self._json({"ok": True, "name": name, "kind": kind})
                return
            if self.path.startswith("/api/t3k/disconnect"):
                cfg = t3k_cfg()
                cfg["tokens"] = None
                t3k_save(cfg)
                self._json({"ok": True})
                return
            if self.path.split("?", 1)[0] == "/api/wifi":
                data = json.loads(body)
                current = wifi_config()
                clean = _clean_networks(data.get("networks",
                                                 current["networks"]))
                ap = current["ap"]
                if "ap" in data:
                    try:
                        ap = _clean_ap(data["ap"], current["ap"])
                    except ValueError as exc:
                        self._json({"error": str(exc)}, 400)
                        return
                cfg = {"networks": clean, "ap": ap}
                _write_wifi(cfg)
                self._json({"ok": True, "networks": clean, "ap": ap})
                return
            if self.path.startswith("/api/upload"):
                name = os.path.basename(self.headers.get("X-Filename", ""))
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                kind = q.get("kind", ["models"])[0]
                ext = ".nam" if kind == "models" else ".wav"
                if not name or ".." in name or not name.lower().endswith(ext):
                    self._json({"error": "serve un file %s" % ext}, 400)
                    return
                d = os.path.join(ROOT, "models" if kind == "models" else "irs")
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, name), "wb") as f:
                    f.write(body)
                self._json({"ok": True, "name": name})
                return
        except Exception as exc:
            self._json({"error": str(exc)}, 500)
            return
        self._json({"error": "unknown"}, 404)


class Server(http.server.ThreadingHTTPServer):
    allow_reuse_address = True


if __name__ == "__main__":
    print("namzero-web su http://0.0.0.0:%d" % PORT, flush=True)
    Server(("0.0.0.0", PORT), Handler).serve_forever()
