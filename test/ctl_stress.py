#!/usr/bin/env python3
"""Stress test del socket di controllo di namzero-lite (runtime, sul Pi).

Non fa parte della suite ctest: parla con il motore vero via
/tmp/namzero-lite-ctl.sock e, per il test web, con http://127.0.0.1:8080.

Uso:
    python3 test/ctl_stress.py [--repo ~/namzero-lite] [--slots 20]

Il motore deve essere avviato **in slot mode** (senza `-p`), come fa il
servizio: il test cambia slot e alla fine torna a quello iniziale. Se lo si e'
avviato con `-p file`, lo slot iniziale non corrisponde al preset caricato e il
ripristino finale non puo' funzionare.

Verifica:
  a) 16 connessioni concorrenti che leggono lo stato (64 richieste);
  b) 20 cambi preset consecutivi (comando slot + state);
  c) poll di /api/state dal web durante i cambi;
  c2) FILTER: solo wah, mix/posizione, defaults, auto-engage reale con il CC 7
      iniettato sulla socket BLE del motore, save e reload del preset;
  d) comandi di gruppo: param, save, save_options, looper, file, defaults;
     drive del fuzz: amount/tone, defaults, save/reload;
     looper.autostart: OFF -> rec immediato, ON -> attesa "armed" senza
     registrare, cancel/stop dall'attesa, soglia con clamp, save_options e
     reload che rilegge il blocco;
  e) EQ globale: band live, clamp, save_options, riavvio che la riapplica,
     defaults; volume globale live, clamp, persistenza, somma col boost;
  f) timeout dei client inattivi (~10 s);
  g) ripristino del preset/presets.json originali e stato finale;
  h) accordatore: on/mute, clamp riferimento/transpose, riferimento effettivo,
     save_options, riavvio che rilegge il blocco tuner (on/mute non persistiti);
  i) SSE /api/events: primo evento, evento live, chiusura pulita, seconda
     connessione e poll /api/state come fallback.

Esce 0 solo se tutti i controlli passano.
"""
import argparse
import hashlib
import http.client
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

CTL = "/tmp/namzero-lite-ctl.sock"
BLE = "/tmp/namzero-lite-ble.sock"
WEB = "http://127.0.0.1:8080/api/state"

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


def close_rel(a, b, tol=2e-3):
    """Confronto relativo (i master_gain arrivano da float C++)."""
    return abs(a - b) <= tol * max(abs(a), abs(b), 1e-9)


def connect(timeout=2.5, attempts=8):
    last = None
    for _ in range(attempts):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        try:
            s.connect(CTL)
            return s
        except OSError as exc:
            s.close()
            last = exc
            time.sleep(0.15)
    raise last


def read_state(timeout=2.5):
    """Apre una connessione, chiede lo stato e legge l'ultima riga."""
    s = connect()
    try:
        f = s.makefile("rwb")
        f.write(b'{"cmd":"state"}\n')
        f.flush()
        s.settimeout(timeout)
        line = f.readline()
        # Come la web UI: il motore manda anche gli snapshot in broadcast, si
        # legge l'ultima riga per un attimo e si chiude subito.
        s.settimeout(0.03)
        while True:
            try:
                nxt = f.readline()
            except (socket.timeout, OSError):
                break
            if not nxt:
                break
            line = nxt
        if not line:
            raise RuntimeError("nessuna riga di stato")
        return json.loads(line)
    finally:
        s.close()


def wait_state(pred, timeout=8.0, label="stato"):
    """Rilegge lo stato finche' pred(stato) e' vero (il motore fa re-exec)."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        try:
            st = read_state()
            last = st
            if pred(st):
                return st
        except Exception:
            pass
        time.sleep(0.1)
    raise RuntimeError("timeout in attesa di %s (ultimo stato: %s)" %
                       (label, json.dumps(last)[:200] if last else "nessuno"))


def send_cmd(payload, read_reply=True):
    """Manda un comando; la risposta e' best effort (il re-exec puo' chiudere)."""
    s = connect()
    try:
        f = s.makefile("rwb")
        f.write((json.dumps(payload) + "\n").encode())
        f.flush()
        if not read_reply:
            return None
        s.settimeout(2.5)
        line = f.readline()
        return json.loads(line) if line else None
    except (socket.timeout, OSError, ValueError):
        return None
    finally:
        s.close()


def slot_file(pj, target):
    f = pj["slots"][target]
    return f if f.startswith("/") else "config/" + f


def socket_sig():
    try:
        st = os.stat(CTL)
        return (st.st_ino, st.st_ctime_ns)
    except OSError:
        return None


def wait_reexec(payload, pred, label, timeout=15, attempts=2):
    """Manda un comando che fa re-exec e aspetta socket nuovo + stato atteso.

    Il solo stato non basta: dopo il comando il processo vecchio pubblica
    subito il nuovo cfg (per esempio il file appena scelto) prima di fare exec.
    La socket invece la ricrea solo il processo nuovo, quindi il suo inode
    cambia: quello e' il segnale che il preset e' stato davvero ricaricato.
    """
    for _ in range(attempts):
        before = socket_sig()
        try:
            send_cmd(payload, read_reply=False)
        except Exception:
            pass
        deadline = time.time() + timeout
        while time.time() < deadline:
            sig = socket_sig()
            if sig is not None and sig != before:
                try:
                    st = read_state()
                    if pred(st):
                        return st
                except Exception:
                    pass
            time.sleep(0.1)
        print("  %s: comando perso o reload lento, riprovo" % label, flush=True)
    raise RuntimeError("timeout in attesa di %s" % label)


def change_slot(pj, target):
    expected = slot_file(pj, target)
    return wait_reexec({"cmd": "slot", "n": target},
                       lambda s: s.get("slot") == target and s.get("preset") == expected,
                       "slot %d" % target, timeout=8)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def web_get():
    try:
        with urllib.request.urlopen(WEB, timeout=3) as r:
            body = r.read()
        return r.status, json.loads(body)
    except urllib.error.HTTPError as exc:
        return exc.code, None
    except Exception as exc:  # timeout, connessione rifiutata...
        return -1, str(exc)


# ── SSE ─────────────────────────────────────────────────────────────────────
# oggetto response di http.client: si leggono le righe finche' non arriva un
# "data: <json>" oppure scade il timeout.
def sse_open(timeout=6.0):
    conn = http.client.HTTPConnection("127.0.0.1", 8080, timeout=timeout)
    conn.request("GET", "/api/events")
    resp = conn.getresponse()
    return conn, resp


def sse_read_data(resp, timeout=6.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            line = resp.readline()
        except (socket.timeout, OSError):
            return None
        if not line:
            return None
        line = line.decode("utf-8", "replace").strip()
        if not line.startswith("data:"):
            continue
        try:
            return json.loads(line[5:].strip())
        except ValueError:
            continue
    return None


# ── iniezione MIDI per l'auto-engage ────────────────────────────────────────
# Il motore ascolta i byte MIDI grezzi sulla socket BLE (una connessione alla
# volta). Il bridge BLE normalmente la occupa: per il test lo si ferma per
# qualche secondo, si manda il CC dell'espressione e si lascia che systemd lo
# rilanci. Niente sudo: il processo e' dello stesso utente.

def ble_pids():
    try:
        out = subprocess.run(["pgrep", "-f", "ble-bridge-py/main.py"],
                             capture_output=True, text=True, timeout=5).stdout
    except Exception:
        return []
    pids = []
    for tok in out.split():
        try:
            pids.append(int(tok))
        except ValueError:
            pass
    return pids


def stop_ble_bridge():
    pids = ble_pids()
    for pid in pids:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass
    return pids


def wait_ble_bridge(timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ble_pids():
            return True
        time.sleep(0.2)
    return False


def ble_connect(timeout=6.0):
    last = None
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(2.5)
        try:
            s.connect(BLE)
            return s
        except OSError as exc:
            s.close()
            last = exc
            time.sleep(0.1)
    raise last if last is not None else RuntimeError("socket BLE assente")


def ble_cc(sock, cc, value, repeat=3):
    for _ in range(repeat):
        sock.sendall(bytes([0xB0, cc & 0x7F, value & 0x7F]))
        time.sleep(0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.expanduser("~/namzero-lite"))
    ap.add_argument("--slots", type=int, default=20)
    args = ap.parse_args()
    repo = os.path.abspath(os.path.expanduser(args.repo))
    presets_json = os.path.join(repo, "config/presets.json")

    print("== repo %s" % repo, flush=True)
    st = read_state()
    initial_slot = st["slot"]
    initial_preset = st["preset"]
    initial_amp = st["slots"]["amp"]["file"]
    print("stato iniziale: slot=%s preset=%s amp=%s" %
          (initial_slot, initial_preset, initial_amp), flush=True)

    # Backup di cio' che i test di scrittura toccano davvero.
    backup_dir = tempfile.mkdtemp(prefix="namzero-ctl-test-")
    backup_preset = None
    if initial_preset and os.path.exists(os.path.join(repo, initial_preset)):
        backup_preset = os.path.join(backup_dir, os.path.basename(initial_preset))
        shutil.copy2(os.path.join(repo, initial_preset), backup_preset)
    backup_presets_json = os.path.join(backup_dir, "presets.json")
    shutil.copy2(presets_json, backup_presets_json)
    last_slot_path = os.path.join(repo, "config/.last_slot")
    backup_last_slot = None
    if os.path.exists(last_slot_path):
        backup_last_slot = os.path.join(backup_dir, ".last_slot")
        shutil.copy2(last_slot_path, backup_last_slot)
    sum_before = (sha(presets_json), sha(backup_preset) if backup_preset else None)

    restored = False

    def restore_config():
        """Rimette i file vivi com'erano (anche se un test si interrompe)."""
        nonlocal restored
        if backup_preset:
            shutil.copy2(backup_preset, os.path.join(repo, initial_preset))
        shutil.copy2(backup_presets_json, presets_json)
        if backup_last_slot:
            shutil.copy2(backup_last_slot, last_slot_path)
        restored = True

    try:
        # ── (a) connessioni concorrenti ─────────────────────────────────────
        n_clients, per_client = 16, 4
        results = []
        errors = []
        barrier = threading.Barrier(n_clients)

        def worker(idx):
            try:
                barrier.wait(timeout=10)
                for _ in range(per_client):
                    t0 = time.time()
                    stt = read_state(timeout=4)
                    results.append(time.time() - t0)
                    if "slots" not in stt:
                        errors.append("client %d: stato senza slots" % idx)
            except Exception as exc:
                errors.append("client %d: %r" % (idx, exc))

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_clients)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(30)
        check("(a) %d richieste concorrenti da %d client" % (n_clients * per_client, n_clients),
              len(results) == n_clients * per_client and not errors,
              "%d ok, %.0f ms max, %d errori" %
              (len(results), (max(results) if results else 0) * 1000, len(errors)))
        if errors:
            print("  errori: %s" % errors[:5], flush=True)

        # ── (b) cambi preset + (c) poll web durante i cambi ─────────────────
        pj = json.load(open(presets_json))
        available = [i for i, f in enumerate(pj.get("slots", []))
                     if isinstance(f, str) and f and os.path.exists(os.path.join(repo, "config", f))]
        check("preset disponibili per il test", len(available) >= 2, str(available))
        if len(available) < 2:
            raise RuntimeError("servono almeno 2 slot configurati")

        web_stats = {"ok": 0, "503": 0, "err": 0}
        web_stop = threading.Event()

        def web_poller():
            while not web_stop.is_set():
                code, _ = web_get()
                if code == 200:
                    web_stats["ok"] += 1
                elif code == 503:
                    web_stats["503"] += 1
                else:
                    web_stats["err"] += 1
                time.sleep(0.05)

        wp = threading.Thread(target=web_poller, daemon=True)
        wp.start()
        lat = []
        slot_errors = []
        for i in range(args.slots):
            target = available[i % len(available)]
            t0 = time.time()
            try:
                change_slot(pj, target)
                lat.append(time.time() - t0)
            except Exception as exc:
                slot_errors.append("cambio %d -> slot %d: %r" % (i, target, exc))
        web_stop.set()
        wp.join(5)
        check("(b) %d cambi preset consecutivi" % args.slots,
              len(lat) == args.slots and not slot_errors,
              "%d ok, media %.0f ms, max %.0f ms" %
              (len(lat), (sum(lat) / len(lat) * 1000) if lat else 0,
               (max(lat) * 1000) if lat else 0))
        if slot_errors:
            print("  errori: %s" % slot_errors[:5], flush=True)
        check("(c) poll web /api/state durante i cambi",
              web_stats["ok"] > 0 and web_stats["503"] <= max(2, web_stats["ok"] // 10),
              "ok=%d 503=%d err=%d" % (web_stats["ok"], web_stats["503"], web_stats["err"]))
        check("motore vivo dopo i cambi", read_state() is not None)

        # Torna allo slot iniziale: i test di scrittura lavorano sul preset
        # dell'utente e vanno ripristinati alla fine.
        change_slot(pj, initial_slot)

        # ── (c2) FILTER: solo wah, mix, auto-engage reale ──────────────────
        # Il filter e' comandato dall'espressione: oltre ai comandi ctl si
        # inietta il CC 7 sulla socket BLE del motore (il bridge viene fermato
        # per pochi secondi e systemd lo rilancia).
        st = read_state()
        has_filter = "filter" in st.get("slots", {})
        check("filter presente nello stato", has_filter)
        if has_filter:
            check("filter senza campi del whammy",
                  "type" not in st["slots"]["filter"] and
                  "range" not in st["slots"]["filter"] and
                  "ratio" not in st["slots"]["filter"])

            send_cmd({"cmd": "param", "slot": "filter", "key": "mix", "value": 0.8})
            st = wait_state(lambda s: abs(s["slots"]["filter"]["mix"] - 0.8) < 1e-6,
                            timeout=4, label="filter mix")
            check("filter.mix (param)", abs(st["slots"]["filter"]["mix"] - 0.8) < 1e-6)

            # Auto-engage con CC 7 iniettato: si prova prima il tacco, poi la
            # punta (wah_invert nella config viva puo' spostare la zona attiva).
            sock = None
            engaged = False
            engage_val = None
            try:
                stop_ble_bridge()
                sock = ble_connect()
                for cand in (0, 127):
                    ble_cc(sock, 7, cand)
                    try:
                        st = wait_state(lambda s: s["slots"]["filter"]["on"] is True,
                                        timeout=2.5, label="auto-engage on (CC%d)" % cand)
                        engaged = True
                        engage_val = cand
                        break
                    except RuntimeError:
                        continue
                check("auto-engage accende il filter (CC7 iniettato)", engaged,
                      "CC %s" % engage_val)

                if engaged:
                    send_cmd({"cmd": "param", "slot": "filter", "key": "position", "value": 1})
                    st = wait_state(lambda s: s["slots"]["filter"]["position"] > 0.99,
                                    timeout=3, label="filter pos 1")
                    check("filter.position 1 (pedale a fondo)",
                          st["slots"]["filter"]["position"] > 0.99)
                    check("filter resta acceso dall'auto-engage",
                          st["slots"]["filter"]["on"] is True)

                    # Rilascio: il CC opposto porta l'attivazione sotto soglia.
                    ble_cc(sock, 7, 127 if engage_val == 0 else 0)
                    st = wait_state(lambda s: s["slots"]["filter"]["on"] is False,
                                    timeout=4, label="auto-engage off")
                    check("auto-engage spegne il filter", st["slots"]["filter"]["on"] is False)
            except Exception as exc:
                check("auto-engage via BLE", False, repr(exc))
            finally:
                if sock is not None:
                    try:
                        sock.close()
                    except OSError:
                        pass
                # Il bridge viene rilanciato da systemd (Restart=always).
                if not wait_ble_bridge(12):
                    print("  nota: bridge BLE non ancora ripartito", flush=True)

            # Save + reload: il mix deve restare nel preset, e il filter deve
            # tornare spento (on e' sempre forzato a false).
            send_cmd({"cmd": "param", "slot": "filter", "key": "mix", "value": 0.8})
            wait_state(lambda s: abs(s["slots"]["filter"]["mix"] - 0.8) < 1e-6,
                       timeout=4, label="filter pre-save")
            send_cmd({"cmd": "save"})
            time.sleep(0.25)
            st = wait_reexec({"cmd": "reload"},
                             lambda s: abs(s["slots"]["filter"]["mix"] - 0.8) < 1e-6 and
                             s["slots"]["filter"]["on"] is False,
                             "filter wah dopo il reload", timeout=20)
            check("filter mix salvato e ricaricato",
                  abs(st["slots"]["filter"]["mix"] - 0.8) < 1e-6)
            check("filter spento dopo il reload", st["slots"]["filter"]["on"] is False)

            # defaults: mix 1.
            send_cmd({"cmd": "defaults", "slot": "filter"})
            st = wait_state(lambda s: abs(s["slots"]["filter"]["mix"] - 1) < 1e-6,
                            timeout=4, label="defaults filter")
            check("defaults filter (mix 1)", abs(st["slots"]["filter"]["mix"] - 1) < 1e-6)

        # ── (d) comandi di gruppo ───────────────────────────────────────────
        send_cmd({"cmd": "param", "slot": "gate", "key": "threshold", "value": -55.5})
        st = wait_state(lambda s: abs(s["slots"]["gate"]["threshold"] + 55.5) < 0.01,
                        label="param gate")
        check("param gate.threshold", st["slots"]["gate"]["threshold"] == -55.5)

        send_cmd({"cmd": "defaults", "slot": "gate"})
        st = wait_state(lambda s: abs(s["slots"]["gate"]["threshold"] - (-55.0)) < 0.01,
                        timeout=4, label="defaults gate")
        check("defaults gate", st["slots"]["gate"]["threshold"] == -55.0)

        # ── drive del fuzz: amount/tone, defaults, save/reload ──────────────
        send_cmd({"cmd": "set", "slot": "drive", "type": "fuzz"})
        send_cmd({"cmd": "param", "slot": "drive", "key": "amount", "value": 0.60})
        send_cmd({"cmd": "param", "slot": "drive", "key": "tone", "value": 0.30})
        st = wait_state(lambda s: s["slots"]["drive"].get("type") == "fuzz" and
                        abs(s["slots"]["drive"].get("amount", -1) - 0.60) < 1e-6 and
                        abs(s["slots"]["drive"].get("tone", -1) - 0.30) < 1e-6,
                        timeout=4, label="param drive amount/tone")
        check("param drive.amount/tone",
              abs(st["slots"]["drive"]["amount"] - 0.60) < 1e-6 and
              abs(st["slots"]["drive"]["tone"] - 0.30) < 1e-6)
        check("drive senza campi dell'ottava",
              "octave" not in st["slots"]["drive"] and
              "octave_on" not in st["slots"]["drive"])

        if backup_preset:
            send_cmd({"cmd": "save"})
            time.sleep(0.25)
            dp = json.load(open(os.path.join(repo, initial_preset)))["slots"]["drive"]
            check("save scrive drive senza ottava",
                  dp.get("type") == "fuzz" and
                  abs(float(dp.get("amount", -1)) - 0.60) < 1e-6 and
                  "octave" not in dp and "octave_on" not in dp,
                  json.dumps(dp))

            st = wait_reexec({"cmd": "reload"},
                             lambda s: s["slots"]["drive"].get("type") == "fuzz" and
                             abs(s["slots"]["drive"].get("amount", -1) - 0.60) < 1e-6,
                             "reload drive", timeout=20)
            check("reload conserva drive.amount senza campi ottava",
                  abs(st["slots"]["drive"]["amount"] - 0.60) < 1e-6 and
                  "octave" not in st["slots"]["drive"] and
                  "octave_on" not in st["slots"]["drive"])
        else:
            check("save/reload drive", True, "nessun preset da salvare")

        send_cmd({"cmd": "defaults", "slot": "drive"})
        st = wait_state(lambda s: s["slots"]["drive"].get("type") == "fuzz" and
                        abs(s["slots"]["drive"].get("amount", -1) - 0.15) < 1e-6 and
                        abs(s["slots"]["drive"].get("tone", -1) - 0.5) < 1e-6,
                        timeout=4, label="defaults drive")
        check("defaults drive (type fuzz, amount 0.15, tone 0.5, senza ottava)",
              st["slots"]["drive"]["type"] == "fuzz" and
              abs(st["slots"]["drive"]["amount"] - 0.15) < 1e-6 and
              abs(st["slots"]["drive"]["tone"] - 0.5) < 1e-6 and
              "octave" not in st["slots"]["drive"] and
              "octave_on" not in st["slots"]["drive"])

        # ── looper: autostart, attesa, soglia, persistenza ──────────────────
        st = read_state()
        lp0 = st.get("looper")
        check("stato looper presente (autostart/soglia)",
              isinstance(lp0, dict) and "autostart" in lp0 and
              "threshold_db" in lp0, json.dumps(lp0))

        # Config nota: autostart OFF e soglia alta (nessun ingresso la supera:
        # il test non dipende da cosa arriva sull'XTONE).
        send_cmd({"cmd": "looper", "autostart": False,
                  "threshold_db": -6})
        st = wait_state(lambda s: s["looper"].get("autostart") is False and
                        abs(float(s["looper"].get("threshold_db", 0)) + 6) < 0.01,
                        timeout=4, label="looper config iniziale")
        check("looper autostart OFF e soglia -6 live",
              st["looper"]["autostart"] is False and
              abs(float(st["looper"]["threshold_db"]) + 6) < 0.01)

        # Loop vuoto: il toggle da fermo deve registrare subito (come prima).
        send_cmd({"cmd": "looper", "action": "clear"})
        st = wait_state(lambda s: s["looper"]["state"] == "idle" and
                        float(s["looper"]["len"]) == 0,
                        timeout=4, label="looper clear")
        check("looper clear da fermo", st["looper"]["state"] == "idle" and
              float(st["looper"]["len"]) == 0)
        send_cmd({"cmd": "looper", "action": "toggle"})
        st = wait_state(lambda s: s["looper"]["state"] == "rec", timeout=4,
                        label="looper rec (autostart off)")
        check("looper autostart OFF -> rec immediato",
              st["looper"]["state"] == "rec", st["looper"]["state"])
        send_cmd({"cmd": "looper", "action": "stop"})
        st = wait_state(lambda s: s["looper"]["state"] == "idle", timeout=4,
                        label="looper stop")
        check("looper stop finalizza e torna idle",
              st["looper"]["state"] == "idle" and float(st["looper"]["len"]) > 0,
              "len=%.2f" % float(st["looper"]["len"]))
        send_cmd({"cmd": "looper", "action": "clear"})
        time.sleep(0.2)

        # Autostart ON: toggle da fermo -> attesa, e resta in attesa (nessuna
        # registrazione) finche' l'ingresso non supera la soglia.
        send_cmd({"cmd": "looper", "autostart": True,
                  "threshold_db": -6})
        wait_state(lambda s: s["looper"].get("autostart") is True, timeout=4,
                   label="looper autostart on")
        send_cmd({"cmd": "looper", "action": "toggle"})
        st = wait_state(lambda s: s["looper"]["state"] == "armed", timeout=4,
                        label="looper armed")
        check("looper autostart ON -> attesa (armed)",
              st["looper"]["state"] == "armed" and st["looper"]["armed"] is False)
        time.sleep(0.6)
        st = read_state()
        check("looper in attesa non registra (soglia -6)",
              st["looper"]["state"] == "armed" and
              float(st["looper"]["len"]) == 0, json.dumps(st["looper"]))

        # Toggle in attesa = annulla; stop e clear dall'attesa = idle.
        send_cmd({"cmd": "looper", "action": "toggle"})
        st = wait_state(lambda s: s["looper"]["state"] == "idle", timeout=4,
                        label="looper cancel")
        check("looper toggle in attesa annulla", st["looper"]["state"] == "idle" and
              float(st["looper"]["len"]) == 0)

        send_cmd({"cmd": "looper", "action": "toggle"})
        wait_state(lambda s: s["looper"]["state"] == "armed", timeout=4,
                   label="looper armed 2")
        send_cmd({"cmd": "looper", "action": "stop"})
        st = wait_state(lambda s: s["looper"]["state"] == "idle", timeout=4,
                        label="looper stop da armed")
        check("looper stop dall'attesa -> idle", st["looper"]["state"] == "idle")

        send_cmd({"cmd": "looper", "action": "toggle"})
        wait_state(lambda s: s["looper"]["state"] == "armed", timeout=4,
                   label="looper armed 3")
        send_cmd({"cmd": "looper", "action": "clear"})
        st = wait_state(lambda s: s["looper"]["state"] == "idle" and
                        float(s["looper"]["len"]) == 0,
                        timeout=4, label="looper clear da armed")
        check("looper clear dall'attesa -> idle", st["looper"]["state"] == "idle" and
              float(st["looper"]["len"]) == 0)

        # Clamp della soglia: il motore la tiene fra -90 e 0 dBFS.
        send_cmd({"cmd": "looper", "threshold_db": -999})
        st = wait_state(lambda s: abs(float(s["looper"]["threshold_db"]) + 90) < 0.01,
                        timeout=4, label="looper threshold clamp basso")
        check("looper threshold clamp a -90", st["looper"]["threshold_db"] == -90.0)
        send_cmd({"cmd": "looper", "threshold_db": 99})
        st = wait_state(lambda s: abs(float(s["looper"]["threshold_db"])) < 0.01,
                        timeout=4, label="looper threshold clamp alto")
        check("looper threshold clamp a 0", st["looper"]["threshold_db"] == 0.0)

        # save_options persiste il blocco looper; reload lo rilegge all'avvio.
        send_cmd({"cmd": "looper", "autostart": True,
                  "threshold_db": -30})
        wait_state(lambda s: s["looper"].get("autostart") is True and
                   abs(float(s["looper"].get("threshold_db", 0)) + 30) < 0.01,
                   timeout=4, label="looper pre save_options")
        send_cmd({"cmd": "save_options"})
        time.sleep(0.25)
        lj = json.load(open(presets_json))
        check("save_options scrive il blocco looper",
              isinstance(lj.get("looper"), dict) and
              lj["looper"].get("autostart") is True and
              abs(float(lj["looper"].get("autostart_threshold_db", 0)) + 30) < 1e-6 and
              abs(float(lj["looper"].get("max_seconds", 0)) - 60.0) < 1e-6,
              json.dumps(lj.get("looper")))

        st = wait_reexec({"cmd": "reload"},
                         lambda s: s["looper"].get("autostart") is True and
                         abs(float(s["looper"].get("threshold_db", 0)) + 30) < 0.01,
                         "looper dopo il reload", timeout=20)
        check("looper autostart/soglia riletti dopo il reload",
              st["looper"]["autostart"] is True and
              abs(float(st["looper"]["threshold_db"]) + 30) < 0.01 and
              abs(float(st["looper"]["max"]) - 60.0) < 0.1,
              json.dumps(st["looper"]))

        # Riporta la config live ai default per il resto del test: la config
        # viva viene comunque ripristinata dal backup nel finally.
        send_cmd({"cmd": "looper", "autostart": False,
                  "threshold_db": -42})
        wait_state(lambda s: s["looper"].get("autostart") is False, timeout=4,
                   label="looper config finale")

        # save_options: scrive config/presets.json (ripristinato nel finally).
        send_cmd({"cmd": "save_options"})
        time.sleep(0.2)
        sj = json.load(open(presets_json))
        check("save_options scrive presets.json",
              all(k in sj for k in ("gate", "autoengage", "boost", "eq", "tuner", "looper")) and
              os.path.getsize(presets_json) > 50)

        # save: scrive il preset corrente (ripristinato nel finally).
        if backup_preset:
            send_cmd({"cmd": "save"})
            time.sleep(0.2)
            check("save riscrive il preset", os.path.getsize(os.path.join(repo, initial_preset)) > 0)

        # file amp: un modello valido diverso da quello corrente.
        models = sorted(f for f in os.listdir(os.path.join(repo, "models"))
                        if f.lower().endswith(".nam"))
        other = [m for m in models if m != os.path.basename(initial_amp)]
        if other:
            st = wait_reexec({"cmd": "file", "slot": "amp", "file": other[0]},
                             lambda s, m=other[0]: s["slots"]["amp"]["file"].endswith(m),
                             "file amp", timeout=20)
            check("file amp (validato + re-exec)", st["slots"]["amp"]["file"].endswith(other[0]),
                  st["slots"]["amp"]["file"])
        else:
            check("file amp", False, "nessun modello .nam alternativo")

        irs = sorted(f for f in os.listdir(os.path.join(repo, "irs"))
                     if f.lower().endswith(".wav"))
        if irs:
            before_ir = read_state()["slots"]["ir"]["file"]
            cand = [x for x in irs if not before_ir.endswith(x)]
            if cand:
                st = wait_reexec({"cmd": "file", "slot": "ir", "file": cand[0]},
                                 lambda s, m=cand[0]: s["slots"]["ir"]["file"].endswith(m),
                                 "file ir", timeout=20)
                check("file ir (validato + re-exec)",
                      st["slots"]["ir"]["file"].endswith(cand[0]), st["slots"]["ir"]["file"])

        # ── (g) EQ globale: live, persistenza, riavvio che la riapplica ─────
        send_cmd({"cmd": "eq", "band": 2, "gain": 6.5})
        st = wait_state(lambda s: "eq" in s and abs(s["eq"]["gains"][2] - 6.5) < 0.01,
                        timeout=4, label="eq band 2")
        check("eq band live", st["eq"]["gains"][2] == 6.5,
              "banda 2 = %s dB" % st["eq"]["gains"][2])

        # Il clamp -12..+12 e' nel motore: un comando fuori scala non lo sfonda.
        send_cmd({"cmd": "eq", "band": 2, "gain": 99})
        st = wait_state(lambda s: abs(s["eq"]["gains"][2] - 12.0) < 0.01,
                        timeout=4, label="eq clamp")
        check("eq clamp a +12 dB", st["eq"]["gains"][2] == 12.0)

        send_cmd({"cmd": "eq", "band": 2, "gain": 6.5})
        wait_state(lambda s: abs(s["eq"]["gains"][2] - 6.5) < 0.01,
                   timeout=4, label="eq band 2 di nuovo")
        send_cmd({"cmd": "eq", "on": False})
        st = wait_state(lambda s: s["eq"]["on"] is False, timeout=4, label="eq off")
        check("eq on/off live", st["eq"]["on"] is False)

        # Volume globale: vive nell'EQ ma si applica sempre. Live + clamp.
        send_cmd({"cmd": "eq", "volume": 9})
        st = wait_state(lambda s: "volume" in s.get("eq", {}) and
                        abs(s["eq"]["volume"] - 9.0) < 0.01,
                        timeout=4, label="eq volume 9")
        check("eq volume live", st["eq"]["volume"] == 9.0,
              "volume = %s dB" % st["eq"]["volume"])

        send_cmd({"cmd": "eq", "volume": 99})
        st = wait_state(lambda s: abs(s["eq"]["volume"] - 24.0) < 0.01,
                        timeout=4, label="eq volume clamp")
        check("eq volume clamp a +24 dB", st["eq"]["volume"] == 24.0)

        # Boost e volume si sommano: master_gain (lineare) li moltiplica. Qui
        # l'EQ e' spento: il volume deve agire lo stesso.
        send_cmd({"cmd": "boost", "on": False})
        wait_state(lambda s: s.get("boost") is False, timeout=4, label="boost off")
        send_cmd({"cmd": "eq", "volume": 0})
        st0 = wait_state(lambda s: abs(s["eq"]["volume"]) < 0.01,
                         timeout=4, label="volume 0")
        send_cmd({"cmd": "eq", "volume": 9})
        st9 = wait_state(lambda s: abs(s["eq"]["volume"] - 9.0) < 0.01,
                         timeout=4, label="volume 9")
        boost_db = float(st9.get("boost_db", 6.0))
        want_v = 10 ** (9.0 / 20.0)
        ratio_v = st9["master_gain"] / st0["master_gain"]
        check("volume +9 dB sul master (EQ spento)", close_rel(ratio_v, want_v),
              "ratio %.4f atteso %.4f" % (ratio_v, want_v))

        send_cmd({"cmd": "boost", "on": True})
        stb = wait_state(lambda s: s.get("boost") is True, timeout=4, label="boost on")
        want_b = 10 ** (boost_db / 20.0)
        ratio_b = stb["master_gain"] / st9["master_gain"]
        check("boost si somma al volume", close_rel(ratio_b, want_b),
              "ratio %.4f atteso %.4f (+%.1f dB)" % (ratio_b, want_b, boost_db))

        # save_options scrive il blocco eq nella config viva (backup/ripristino
        # piu' sotto): poi un riavvio deve rileggerlo, volume compreso.
        send_cmd({"cmd": "save_options"})
        time.sleep(0.25)
        ej = json.load(open(presets_json))
        check("save_options scrive eq in presets.json",
              isinstance(ej.get("eq"), dict) and ej["eq"].get("on") is False and
              len(ej["eq"].get("gains", [])) == 5 and
              abs(float(ej["eq"]["gains"][2]) - 6.5) < 1e-6 and
              abs(float(ej["eq"].get("volume_db", 0)) - 9.0) < 1e-6,
              json.dumps(ej.get("eq")))

        st = wait_reexec({"cmd": "reload"},
                         lambda s: (s.get("eq", {}).get("on") is False and
                                    abs(s["eq"]["gains"][2] - 6.5) < 0.01 and
                                    abs(s["eq"]["volume"] - 9.0) < 0.01),
                         "eq dopo il riavvio", timeout=20)
        check("eq riapplicata dopo il riavvio",
              st["eq"]["on"] is False and st["eq"]["gains"][2] == 6.5 and
              st["eq"]["volume"] == 9.0,
              "on=%s banda2=%s volume=%s" %
              (st["eq"]["on"], st["eq"]["gains"][2], st["eq"]["volume"]))

        send_cmd({"cmd": "defaults", "slot": "eq"})
        st = wait_state(lambda s: (s["eq"]["on"] is True and
                                   all(abs(g) < 1e-6 for g in s["eq"]["gains"]) and
                                   abs(s["eq"]["volume"] - 6.0) < 1e-6),
                        timeout=4, label="defaults eq")
        check("defaults eq (on, piatto, volume +6 dB)",
              st["eq"]["on"] is True and all(abs(g) < 1e-6 for g in st["eq"]["gains"]) and
              st["eq"]["volume"] == 6.0)

        # ── (h) accordatore: comandi, clamp, persistenza ─────────────────────
        st = read_state()
        check("stato tuner presente", isinstance(st.get("tuner"), dict),
              json.dumps(st.get("tuner")))
        t0 = st.get("tuner", {})
        check("tuner parte spento e non muto",
              t0.get("on") is False and t0.get("muted") is False,
              json.dumps(t0))
        check("tuner ha riferimento/transpose",
              isinstance(t0.get("reference_hz"), (int, float)) and
              isinstance(t0.get("transpose"), int), json.dumps(t0))

        send_cmd({"cmd": "tuner", "on": True})
        st = wait_state(lambda s: s["tuner"]["on"] is True, timeout=4, label="tuner on")
        check("tuner on", st["tuner"]["on"] is True)
        # Nessun ingresso audio: dopo ~0.5 s niente nota e confidenza zero.
        time.sleep(0.5)
        st = read_state()
        check("tuner senza segnale: nessuna nota",
              st["tuner"]["note"] == "" and st["tuner"]["freq"] == 0 and
              st["tuner"]["confidence"] == 0, json.dumps(st["tuner"]))

        send_cmd({"cmd": "tuner", "mute": True})
        st = wait_state(lambda s: s["tuner"]["muted"] is True, timeout=4,
                        label="tuner mute")
        check("tuner mute", st["tuner"]["muted"] is True)

        # Clamp del riferimento (il limite inferiore include 415.3 Hz).
        send_cmd({"cmd": "tuner", "reference_hz": 400})
        st = wait_state(lambda s: abs(s["tuner"]["reference_hz"] - 415.0) < 1e-6,
                        timeout=4, label="tuner ref clamp basso")
        check("tuner reference clamp a 415", st["tuner"]["reference_hz"] == 415.0)
        send_cmd({"cmd": "tuner", "reference_hz": 500})
        st = wait_state(lambda s: abs(s["tuner"]["reference_hz"] - 450.0) < 1e-6,
                        timeout=4, label="tuner ref clamp alto")
        check("tuner reference clamp a 450", st["tuner"]["reference_hz"] == 450.0)

        send_cmd({"cmd": "tuner", "transpose": 99})
        st = wait_state(lambda s: s["tuner"]["transpose"] == 6, timeout=4,
                        label="tuner transpose clamp alto")
        check("tuner transpose clamp a +6", st["tuner"]["transpose"] == 6)
        send_cmd({"cmd": "tuner", "transpose": -99})
        st = wait_state(lambda s: s["tuner"]["transpose"] == -6, timeout=4,
                        label="tuner transpose clamp basso")
        check("tuner transpose clamp a -6", st["tuner"]["transpose"] == -6)

        # transpose -1 con riferimento 440 -> riferimento effettivo 415.3.
        send_cmd({"cmd": "tuner", "reference_hz": 440.0, "transpose": -1})
        st = wait_state(lambda s: abs(s["tuner"]["reference_hz"] - 440.0) < 1e-6 and
                        s["tuner"]["transpose"] == -1, timeout=4,
                        label="tuner transpose -1")
        eff = st["tuner"].get("effective_reference_hz")
        check("transpose -1 con ref 440 -> riferimento effettivo 415.3",
              eff is not None and abs(eff - 440.0 * 2 ** (-1 / 12)) < 0.01,
              str(eff))

        # La stessa accordatura si ottiene scrivendo direttamente 415.3.
        send_cmd({"cmd": "tuner", "reference_hz": 415.3, "transpose": 0})
        st = wait_state(lambda s: abs(s["tuner"]["reference_hz"] - 415.3) < 1e-6 and
                        s["tuner"]["transpose"] == 0, timeout=4,
                        label="tuner ref 415.3")
        eff = st["tuner"].get("effective_reference_hz")
        check("reference 415.3 -> riferimento effettivo 415.3",
              eff is not None and abs(eff - 415.3) < 0.01, str(eff))

        # save_options scrive il blocco tuner; on/mute non sono persistiti.
        send_cmd({"cmd": "save_options"})
        time.sleep(0.25)
        tj = json.load(open(presets_json))
        check("save_options scrive il blocco tuner",
              isinstance(tj.get("tuner"), dict) and
              abs(float(tj["tuner"].get("reference_hz", 0)) - 415.3) < 1e-6 and
              int(tj["tuner"].get("transpose", 99)) == 0,
              json.dumps(tj.get("tuner")))

        send_cmd({"cmd": "tuner", "on": True, "mute": True})
        wait_state(lambda s: s["tuner"]["on"] is True and s["tuner"]["muted"] is True,
                   timeout=4, label="tuner on+mute pre reload")
        st = wait_reexec({"cmd": "reload"},
                         lambda s: abs(s["tuner"]["reference_hz"] - 415.3) < 1e-6 and
                         s["tuner"]["transpose"] == 0 and
                         s["tuner"]["on"] is False and s["tuner"]["muted"] is False,
                         "tuner dopo il riavvio", timeout=20)
        check("tuner reference/transpose riletti dopo il riavvio",
              abs(st["tuner"]["reference_hz"] - 415.3) < 1e-6 and
              st["tuner"]["transpose"] == 0, json.dumps(st["tuner"]))
        check("tuner on/mute non persistiti dopo il riavvio",
              st["tuner"]["on"] is False and st["tuner"]["muted"] is False)

        # ── ripristino config viva ──────────────────────────────────────────
        restore_config()

        def restored_ok(s):
            return (s.get("slot") == initial_slot and
                    s.get("preset") == initial_preset and
                    s["slots"]["amp"]["file"] == initial_amp)

        st = wait_reexec({"cmd": "reload"}, restored_ok, "ripristino preset", timeout=20)
        check("preset/presets.json ripristinati",
              sha(presets_json) == sum_before[0] and
              (not backup_preset or sha(os.path.join(repo, initial_preset)) == sum_before[1]),
              "slot=%s amp=%s" % (st["slot"], st["slots"]["amp"]["file"]))

        # ── (i) SSE /api/events + fallback poll ─────────────────────────────
        conn = resp = None
        try:
            conn, resp = sse_open()
            check("(i) SSE risponde 200", resp.status == 200, str(resp.status))
            ctype = resp.getheader("Content-Type", "")
            check("(i) SSE content-type text/event-stream",
                  "text/event-stream" in (ctype or ""), str(ctype))
            ev = sse_read_data(resp, timeout=6)
            check("(i) SSE primo evento con lo stato",
                  isinstance(ev, dict) and "slots" in ev)
            # Un comando cambiato da un'altra connessione deve arrivare subito
            # sul flusso SSE (render della modale a 10-20 Hz).
            send_cmd({"cmd": "tuner", "on": True})
            live = None
            for _ in range(8):
                ev = sse_read_data(resp, timeout=3)
                if ev and ev.get("tuner", {}).get("on") is True:
                    live = ev
                    break
            check("(i) SSE riceve l'evento live (tuner on)", live is not None)
        finally:
            if resp is not None:
                try:
                    resp.close()
                except Exception:
                    pass
            if conn is not None:
                try:
                    conn.close()
                except Exception:
                    pass
        time.sleep(0.2)
        code, pst = web_get()
        check("(i) poll /api/state dopo la chiusura SSE",
              code == 200 and isinstance(pst, dict) and "slots" in pst,
              "code=%s" % code)
        # Una seconda connessione SSE funziona: il server non e' rimasto appeso
        # e la chiusura del client e' stata gestita.
        conn2 = resp2 = None
        try:
            conn2, resp2 = sse_open()
            ev = sse_read_data(resp2, timeout=6)
            check("(i) seconda connessione SSE", isinstance(ev, dict) and "slots" in ev)
        finally:
            if resp2 is not None:
                try:
                    resp2.close()
                except Exception:
                    pass
            if conn2 is not None:
                try:
                    conn2.close()
                except Exception:
                    pass
        send_cmd({"cmd": "tuner", "on": False, "mute": False})

        # ── (e) client inattivo chiuso dopo ~10 s ──────────────────────────
        s = connect()
        s.settimeout(13)
        t0 = time.time()
        closed = False
        try:
            while time.time() - t0 < 13:
                data = s.recv(4096)
                if not data:
                    closed = True
                    break
        except socket.timeout:
            closed = False
        except OSError:
            closed = True
        s.close()
        elapsed = time.time() - t0
        check("(e) client inattivo chiuso dal motore", closed and 9.5 <= elapsed <= 12.5,
              "chiuso=%s dopo %.1f s" % (closed, elapsed))

        # ── (f) motore ancora raggiungibile e nessun client appeso ─────────
        st = read_state()
        check("motore raggiungibile a fine test", st is not None)
        print("== risultato: %d PASS, %d FAIL" % (ok_count, fail_count), flush=True)
        return 0 if fail_count == 0 else 1
    finally:
        if not restored:
            restore_config()
            try:
                send_cmd({"cmd": "reload"}, read_reply=False)
            except Exception:
                pass
        shutil.rmtree(backup_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("FAIL test interrotto: %r" % (exc,), flush=True)
        sys.exit(2)
