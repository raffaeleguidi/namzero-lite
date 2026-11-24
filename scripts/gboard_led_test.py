#!/usr/bin/env python3
"""Test LED G-Board: Program Change N -> accende il LED N (nota N).

Solo stdlib (ALSA rawmidi, /dev/snd/midiC*D*). Non tocca la config ne' il motore:
va lanciato con il motore FERMO (il rawmidi della G-Board e' unico).

Uso:
    pkill -x namzero-lite
    python3 scripts/gboard_led_test.py [--device /dev/snd/midiC2D0]

Manda Note On canale 1 (0x90) nota N velocity 127 e spegne l'eventuale LED
precedente (velocity 0). Ctrl-C per fermare.
"""
import argparse
import glob
import os
import re
import select
import sys


def find_gboard():
    """Trova la rawmidi della G-Board dal nome della card in /proc/asound/cards."""
    try:
        text = open("/proc/asound/cards").read()
    except OSError:
        return None
    m = re.search(r"(\d+)\s+\[(V\d{3}|G_Boar|iCON)[^\]]*\]:", text)
    if not m:
        return None
    return "/dev/snd/midiC%sD0" % m.group(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device", default="")
    args = ap.parse_args()
    dev = args.device or find_gboard()
    if not dev or not os.path.exists(dev):
        print("G-Board non trovata; passa --device /dev/snd/midiC2D0", file=sys.stderr)
        return 1
    print("[led] %s — PC 0-7 -> LED 0-7, Ctrl-C per fermare" % dev, file=sys.stderr)
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)
    status = None
    msg = []
    cur = None  # LED attualmente acceso
    try:
        while True:
            ready, _, _ = select.select([fd], [], [], 0.5)
            if not ready:
                continue
            for b in os.read(fd, 256):
                if b & 0x80:
                    status = b
                    msg = [b]
                elif status is not None:
                    msg.append(b)
                kind = status & 0xF0 if status is not None else 0
                need = 1 if kind in (0xC0, 0xD0) else 2
                if status is None or len(msg) != need + 1:
                    continue
                if kind == 0xC0:  # Program Change
                    pc = msg[1]
                    if 0 <= pc <= 7:
                        if cur is not None:
                            os.write(fd, bytes([0x90, cur, 0]))
                        os.write(fd, bytes([0x90, pc, 127]))
                        cur = pc
                        print("[led] PC %d -> LED %d (nota %d) acceso" % (pc, pc, pc),
                              flush=True)
                    else:
                        print("[led] PC %d ignorato (solo 0-7)" % pc, flush=True)
                msg = [status] if (status is not None and status < 0xF0) else []
    except KeyboardInterrupt:
        pass
    finally:
        # spegni tutto prima di uscire
        try:
            for n in range(8):
                os.write(fd, bytes([0x90, n, 0]))
        except OSError:
            pass
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
