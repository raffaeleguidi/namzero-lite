#!/usr/bin/env python3
"""Test runtime dell'interfaccia audio configurabile (motore e web veri, sul Pi).

Verifica, senza spegnere l'audio degli altri servizi:

  - GET /api/audio e /api/audio/devices;
  - POST /api/audio invalido -> 400 e presets.json intatto;
  - POST /api/audio valido -> 200, blocco audio persistito, reload del motore
    e riapertura dello stesso device (o del fallback);
  - nessuna altra chiave di presets.json cambiata.

Uso (sul Pi, servizi gia' avviati):
    python3 test/audio_runtime.py [--repo ~/namzero-lite] [--url http://127.0.0.1:8080]

Alla fine, se qualcosa fallisce, ripristina il presets.json iniziale. Se tutto
passa lascia il blocco audio (con i valori iniziali) nella config viva.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ok_count = 0
fail_count = 0
failures = []


def check(name, cond, detail=""):
    global ok_count, fail_count
    if cond:
        ok_count += 1
        print("PASS %s%s" % (name, (" — " + detail) if detail else ""), flush=True)
    else:
        fail_count += 1
        failures.append("%s%s" % (name, (": " + detail) if detail else ""))
        print("FAIL %s%s" % (name, (" — " + detail) if detail else ""), flush=True)


class Api:
    def __init__(self, base):
        self.base = base.rstrip("/")

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=10) as resp:
            return resp.status, json.loads(resp.read())

    def post(self, path, payload, raw=None):
        body = raw if raw is not None else json.dumps(payload).encode()
        req = urllib.request.Request(self.base + path, data=body, method="POST",
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=20) as resp:
                return resp.status, json.loads(resp.read())
        except urllib.error.HTTPError as exc:
            return exc.code, json.loads(exc.read())


def read_json(path):
    with open(path) as f:
        return json.load(f)


def write_atomic(path, data):
    tmp = path + ".test-tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def file_hash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def wait_audio(api, expected, timeout=12.0):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            _, st = api.get("/api/state")
            last = st.get("audio")
            if (last and last.get("config_device") == expected.get("device")
                    and last.get("buffer") == expected.get("buffer")
                    and last.get("periods") == expected.get("periods")
                    and last.get("output") == expected.get("output", 0)
                    and "open" in last):
                return last
        except Exception:
            pass
        time.sleep(0.3)
    return last


def engine_print_config(repo, output):
    """Esegue --print-config (esce prima di aprire ALSA) per verificare il clamp.

    Gira in una directory vuota: con un config/presets.json presente il motore
    entrerebbe in slot mode e rifiuterebbe i flag (`--preset` + flag).
    """
    binary = os.path.abspath(os.path.join(repo, "build/namzero-lite"))
    if not os.path.isfile(binary):
        return None
    try:
        with tempfile.TemporaryDirectory() as tmp:
            return subprocess.run(
                [binary, "--print-config", "--output", str(output)],
                cwd=tmp, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as exc:
        print("NOTA: --print-config non eseguibile: %s" % exc, flush=True)
        return None


def without_audio(cfg):
    return {k: v for k, v in cfg.items() if k != "audio"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=os.path.expanduser("~/namzero-lite"))
    parser.add_argument("--url", default="http://127.0.0.1:8080")
    args = parser.parse_args()
    presets_path = os.path.join(args.repo, "config/presets.json")
    api = Api(args.url)

    if not os.path.exists(presets_path):
        print("manca %s" % presets_path)
        return 2
    with open(presets_path, "rb") as f:
        backup_bytes = f.read()
    backup = json.loads(backup_bytes)
    original_hash = hashlib.sha256(backup_bytes).hexdigest()

    try:
        # ── stato iniziale ────────────────────────────────────────────────
        status, audio = api.get("/api/audio")
        check("GET /api/audio risponde 200", status == 200)
        check("config audio completa", all(k in audio for k in
              ("device", "buffer", "periods", "input", "output", "rate",
               "inputs", "outputs", "status")),
              str(audio))
        check("rate fisso 48000", audio.get("rate") == 48000)
        check("ingressi disponibili", isinstance(audio.get("inputs"), list)
              and len(audio["inputs"]) >= 1, str(audio.get("inputs")))
        check("uscite disponibili", isinstance(audio.get("outputs"), list)
              and [o.get("index") for o in audio["outputs"]] == [0, 1, 2],
              str(audio.get("outputs")))
        check("output in 0..2", audio.get("output") in (0, 1, 2),
              str(audio.get("output")))
        device = audio["device"]
        check("device configurato non vuoto", bool(device), device)
        if device == "hw:XTONE":
            check("device corrente hw:XTONE", True)

        # Clamp del motore: --output fuori range non deve mai arrivare ad ALSA.
        proc = engine_print_config(args.repo, 3)
        if proc is not None:
            check("CLI --output 3 clampato a 2",
                  proc.returncode == 0 and "output=2" in proc.stdout,
                  (proc.stderr or proc.stdout)[-300:])
        proc = engine_print_config(args.repo, 0)
        if proc is not None:
            check("CLI --output 0 accettato",
                  proc.returncode == 0 and "output=0" in proc.stdout,
                  (proc.stderr or proc.stdout)[-300:])

        status, devs = api.get("/api/audio/devices")
        check("GET /api/audio/devices risponde 200", status == 200)
        check("elenco device non vuoto", bool(devs.get("devices")),
              str(devs.get("error", "")))
        check("default presente", any(d.get("pcm") == "default"
                                      for d in devs.get("devices", [])))
        check("device ha i campi attesi",
              all(all(k in d for k in ("card", "device", "name", "id", "pcm",
                                       "plughw", "label"))
                  for d in devs.get("devices", [])))
        ids = [d.get("id") for d in devs.get("devices", [])]
        if "XTONE" in ids:
            check("XTONE elencato da aplay -l", True)

        # ── POST invalidi: nessuna scrittura ──────────────────────────────
        invalid = [
            ({"device": ""}, "device vuoto"),
            ({"device": "hw: XTONE"}, "device con spazio"),
            ({"buffer": 15}, "buffer sotto 16"),
            ({"buffer": 2049}, "buffer sopra 2048"),
            ({"periods": 1}, "periodi sotto 2"),
            ({"periods": 9}, "periodi sopra 8"),
            ({"input": 2}, "input fuori range"),
            ({"input": "0"}, "input stringa"),
            ({"output": -1}, "output sotto 0"),
            ({"output": 3}, "output sopra 2"),
            ({"output": "0"}, "output stringa"),
            ({"output": 1.5}, "output non intero"),
        ]
        for payload, label in invalid:
            status, data = api.post("/api/audio", payload)
            check("POST invalido (%s) -> 400" % label, status == 400,
                  "status %s" % status)
            check("POST invalido (%s) ha error" % label, "error" in data)
        status, _ = api.post("/api/audio", None, raw=b"{non json")
        check("POST JSON rotto -> 400", status == 400)
        check("presets.json intatto dopo i POST invalidi",
              file_hash(presets_path) == original_hash)

        # ── POST valido: salva + reload + riapertura ──────────────────────
        target = {"device": device, "buffer": audio["buffer"],
                  "periods": audio["periods"], "input": audio["input"],
                  "output": audio["output"]}
        # Cambia SOLO il canale di uscita (device e buffer restano quelli in
        # uso): nessun rischio per l'audio oltre al re-exec necessario.
        target["output"] = 2 if target["output"] != 2 else 1
        status, resp = api.post("/api/audio", target)
        check("POST valido -> 200", status == 200, "status %s" % status)
        check("POST valido: reload confermato", resp.get("reloaded") is True,
              str(resp.get("warning", "")))
        saved = read_json(presets_path)
        check("blocco audio persistito", saved.get("audio") == target,
              str(saved.get("audio")))
        check("device invariato dopo il POST",
              saved.get("audio", {}).get("device") == device, str(saved.get("audio")))
        st = wait_audio(api, target)
        check("motore ripartito con la config nuova", st is not None, str(st))
        check("audio aperto", bool(st and st.get("open")),
              str(st.get("error", "") if st else "stato assente"))
        check("output applicato dal motore",
              bool(st and st.get("output") == target["output"]), str(st))
        if st and st.get("open"):
            actual = st.get("device")
            check("device riaperto (o fallback)", actual in (device, "default",
                                                              "hw:XTONE"),
                  "in uso %s, configurato %s" % (actual, device))
            if actual != device:
                print("NOTA: fallback attivo: configurato %s, in uso %s"
                      % (device, actual))
        check("altre chiavi di presets.json invariate",
              without_audio(saved) == without_audio(backup))

        # ── ripristino dei valori iniziali (blocco audio conservato) ──────
        status, resp = api.post("/api/audio", audio)
        check("ripristino config iniziale -> 200", status == 200)
        st = wait_audio(api, audio)
        check("motore tornato alla config iniziale", st is not None, str(st))
        check("audio aperto dopo il ripristino", bool(st and st.get("open")),
              str(st.get("error", "") if st else "stato assente"))
        restored = read_json(presets_path)
        check("config iniziale ripristinata",
              restored.get("audio") == {k: audio[k] for k in
                                        ("device", "buffer", "periods", "input",
                                         "output")})
        check("altre chiavi invariate dopo il ripristino",
              without_audio(restored) == without_audio(backup))
    finally:
        if fail_count:
            print("\nRipristino presets.json di backup (%d byte)" % len(backup_bytes),
                  flush=True)
            tmp = presets_path + ".restore"
            with open(tmp, "wb") as f:
                f.write(backup_bytes)
            os.replace(tmp, presets_path)
            try:
                api.post("/api/cmd", {"cmd": "reload"})
                time.sleep(1.0)
            except Exception:
                pass

    print("\n%d PASS, %d FAIL" % (ok_count, fail_count))
    for failure in failures:
        print("  - " + failure)
    return 0 if fail_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
