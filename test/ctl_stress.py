#!/usr/bin/env python3
"""Stress test del socket di controllo di namzero-lite (runtime, sul Pi).

Non fa parte della suite ctest: parla con il motore vero via
/tmp/namzero-lite-ctl.sock e, per il test web, con http://127.0.0.1:8080.

Uso:
    python3 test/ctl_stress.py [--repo ~/namzero-lite] [--slots 20]

Verifica:
  a) 16 connessioni concorrenti che leggono lo stato (64 richieste);
  b) 20 cambi preset consecutivi (comando slot + state);
  c) poll di /api/state dal web durante i cambi;
  d) comandi di gruppo: param, save, save_options, looper, file, defaults;
  e) EQ globale: band live, clamp, save_options, riavvio che la riapplica,
     defaults; volume globale live, clamp, persistenza, somma col boost;
  f) timeout dei client inattivi (~10 s);
  g) ripristino del preset/presets.json originali e stato finale.

Esce 0 solo se tutti i controlli passano.
"""
import argparse
import hashlib
import json
import os
import shutil
import socket
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

CTL = "/tmp/namzero-lite-ctl.sock"
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

        # ── (d) comandi di gruppo ───────────────────────────────────────────
        send_cmd({"cmd": "param", "slot": "gate", "key": "threshold", "value": -55.5})
        st = wait_state(lambda s: abs(s["slots"]["gate"]["threshold"] + 55.5) < 0.01,
                        label="param gate")
        check("param gate.threshold", st["slots"]["gate"]["threshold"] == -55.5)

        send_cmd({"cmd": "defaults", "slot": "gate"})
        st = wait_state(lambda s: abs(s["slots"]["gate"]["threshold"] - (-55.0)) < 0.01,
                        timeout=4, label="defaults gate")
        check("defaults gate", st["slots"]["gate"]["threshold"] == -55.0)

        send_cmd({"cmd": "looper", "action": "stop"})
        time.sleep(0.25)
        loop0 = read_state().get("looper")
        send_cmd({"cmd": "looper", "action": "toggle"})
        time.sleep(0.3)
        loop1 = read_state().get("looper")
        send_cmd({"cmd": "looper", "action": "stop"})
        time.sleep(0.3)
        loop2 = read_state().get("looper")
        if loop0 is None:
            check("looper toggle/stop", True, "looper assente nel preset")
        else:
            check("looper toggle/stop",
                  loop1 is not None and loop2 is not None and
                  loop1["state"] != loop0["state"] and loop2["state"] == "idle",
                  "%s -> %s -> %s" % (loop0["state"], loop1["state"], loop2["state"]))

        # save_options: scrive config/presets.json (ripristinato nel finally).
        send_cmd({"cmd": "save_options"})
        time.sleep(0.2)
        sj = json.load(open(presets_json))
        check("save_options scrive presets.json",
              all(k in sj for k in ("gate", "autoengage", "boost", "eq")) and
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
