#!/usr/bin/env python3
"""Test runtime dei Program Change 0-7 della G-Board (motore vero, sul Pi).

Inietta i byte MIDI sulla socket del bridge BLE, come ctl_stress.py, e verifica
la nuova mappa:

  - preset_pc=true: PC 0..7 selezionano gli slot 0..7 (stesso percorso della
    tastiera: slot/preset, .last_slot, flash LED, re-exec);
  - preset_pc=false e pc_effects=false: i PC sono ignorati (nessun re-exec,
    stato e .last_slot invariati);
  - preset_pc=false e pc_effects=true: i vecchi PC-effetto funzionano ancora,
    one-shot (nessun debounce), senza cambiare preset;
  - preset_pc=true e pc_effects=true: avviso a runtime e priorita' preset;
  - i CC 50-57 restano gli ingressi degli effetti (li copre
    gboard_cc_runtime.py, non vengono toccati qui).

Uso (sul Pi):
    python3 test/gboard_pc_runtime.py --repo ~/namzero-lite --all
    python3 test/gboard_pc_runtime.py            # prova solo la config viva

--all cambia temporaneamente config/midi.json (con backup) e riavvia il
servizio per coprire i quattro casi; alla fine ripristina config e slot iniziali.
Senza --all tocca solo la configurazione gia' presente e non riavvia il
servizio. Serve sudo passwordless per fermare/riavviare i servizi; se manca,
la modalita' --all si ferma con un errore chiaro (la modalita' auto continua
finche' non deve riavviare).

Esce 0 solo se tutti i controlli passano.
"""

import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

CTL = "/tmp/namzero-lite-ctl.sock"
BLE = "/tmp/namzero-lite-ble.sock"
MOD_TYPES = ["tremolo", "univibe", "flanger", "phaser"]
DRIVE_TYPES = ["fuzz", "klon", "tubescreamer"]

ok_count = 0
fail_count = 0
failures = []


def check(name, cond, detail=""):
    global ok_count, fail_count
    if cond:
        ok_count += 1
        print("PASS %s%s" % (name, (" - " + detail) if detail else ""), flush=True)
    else:
        fail_count += 1
        failures.append("%s%s" % (name, (": " + detail) if detail else ""))
        print("FAIL %s%s" % (name, (" - " + detail) if detail else ""), flush=True)


def note(msg):
    print("  " + msg, flush=True)


# ── socket di controllo / stato ─────────────────────────────────────────────

def connect(timeout=2.5, attempts=10):
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
    raise last if last is not None else RuntimeError("ctl socket assente")


def read_state(timeout=2.5, attempts=10):
    last = None
    for _ in range(attempts):
        s = None
        try:
            s = connect(timeout)
            f = s.makefile("rwb")
            f.write(b'{"cmd":"state"}\n')
            f.flush()
            line = f.readline()
            # Il motore manda anche gli snapshot in broadcast: si prende
            # l'ultima riga disponibile, come la UI.
            s.settimeout(0.03)
            while True:
                try:
                    nxt = f.readline()
                except (socket.timeout, OSError):
                    break
                if not nxt:
                    break
                line = nxt
            if line:
                return json.loads(line)
            last = RuntimeError("nessuna riga di stato")
        except (OSError, ValueError, RuntimeError) as exc:
            last = exc
        finally:
            if s is not None:
                s.close()
        time.sleep(0.1)
    raise last if last is not None else RuntimeError("stato non disponibile")


def socket_sig():
    try:
        st = os.stat(CTL)
        return (st.st_ino, st.st_ctime_ns)
    except OSError:
        return None


def send_cmd(payload, read_reply=True):
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


def wait_socket_change(before, pred, label, timeout=12.0):
    """Aspetta che la socket di controllo venga ricreata e lo stato atteso."""
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
    raise RuntimeError("timeout in attesa di %s" % label)


def wait_reexec(payload, pred, label, timeout=12.0):
    """Manda un comando che fa re-exec e aspetta socket nuovo + stato atteso."""
    before = socket_sig()
    try:
        send_cmd(payload, read_reply=False)
    except Exception:
        pass
    return wait_socket_change(before, pred, label, timeout)


# ── bridge BLE: fermo il servizio per l'iniezione ───────────────────────────

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
    """Ferma il bridge per tutta la durata del test.

    Con systemd fermo non riparte ogni 3 s e non ruba la socket a meta' test.
    Se sudo non c'e' si ripiega sul SIGTERM, come fanno gli altri test.
    """
    r = subprocess.run(["sudo", "-n", "systemctl", "stop", "namzero-ble-bridge"],
                       capture_output=True, text=True, timeout=15)
    if r.returncode != 0:
        note("systemctl stop del bridge non disponibile: uso SIGTERM")
        for pid in ble_pids():
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                pass
    deadline = time.time() + 6.0
    while time.time() < deadline:
        if not ble_pids():
            return
        time.sleep(0.15)


def start_ble_bridge():
    subprocess.run(["sudo", "-n", "systemctl", "start", "namzero-ble-bridge"],
                   capture_output=True, text=True, timeout=15)


def ble_connect(timeout=8.0):
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


def pc_once(number):
    """Un PC su una connessione BLE nuova.

    Serve per i cambi preset: il re-exec chiude la socket, quindi la
    connessione va riaperta a ogni PC.
    """
    s = ble_connect()
    try:
        s.sendall(bytes([0xC0, number & 0x7F]))
    finally:
        s.close()


def pc_on(sock, number):
    sock.sendall(bytes([0xC0, number & 0x7F]))


# ── config viva e riavvio del servizio ──────────────────────────────────────

def restart_engine(label):
    before = socket_sig()
    r = subprocess.run(["sudo", "-n", "systemctl", "restart", "namzero-lite"],
                       capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        raise RuntimeError("systemctl restart namzero-lite: %s %s" %
                           (r.stdout.strip(), r.stderr.strip()))
    deadline = time.time() + 20.0
    last = None
    while time.time() < deadline:
        sig = socket_sig()
        if sig is not None and sig != before:
            try:
                st = read_state()
                if st.get("slot") is not None:
                    note("servizio riavviato (%s), slot=%s preset=%s" %
                         (label, st.get("slot"), st.get("preset")))
                    return st
            except Exception as exc:
                last = exc
        time.sleep(0.15)
    raise RuntimeError("servizio non pronto dopo il riavvio (%s): %r" % (label, last))


def write_config(path, preset_pc, pc_effects):
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    cfg.setdefault("gboard", {})
    cfg["gboard"]["preset_pc"] = bool(preset_pc)
    cfg["gboard"]["pc_effects"] = bool(pc_effects)
    tmp = path + ".test.tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cfg, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def slot_file(repo, pj, n):
    """Percorso come lo riporta /api/state (relativo alla WorkingDirectory)."""
    f = pj["slots"][n]
    return f if f.startswith("/") else "config/" + f


def read_last_slot(repo):
    try:
        with open(os.path.join(repo, "config/.last_slot"), "r", encoding="utf-8") as f:
            return int(f.read().strip())
    except (OSError, ValueError):
        return None


# ── firme dello stato ───────────────────────────────────────────────────────

def effects_sig(st):
    m = st["slots"]["mod"]
    d = st["slots"]["drive"]
    return (m["type"], bool(m["on"]), d["type"], bool(d["on"]), bool(st["boost"]))


def leds_expected(st):
    gb = st.get("gboard", {})
    mod_on = bool(st["slots"]["mod"]["on"])
    mod_type = st["slots"]["mod"]["type"]
    drive_on = bool(st["slots"]["drive"]["on"])
    drive_type = st["slots"]["drive"]["type"]
    out = []
    for i, note in enumerate(gb.get("mod_led", [])):
        out.append({"note": note, "on": mod_on and mod_type == MOD_TYPES[i]})
    for i, note in enumerate(gb.get("drive_led", [])):
        out.append({"note": note, "on": drive_on and drive_type == DRIVE_TYPES[i]})
    out.append({"note": gb.get("boost_led"), "on": bool(st["boost"])})
    return out


def check_leds(label, st):
    got = st.get("gboard", {}).get("leds")
    want = leds_expected(st)
    check(label + " LED", got == want, "atteso %r, letto %r" % (want, got))


def preset_effect_expected(repo, pj, n):
    """mod/drive del file di preset: il PC deve caricare quello, non un toggle."""
    path = os.path.join(repo, slot_file(repo, pj, n))
    try:
        with open(path, "r", encoding="utf-8") as f:
            p = json.load(f)
    except (OSError, ValueError):
        return None
    out = {}
    for key in ("mod", "drive"):
        if isinstance(p.get(key), dict):
            out[key] = (p[key].get("type"), bool(p[key].get("on", False)))
    return out


def check_preset_effects(label, repo, pj, n, st):
    want = preset_effect_expected(repo, pj, n)
    if want is None:
        return
    if "mod" in want:
        got = (st["slots"]["mod"]["type"], bool(st["slots"]["mod"]["on"]))
        check(label + " mod dal preset", got == want["mod"],
              "atteso %r, letto %r" % (want["mod"], got))
    if "drive" in want:
        got = (st["slots"]["drive"]["type"], bool(st["slots"]["drive"]["on"]))
        check(label + " drive dal preset", got == want["drive"],
              "atteso %r, letto %r" % (want["drive"], got))
    if isinstance(pj.get("boost"), dict):
        want_boost = bool(pj["boost"].get("on", False))
        check(label + " boost dal preset", bool(st["boost"]) == want_boost,
              "atteso %r, letto %r" % (want_boost, st["boost"]))


# ── scenari ─────────────────────────────────────────────────────────────────

def scenario_preset(repo, pj):
    st0 = read_state()
    gb = st0.get("gboard", {})
    check("state espone gboard.preset_pc", "preset_pc" in gb, repr(gb))
    check("state espone gboard.pc_effects", "pc_effects" in gb, repr(gb))
    check("config viva: preset_pc=true", gb.get("preset_pc") is True, repr(gb.get("preset_pc")))
    check("config viva: pc_effects=false", gb.get("pc_effects") is False,
          repr(gb.get("pc_effects")))
    original_slot = st0["slot"]
    original_last = read_last_slot(repo)

    # PC 0..7 -> slot 0..7, uno alla volta (ogni cambio fa re-exec).
    for n in range(8):
        before_sig = socket_sig()
        try:
            pc_once(n)
            st = wait_socket_change(before_sig,
                                    lambda s: s.get("slot") == n,
                                    "PC%d -> slot %d" % (n, n))
        except RuntimeError as exc:
            check("PC%d -> slot %d" % (n, n), False, str(exc))
            continue
        want_preset = slot_file(repo, pj, n)
        check("PC%d -> slot %d" % (n, n), st.get("slot") == n,
              "letto %r" % (st.get("slot"),))
        check("PC%d -> preset dello slot %d" % (n, n),
              st.get("preset") == want_preset,
              "atteso %r, letto %r" % (want_preset, st.get("preset")))
        check("PC%d scrive .last_slot=%d" % (n, n), read_last_slot(repo) == n,
              "letto %r" % (read_last_slot(repo),))
        check_preset_effects("PC%d" % n, repo, pj, n, st)
        check_leds("PC%d" % n, st)

    # PC fuori 0-7 non ha ne' preset ne' effetto (pc_effects=false).
    st = read_state()
    sig = socket_sig()
    slot_before = st["slot"]
    preset_before = st["preset"]
    fx_before = effects_sig(st)
    last_before = read_last_slot(repo)
    try:
        pc_once(8)
    except Exception as exc:
        check("PC8 ignorato", False, repr(exc))
    time.sleep(0.45)
    st8 = read_state()
    check("PC8 ignorato (nessun re-exec)", socket_sig() == sig,
          "socket cambiata")
    check("PC8 non cambia slot/preset",
          st8["slot"] == slot_before and st8["preset"] == preset_before,
          "slot %r->%r preset %r->%r" % (slot_before, st8["slot"],
                                         preset_before, st8["preset"]))
    check("PC8 non fa azioni effetti", effects_sig(st8) == fx_before,
          "prima %r dopo %r" % (fx_before, effects_sig(st8)))
    check("PC8 non tocca .last_slot", read_last_slot(repo) == last_before,
          "letto %r" % (read_last_slot(repo),))

    # Si torna allo slot iniziale con lo stesso percorso (ctl -> re-exec).
    try:
        wait_reexec({"cmd": "slot", "n": original_slot},
                    lambda s: s.get("slot") == original_slot,
                    "ritorno allo slot %d" % original_slot)
        check("slot iniziale ripristinato", read_state()["slot"] == original_slot)
    except RuntimeError as exc:
        check("slot iniziale ripristinato", False, str(exc))
    if original_last is not None:
        check(".last_slot iniziale ripristinato", read_last_slot(repo) == original_last,
              "letto %r" % (read_last_slot(repo),))


def scenario_ignored(repo):
    st0 = read_state()
    gb = st0.get("gboard", {})
    check("config viva: preset_pc=false", gb.get("preset_pc") is False,
          repr(gb.get("preset_pc")))
    check("config viva: pc_effects=false", gb.get("pc_effects") is False,
          repr(gb.get("pc_effects")))
    sig = socket_sig()
    slot_before = st0["slot"]
    preset_before = st0["preset"]
    fx_before = effects_sig(st0)
    last_before = read_last_slot(repo)

    sock = ble_connect()
    try:
        for n in range(8):
            pc_on(sock, n)
            time.sleep(0.04)
    finally:
        sock.close()
    time.sleep(0.45)
    st = read_state()
    check("PC 0-7 ignorati: nessun re-exec", socket_sig() == sig, "socket cambiata")
    check("PC 0-7 ignorati: slot/preset invariati",
          st["slot"] == slot_before and st["preset"] == preset_before,
          "slot %r->%r preset %r->%r" % (slot_before, st["slot"],
                                         preset_before, st["preset"]))
    check("PC 0-7 ignorati: nessuna azione effetti", effects_sig(st) == fx_before,
          "prima %r dopo %r" % (fx_before, effects_sig(st)))
    check("PC 0-7 ignorati: .last_slot invariato", read_last_slot(repo) == last_before,
          "letto %r" % (read_last_slot(repo),))
    check_leds("PC ignorati", st)


def scenario_both(repo, pj):
    """Entrambi true: avviso all'avvio e priorita' preset sui PC 0-7."""
    st0 = read_state()
    gb = st0.get("gboard", {})
    check("config viva: preset_pc=true (both)", gb.get("preset_pc") is True,
          repr(gb.get("preset_pc")))
    check("config viva: pc_effects=true (both)", gb.get("pc_effects") is True,
          repr(gb.get("pc_effects")))

    warned = False
    try:
        j = subprocess.run(["sudo", "-n", "journalctl", "-u", "namzero-lite",
                            "--since", "2 min ago", "--no-pager"],
                           capture_output=True, text=True, timeout=10)
        warned = ("gboard.preset_pc e gboard.pc_effects" in j.stdout or
                  "gboard.preset_pc e gboard.pc_effects" in j.stderr)
    except Exception:
        pass
    check("avviso runtime con preset_pc e pc_effects entrambi true", warned)

    original_slot = st0["slot"]
    original_last = read_last_slot(repo)
    before_sig = socket_sig()
    try:
        pc_once(4)  # legacy sarebbe mod tremolo; con priorita' deve fare slot 4
        st = wait_socket_change(before_sig, lambda s: s.get("slot") == 4,
                                "PC4 -> slot 4 (both)")
    except RuntimeError as exc:
        check("priorita' preset: PC4 -> slot 4", False, str(exc))
        return
    check("priorita' preset: PC4 -> slot 4", st["slot"] == 4,
          "letto %r" % (st["slot"],))
    check("priorita' preset: preset dello slot 4",
          st["preset"] == slot_file(repo, pj, 4),
          "letto %r" % (st["preset"],))
    check("priorita' preset: .last_slot=4", read_last_slot(repo) == 4,
          "letto %r" % (read_last_slot(repo),))
    check_preset_effects("PC4 (both)", repo, pj, 4, st)

    try:
        wait_reexec({"cmd": "slot", "n": original_slot},
                    lambda s: s.get("slot") == original_slot,
                    "ritorno allo slot %d (both)" % original_slot)
        check("slot iniziale ripristinato (both)",
              read_state()["slot"] == original_slot)
    except RuntimeError as exc:
        check("slot iniziale ripristinato (both)", False, str(exc))
    if original_last is not None:
        check(".last_slot iniziale ripristinato (both)",
              read_last_slot(repo) == original_last,
              "letto %r" % (read_last_slot(repo),))


def scenario_effects(repo, gb_cfg):
    st0 = read_state()
    gb = st0.get("gboard", {})
    check("config viva: preset_pc=false", gb.get("preset_pc") is False,
          repr(gb.get("preset_pc")))
    check("config viva: pc_effects=true", gb.get("pc_effects") is True,
          repr(gb.get("pc_effects")))
    sig = socket_sig()
    slot_before = st0["slot"]
    preset_before = st0["preset"]
    last_before = read_last_slot(repo)
    initial_fx = effects_sig(st0)

    mod_pc = gb_cfg.get("mod_pc", [])
    drive_pc = gb_cfg.get("drive_pc", [])
    boost_pc = gb_cfg.get("boost_pc", -1)

    sock = ble_connect()
    try:
        # I tre drive (PC 0-2 nella config viva).
        for idx, number in enumerate(drive_pc):
            if not (0 <= number <= 127):
                continue
            before = read_state()
            d = before["slots"]["drive"]
            target = DRIVE_TYPES[idx]
            want = (target, not (d["type"] == target and bool(d["on"])))
            pc_on(sock, number)
            time.sleep(0.08)
            try:
                st = read_state()
            except Exception as exc:
                check("PC drive %d" % number, False, repr(exc))
                continue
            got = (st["slots"]["drive"]["type"], bool(st["slots"]["drive"]["on"]))
            check("PC%d (legacy) -> drive %s %s" %
                  (number, want[0], "on" if want[1] else "off"), got == want,
                  "letto %r" % (got,))
            check("PC%d non cambia preset" % number,
                  st["slot"] == slot_before and st["preset"] == preset_before,
                  "slot %r preset %r" % (st["slot"], st["preset"]))
            check("PC%d nessun re-exec" % number, socket_sig() == sig,
                  "socket cambiata")
            check_leds("PC drive %d" % number, st)

        # I quattro mod (PC 4-7 nella config viva).
        for idx, number in enumerate(mod_pc):
            if not (0 <= number <= 127):
                continue
            before = read_state()
            m = before["slots"]["mod"]
            target = MOD_TYPES[idx]
            want = (target, not (m["type"] == target and bool(m["on"])))
            pc_on(sock, number)
            time.sleep(0.08)
            try:
                st = read_state()
            except Exception as exc:
                check("PC mod %d" % number, False, repr(exc))
                continue
            got = (st["slots"]["mod"]["type"], bool(st["slots"]["mod"]["on"]))
            check("PC%d (legacy) -> mod %s %s" %
                  (number, want[0], "on" if want[1] else "off"), got == want,
                  "letto %r" % (got,))
            check_leds("PC mod %d" % number, st)

        # Boost.
        if boost_pc >= 0:
            before = read_state()
            want_boost = not bool(before["boost"])
            pc_on(sock, boost_pc)
            time.sleep(0.08)
            st = read_state()
            check("PC%d (legacy) -> boost %s" %
                  (boost_pc, "on" if want_boost else "off"),
                  bool(st["boost"]) == want_boost, "letto %r" % (st["boost"],))
            check_leds("PC boost", st)

        # One-shot: due PC uguali ravvicinati = due toggle, nessun debounce.
        if mod_pc and 0 <= mod_pc[0] <= 127:
            number = mod_pc[0]
            before = read_state()
            m = before["slots"]["mod"]
            target = MOD_TYPES[0]
            want1 = (target, not (m["type"] == target and bool(m["on"])))
            pc_on(sock, number)
            time.sleep(0.08)
            st1 = read_state()
            got1 = (st1["slots"]["mod"]["type"], bool(st1["slots"]["mod"]["on"]))
            check("PC%d one-shot: primo toggle" % number, got1 == want1,
                  "letto %r" % (got1,))
            m1 = st1["slots"]["mod"]
            want2 = (target, not (m1["type"] == target and bool(m1["on"])))
            pc_on(sock, number)
            time.sleep(0.08)
            st2 = read_state()
            got2 = (st2["slots"]["mod"]["type"], bool(st2["slots"]["mod"]["on"]))
            check("PC%d one-shot: due PC ravvicinati = due toggle" % number,
                  got2 == want2, "letto %r" % (got2,))
            check_leds("PC one-shot", st2)
    finally:
        sock.close()

    st = read_state()
    check("PC legacy non tocca .last_slot", read_last_slot(repo) == last_before,
          "letto %r" % (read_last_slot(repo),))
    check("PC legacy non cambia slot/preset",
          st["slot"] == slot_before and st["preset"] == preset_before,
          "slot %r preset %r" % (st["slot"], st["preset"]))

    # Ripristino degli effetti con la stessa via (PC legacy), come faceva
    # gboard_cc_runtime.py con i CC.
    try:
        restore_effects_with_pc(gb_cfg, initial_fx)
    except Exception as exc:
        check("stato effetti iniziale ripristinato", False, repr(exc))
    else:
        final = read_state()
        check("stato effetti iniziale ripristinato",
              effects_sig(final) == initial_fx,
              "prima %r dopo %r" % (initial_fx, effects_sig(final)))


def restore_effects_with_pc(gb_cfg, initial_fx):
    """Riporta mod/drive/boost allo stato iniziale usando i PC legacy."""
    mod_pc = gb_cfg.get("mod_pc", [])
    drive_pc = gb_cfg.get("drive_pc", [])
    boost_pc = gb_cfg.get("boost_pc", -1)
    sock = ble_connect()
    try:
        def press(number):
            pc_on(sock, number)
            time.sleep(0.08)

        def restore_group(read_group, names, pcs, target):
            for _ in range(4):
                cur = read_group()
                if cur == target:
                    return
                if target[0] not in names:
                    return
                idx = names.index(target[0])
                if idx >= len(pcs) or not (0 <= pcs[idx] <= 127):
                    return
                if cur[0] == target[0]:
                    press(pcs[idx])  # stesso tipo: cambia solo on/off
                else:
                    press(pcs[idx])  # seleziona il tipo (lo accende)
                    if not target[1]:
                        press(pcs[idx])  # ...e lo rispegne
            cur = read_group()
            return cur == target

        def drive_group():
            st = read_state()
            d = st["slots"]["drive"]
            return (d["type"], bool(d["on"]))

        def mod_group():
            st = read_state()
            m = st["slots"]["mod"]
            return (m["type"], bool(m["on"]))

        restore_group(drive_group, DRIVE_TYPES, drive_pc,
                      (initial_fx[2], initial_fx[3]))
        restore_group(mod_group, MOD_TYPES, mod_pc,
                      (initial_fx[0], initial_fx[1]))
        if boost_pc >= 0:
            for _ in range(3):
                if bool(read_state()["boost"]) == initial_fx[4]:
                    break
                press(boost_pc)
    finally:
        sock.close()


# ── main ────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.expanduser("~/namzero-lite"))
    ap.add_argument("--all", action="store_true",
                    help="prova i quattro casi (preset, ignorato, both, pc_effects) "
                         "cambiando config e riavviando il servizio")
    ap.add_argument("--mode",
                    choices=["auto", "preset", "ignored", "effects", "both"],
                    default="auto")
    args = ap.parse_args()
    repo = os.path.abspath(os.path.expanduser(args.repo))
    midi_path = os.path.join(repo, "config/midi.json")
    presets_path = os.path.join(repo, "config/presets.json")
    last_slot_path = os.path.join(repo, "config/.last_slot")
    print("== repo %s" % repo, flush=True)

    with open(presets_path, "r", encoding="utf-8") as f:
        pj = json.load(f)
    with open(midi_path, "r", encoding="utf-8") as f:
        midi_json = json.load(f)
    gb_cfg = midi_json.get("gboard", {})

    st = read_state()
    initial_slot = st["slot"]
    initial_preset = st["preset"]
    initial_last = read_last_slot(repo)
    orig_preset_pc = gb_cfg.get("preset_pc", True)
    orig_pc_effects = gb_cfg.get("pc_effects", False)
    print("stato iniziale: slot=%s preset=%s" % (initial_slot, initial_preset),
          flush=True)

    backup_dir = tempfile.mkdtemp(prefix="namzero-pc-test-")
    backup_midi = os.path.join(backup_dir, "midi.json")
    shutil.copy2(midi_path, backup_midi)
    backup_last = None
    if os.path.exists(last_slot_path):
        backup_last = os.path.join(backup_dir, ".last_slot")
        shutil.copy2(last_slot_path, backup_last)
    config_changed = False
    ble_stopped = False

    def restore():
        nonlocal config_changed
        if config_changed:
            shutil.copy2(backup_midi, midi_path)
            config_changed = False
        if backup_last is not None:
            shutil.copy2(backup_last, last_slot_path)
        elif os.path.exists(last_slot_path):
            os.remove(last_slot_path)

    try:
        stop_ble_bridge()
        ble_stopped = True

        modes = []
        if args.all:
            modes = ["preset", "ignored", "both", "effects"]
        elif args.mode != "auto":
            modes = [args.mode]
        else:
            if gb_cfg.get("preset_pc", True):
                modes = ["preset"]
            elif gb_cfg.get("pc_effects", False):
                modes = ["effects"]
            else:
                modes = ["ignored"]

        for mode in modes:
            print("\n-- scenario %s" % mode, flush=True)
            if mode == "preset":
                if not (gb_cfg.get("preset_pc", True) and
                        not gb_cfg.get("pc_effects", False)):
                    write_config(midi_path, True, False)
                    config_changed = True
                    restart_engine("preset_pc=true, pc_effects=false")
                    with open(midi_path, "r", encoding="utf-8") as f:
                        gb_cfg = json.load(f).get("gboard", {})
                scenario_preset(repo, pj)
            elif mode == "ignored":
                if not (not gb_cfg.get("preset_pc", True) and
                        not gb_cfg.get("pc_effects", False)):
                    write_config(midi_path, False, False)
                    config_changed = True
                    restart_engine("preset_pc=false, pc_effects=false")
                    with open(midi_path, "r", encoding="utf-8") as f:
                        gb_cfg = json.load(f).get("gboard", {})
                scenario_ignored(repo)
            elif mode == "both":
                if not (gb_cfg.get("preset_pc", True) and
                        gb_cfg.get("pc_effects", False)):
                    write_config(midi_path, True, True)
                    config_changed = True
                    restart_engine("preset_pc=true, pc_effects=true")
                    with open(midi_path, "r", encoding="utf-8") as f:
                        gb_cfg = json.load(f).get("gboard", {})
                scenario_both(repo, pj)
            elif mode == "effects":
                if not (not gb_cfg.get("preset_pc", True) and
                        gb_cfg.get("pc_effects", False)):
                    write_config(midi_path, False, True)
                    config_changed = True
                    restart_engine("preset_pc=false, pc_effects=true")
                    with open(midi_path, "r", encoding="utf-8") as f:
                        gb_cfg = json.load(f).get("gboard", {})
                scenario_effects(repo, gb_cfg)

        # Ripristino: config originale, .last_slot originale e riavvio, cosi'
        # il servizio torna esattamente allo stato iniziale.
        if config_changed:
            if backup_last is not None:
                shutil.copy2(backup_last, last_slot_path)
            elif os.path.exists(last_slot_path):
                os.remove(last_slot_path)
            shutil.copy2(backup_midi, midi_path)
            config_changed = False
            restart_engine("config originale")
        final = read_state()
        check("config originale ripristinata",
              final.get("gboard", {}).get("preset_pc") == orig_preset_pc and
              final.get("gboard", {}).get("pc_effects") == orig_pc_effects,
              "atteso preset_pc=%r pc_effects=%r, letto %r" %
              (orig_preset_pc, orig_pc_effects, final.get("gboard", {})))
        check("stato finale: slot/preset iniziali",
              final["slot"] == initial_slot and final["preset"] == initial_preset,
              "atteso slot=%r preset=%r, letto slot=%r preset=%r" %
              (initial_slot, initial_preset, final["slot"], final["preset"]))
        check("stato finale: .last_slot iniziale",
              initial_last is None or read_last_slot(repo) == initial_last,
              "atteso %r, letto %r" % (initial_last, read_last_slot(repo)))
    finally:
        try:
            if config_changed:
                restore()
                restart_engine("ripristino di emergenza")
        except Exception as exc:
            print("  ATTENZIONE: ripristino config fallito: %r" % (exc,), flush=True)
        if backup_last is not None:
            try:
                shutil.copy2(backup_last, last_slot_path)
            except OSError:
                pass
        if ble_stopped:
            start_ble_bridge()
        shutil.rmtree(backup_dir, ignore_errors=True)

    print("\n%d PASS, %d FAIL" % (ok_count, fail_count), flush=True)
    if failures:
        for f in failures:
            print("  - " + f, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
