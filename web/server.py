#!/usr/bin/env python3
"""Servizio web locale per namzero-lite (nessuna dipendenza).

Serve la pagina e inoltra comandi/stato al socket di controllo del motore.
"""
import http.server
import json
import os
import socket
import urllib.parse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "web")
CTL = "/tmp/namzero-lite-ctl.sock"
PORT = int(os.environ.get("NAMZERO_WEB_PORT", "8080"))


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
            d = os.path.join(ROOT, "models" if kind == "models" else "irs")
            files = sorted(os.listdir(d)) if os.path.isdir(d) else []
            self._json({"files": files})
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
            if self.path.startswith("/api/upload"):
                name = os.path.basename(self.headers.get("X-Filename", ""))
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                kind = q.get("kind", ["models"])[0]
                if not name or ".." in name:
                    self._json({"error": "nome non valido"}, 400)
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
    print(f"namzero-web su http://0.0.0.0:{PORT}", flush=True)
    Server(("0.0.0.0", PORT), Handler).serve_forever()
