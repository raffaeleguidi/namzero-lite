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
import secrets
import socket
import time
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
CTL = "/tmp/namzero-lite-ctl.sock"
WIFI = os.path.join(ROOT, "config/wifi.json")
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

def ctl(payload):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2.5)
    s.connect(CTL)
    f = s.makefile("rwb")
    if payload is not None:
        f.write((json.dumps(payload) + "\n").encode())
        f.flush()
    f.write(b'{"cmd":"state"}\n')
    f.flush()
    line = f.readline()
    s.close()
    if not line:
        raise RuntimeError("motore senza risposta")
    return json.loads(line)


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
    req = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
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

    def do_GET(self):
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
        if self.path.startswith("/api/wifi"):
            try:
                with open(WIFI) as f:
                    self._json(json.load(f))
            except Exception:
                self._json({"networks": []})
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
            if self.path.startswith("/api/wifi"):
                data = json.loads(body)
                nets = data.get("networks", [])
                clean = []
                for net in nets:
                    ssid = str(net.get("ssid", "")).strip()
                    if not ssid:
                        continue
                    clean.append({"ssid": ssid,
                                  "password": str(net.get("password", "")),
                                  "priority": int(net.get("priority", 0))})
                with open(WIFI, "w") as f:
                    json.dump({"networks": clean}, f, indent=2)
                os.chmod(WIFI, 0o600)
                self._json({"ok": True, "networks": clean})
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
