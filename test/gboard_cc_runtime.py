#!/usr/bin/env python3
"""Test runtime della G-Board riconfigurata a CC (motore vero, sul Pi).

Inietta i byte MIDI sulla socket del bridge BLE, come fa ctl_stress.py per
l'espressione, e verifica:

  - i CC 50-52 selezionano fuzz/klon/tubescreamer e accendono/spegnono il drive;
  - i CC 54-57 selezionano tremolo/univibe/flanger/phaser e accendono/spegnono
    il mod;
  - il CC 53 fa on/off del boost;
  - il rilascio (0) non fa nulla e due pressioni ravvicinate contano una sola
    volta (SwitchDebounce in modo edge, 150 ms);
  - i vecchi Program Change (4-7 mod, 0-2 drive, 3 boost) sono ancora accettati
    e restano one-shot (nessun debounce);
  - i LED riportati in /api/state usano le note di midi.json (4-7 / 0-2 / 3),
    indipendenti dai numeri dei CC.

Uso (sul Pi, motore gia' avviato):
    python3 test/gboard_cc_runtime.py [--repo ~/namzero-lite]

Il bridge BLE viene fermato per la durata del test e lasciato ripartire:
systemd lo rilancia. Esce 0 solo se tutti i controlli passano.
"""

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import time

CTL = "/tmp/namzero-lite-ctl.sock"
BLE = "/tmp/namzero-lite-ble.sock"

MOD_TYPES = ["tremolo", "univibe", "flanger", "phaser"]
DRIVE_TYPES = ["fuzz", "klon", "tubescreamer"]
MOD_CC = [54, 55, 56, 57]
DRIVE_CC = [50, 51, 52]
BOOST_CC = 53
MOD_LED = [4, 5, 6, 7]
DRIVE_LED = [0, 1, 2]
BOOST_LED = 3
MOD_PC = [4, 5, 6, 7]
DRIVE_PC = [0, 1, 2]
BOOST_PC = 3

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
    raise last


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


def wait_state(pred, timeout=4.0, label="stato"):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        st = read_state()
        last = st
        if pred(st):
            return st
        time.sleep(0.04)
    raise RuntimeError("timeout in attesa di %s: %r" % (label, last))


# ── bridge BLE: si ferma, si inietta, si lascia ripartire ───────────────────

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
    for pid in ble_pids():
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


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


def cc(sock, number, value=127):
    sock.sendall(bytes([0xB0, number & 0x7F, value & 0x7F]))


def pc(sock, number):
    sock.sendall(bytes([0xC0, number & 0x7F]))


# ── attese e letture di stato ───────────────────────────────────────────────

def mod_sig(st):
    m = st["slots"]["mod"]
    return (m["type"], bool(m["on"]))


def drive_sig(st):
    d = st["slots"]["drive"]
    return (d["type"], bool(d["on"]))


def mod_expected(st, idx):
    typ, on = mod_sig(st)
    target = MOD_TYPES[idx]
    return (target, not (typ == target and on))


def drive_expected(st, idx):
    typ, on = drive_sig(st)
    target = DRIVE_TYPES[idx]
    return (target, not (typ == target and on))


def leds_expected(st):
    m_on = bool(st["slots"]["mod"]["on"])
    m_type = st["slots"]["mod"]["type"]
    d_on = bool(st["slots"]["drive"]["on"])
    d_type = st["slots"]["drive"]["type"]
    boost = bool(st["boost"])
    out = []
    for i, note in enumerate(MOD_LED):
        out.append({"note": note, "on": m_on and m_type == MOD_TYPES[i]})
    for i, note in enumerate(DRIVE_LED):
        out.append({"note": note, "on": d_on and d_type == DRIVE_TYPES[i]})
    out.append({"note": BOOST_LED, "on": boost})
    return out


def check_leds(label, st):
    got = st.get("gboard", {}).get("leds")
    want = leds_expected(st)
    check(label + " LED", got == want, "atteso %r, letto %r" % (want, got))


def settle():
    time.sleep(0.12)


def burst_cc(sock, number, repeats=3):
    """Ripete 127 come fa ctl_stress: se un byte si perde non e' un falso FAIL.
    Il debounce del motore deve comunque contare una sola pressione."""
    for i in range(repeats):
        cc(sock, number, 127)
        if i + 1 < repeats:
            time.sleep(0.01)


def press_cc(sock, number, label, before):
    burst_cc(sock, number)
    time.sleep(0.02)
    cc(sock, number, 0)  # rilascio reale: non deve fare nulla
    settle()
    st = wait_state(lambda s: s != before, label=label)
    return st


def do_cc(sock, number):
    """Una pressione completa (127 + rilascio) con attesa oltre il debounce."""
    burst_cc(sock, number)
    time.sleep(0.05)
    cc(sock, number, 0)
    time.sleep(0.20)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=os.path.expanduser("~/namzero-lite"))
    args = ap.parse_args()
    repo = os.path.abspath(os.path.expanduser(args.repo))
    print("== repo %s" % repo, flush=True)

    st0 = read_state()
    print("stato iniziale: mod=%r drive=%r boost=%r" %
          (mod_sig(st0), drive_sig(st0), st0["boost"]), flush=True)

    # La config viva deve essere quella nuova: se no, il deploy non e' passato.
    gb = st0.get("gboard", {})
    check("config: mod_cc 54-57", gb.get("mod_cc") == MOD_CC, repr(gb.get("mod_cc")))
    check("config: drive_cc 50-52", gb.get("drive_cc") == DRIVE_CC,
          repr(gb.get("drive_cc")))
    check("config: boost_cc 53", gb.get("boost_cc") == BOOST_CC,
          repr(gb.get("boost_cc")))
    check("config: mod_led 4-7", gb.get("mod_led") == MOD_LED, repr(gb.get("mod_led")))
    check("config: drive_led 0-2", gb.get("drive_led") == DRIVE_LED,
          repr(gb.get("drive_led")))
    check("config: boost_led 3", gb.get("boost_led") == BOOST_LED,
          repr(gb.get("boost_led")))
    check_leds("stato iniziale", st0)

    stop_ble_bridge()
    time.sleep(0.3)
    sock = ble_connect()
    try:
        # I CC 50-52: drive.
        for idx, number in enumerate(DRIVE_CC):
            before = read_state()
            want = drive_expected(before, idx)
            try:
                st = press_cc(sock, number, "drive CC%d" % number, before)
            except RuntimeError as exc:
                check("drive CC%d" % number, False, str(exc))
                continue
            check("drive CC%d -> %s %s" % (number, want[0], "on" if want[1] else "off"),
                  drive_sig(st) == want, "letto %r" % (drive_sig(st),))
            check_leds("drive CC%d" % number, st)

        # I CC 54-57: mod.
        for idx, number in enumerate(MOD_CC):
            before = read_state()
            want = mod_expected(before, idx)
            try:
                st = press_cc(sock, number, "mod CC%d" % number, before)
            except RuntimeError as exc:
                check("mod CC%d" % number, False, str(exc))
                continue
            check("mod CC%d -> %s %s" % (number, want[0], "on" if want[1] else "off"),
                  mod_sig(st) == want, "letto %r" % (mod_sig(st),))
            check_leds("mod CC%d" % number, st)

        # Boost (CC 53).
        before = read_state()
        want_boost = not bool(before["boost"])
        try:
            st = press_cc(sock, BOOST_CC, "boost CC53", before)
            check("boost CC53 -> %s" % ("on" if want_boost else "off"),
                  bool(st["boost"]) == want_boost, "letto %r" % (st["boost"],))
            check_leds("boost CC53", st)
        except RuntimeError as exc:
            check("boost CC53", False, str(exc))

        # Rilascio (0): nessuna azione.
        before = read_state()
        cc(sock, MOD_CC[0], 0)
        settle()
        after = read_state()
        check("rilascio CC54 = nessuna azione",
              mod_sig(after) == mod_sig(before) and bool(after["boost"]) == bool(before["boost"]),
              "prima %r dopo %r" % (mod_sig(before), mod_sig(after)))

        # Debounce: due 127 ravvicinati contano una volta sola. Si manda il
        # secondo entro 150 ms dal primo, senza aspettare lo stato nel mezzo.
        before = read_state()
        want = mod_expected(before, 1)  # univibe, CC55
        burst_cc(sock, MOD_CC[1])
        time.sleep(0.02)
        cc(sock, MOD_CC[1], 0)
        time.sleep(0.02)
        burst_cc(sock, MOD_CC[1])  # entro i 150 ms: ignorata
        time.sleep(0.25)
        twice = read_state()
        check("debounce CC55: seconda pressione ravvicinata ignorata",
              mod_sig(twice) == want, "atteso %r, letto %r" % (want, mod_sig(twice)))
        check_leds("debounce CC55", twice)
        cc(sock, MOD_CC[1], 0)
        time.sleep(0.20)
        # Oltre i 150 ms deve tornare a contare.
        before = read_state()
        want2 = mod_expected(before, 1)
        try:
            st = press_cc(sock, MOD_CC[1], "CC55 dopo debounce", before)
            check("CC55 dopo 150 ms conta di nuovo", mod_sig(st) == want2,
                  "letto %r" % (mod_sig(st),))
        except RuntimeError as exc:
            check("CC55 dopo 150 ms conta di nuovo", False, str(exc))

        # Vecchi Program Change: mod 4-7, drive 0-2, boost 3.
        for idx, number in enumerate(MOD_PC):
            before = read_state()
            want = mod_expected(before, idx)
            pc(sock, number)
            settle()
            try:
                st = wait_state(lambda s: mod_sig(s) == want, label="PC%d" % number)
                check("PC%d (legacy) -> %s %s" % (number, want[0],
                                                   "on" if want[1] else "off"),
                      mod_sig(st) == want, "letto %r" % (mod_sig(st),))
                check_leds("PC%d" % number, st)
            except RuntimeError as exc:
                check("PC%d (legacy)" % number, False, str(exc))

        for idx, number in enumerate(DRIVE_PC):
            before = read_state()
            want = drive_expected(before, idx)
            pc(sock, number)
            settle()
            try:
                st = wait_state(lambda s: drive_sig(s) == want, label="PC%d" % number)
                check("PC%d (legacy) -> %s %s" % (number, want[0],
                                                   "on" if want[1] else "off"),
                      drive_sig(st) == want, "letto %r" % (drive_sig(st),))
                check_leds("PC%d" % number, st)
            except RuntimeError as exc:
                check("PC%d (legacy)" % number, False, str(exc))

        before = read_state()
        want_boost = not bool(before["boost"])
        pc(sock, BOOST_PC)
        settle()
        try:
            st = wait_state(lambda s: bool(s["boost"]) == want_boost, label="PC3")
            check("PC3 (legacy) -> boost %s" % ("on" if want_boost else "off"),
                  bool(st["boost"]) == want_boost, "letto %r" % (st["boost"],))
            check_leds("PC3", st)
        except RuntimeError as exc:
            check("PC3 (legacy)", False, str(exc))

        # I PC sono one-shot: due PC uguali ravvicinati = due toggle (il
        # secondo spegne lo stesso tipo, non viene ignorato dal debounce).
        before = read_state()
        want1 = mod_expected(before, 2)
        pc(sock, MOD_PC[2])
        settle()
        try:
            st1 = wait_state(lambda s: mod_sig(s) == want1, label="PC6 primo")
            want2 = mod_expected(st1, 2)
            pc(sock, MOD_PC[2])
            settle()
            st2 = wait_state(lambda s: mod_sig(s) == want2, label="PC6 secondo")
            check("PC6 one-shot: due PC ravvicinati = due toggle",
                  mod_sig(st2) == want2, "letto %r" % (mod_sig(st2),))
            check_leds("PC6 one-shot", st2)
        except RuntimeError as exc:
            check("PC6 one-shot", False, str(exc))

    finally:
        # Ripristino dello stato iniziale, sempre tramite la stessa logica.
        try:
            def restore_group(read_sig, cc_list, names, initial):
                t, o = read_sig(read_state())
                target_type = initial["type"]
                target_on = bool(initial["on"])
                if t == target_type and o == target_on:
                    return
                if target_on:
                    do_cc(sock, cc_list[names.index(target_type)])
                    return
                if o:
                    do_cc(sock, cc_list[names.index(t)])
                t, o = read_sig(read_state())
                if t != target_type:
                    # Il tipo cambia solo selezionandolo: per lasciarlo spento
                    # servono due pressioni (on + off).
                    do_cc(sock, cc_list[names.index(target_type)])
                    do_cc(sock, cc_list[names.index(target_type)])

            restore_group(mod_sig, MOD_CC, MOD_TYPES, st0["slots"]["mod"])
            restore_group(drive_sig, DRIVE_CC, DRIVE_TYPES, st0["slots"]["drive"])
            if bool(read_state()["boost"]) != bool(st0["boost"]):
                do_cc(sock, BOOST_CC)
            final = read_state()
            check("stato iniziale ripristinato",
                  mod_sig(final) == mod_sig(st0) and
                  drive_sig(final) == drive_sig(st0) and
                  bool(final["boost"]) == bool(st0["boost"]),
                  "mod=%r drive=%r boost=%r" %
                  (mod_sig(final), drive_sig(final), final["boost"]))
            check_leds("ripristino", final)
        except Exception as exc:
            check("ripristino stato iniziale", False, repr(exc))
        try:
            sock.close()
        except Exception:
            pass
        if not wait_ble_bridge(12):
            print("  nota: bridge BLE non ancora ripartito", flush=True)

    print("\n%d PASS, %d FAIL" % (ok_count, fail_count), flush=True)
    if failures:
        for f in failures:
            print("  - " + f, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
