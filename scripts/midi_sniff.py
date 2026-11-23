#!/usr/bin/env python3
"""Sniffer MIDI standalone: legge un device ALSA rawmidi e decodifica su stdout.

Uso:
  scripts/midi_sniff.py                 # lista i device e chiede/prende il primo
  scripts/midi_sniff.py --list
  scripts/midi_sniff.py hw:2,0          # oppure midiC2D0 / /dev/snd/midiC2D0
Ctrl-C per fermare. Non serve il motore: i device devono essere liberi.
"""
import argparse
import glob
import os
import re
import select
import sys
import time

NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def card_names():
    names = {}
    try:
        for line in open("/proc/asound/cards"):
            m = re.match(r"\s*(\d+)\s+\[([^\]]+)\]:\s*(.*)", line)
            if m:
                names[int(m.group(1))] = m.group(3).strip() or m.group(2)
    except OSError:
        pass
    return names


def list_devices():
    names = card_names()
    out = []
    for path in sorted(glob.glob("/dev/snd/midiC*D*")):
        m = re.match(r".*midiC(\d+)D(\d+)", path)
        if not m:
            continue
        card, dev = int(m.group(1)), int(m.group(2))
        out.append((path, f"hw:{card},{dev}", names.get(card, f"card{card}")))
    return out


def resolve(arg):
    if arg.startswith("/"):
        return arg
    m = re.match(r"(?:hw:)?(\d+)[,:](\d+)$", arg) or re.match(r"midiC(\d+)D(\d+)$", arg)
    if m:
        return f"/dev/snd/midiC{m.group(1)}D{m.group(2)}"
    return None


def decode(msg):
    status, data = msg[0], msg[1:]
    ch = status & 0x0F
    kind = status & 0xF0
    if kind == 0x90 and data[1] > 0:
        return f"Note On  ch{ch+1} {NOTE[data[0] % 12]}{data[0]//12-1} vel {data[1]}"
    if kind == 0x80 or (kind == 0x90 and data[1] == 0):
        return f"Note Off ch{ch+1} {NOTE[data[0] % 12]}{data[0]//12-1} vel {data[1]}"
    if kind == 0xB0:
        return f"CC       ch{ch+1} cc {data[0]:3d} val {data[1]:3d}"
    if kind == 0xC0:
        return f"PC       ch{ch+1} program {data[0]}"
    if kind == 0xE0:
        return f"Bend     ch{ch+1} {data[1] * 128 + data[0] - 8192:+d}"
    return "Other    " + " ".join(f"{b:02X}" for b in msg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device", nargs="?")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    devs = list_devices()
    if args.list:
        for path, hw, name in devs:
            print(f"{hw:10s} {path}  {name}")
        return 0
    if not devs:
        print("nessun device MIDI ALSA trovato", file=sys.stderr)
        return 1
    if args.device:
        path = resolve(args.device)
        if not path or not os.path.exists(path):
            print(f"device non valido: {args.device}", file=sys.stderr)
            return 1
    else:
        print("device disponibili:", file=sys.stderr)
        for path, hw, name in devs:
            print(f"  {hw:10s} {name}", file=sys.stderr)
        path = devs[0][0]
    print(f"[sniff] {path}  (Ctrl-C per fermare)", file=sys.stderr)
    fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    size = {0x80: 2, 0x90: 2, 0xA0: 2, 0xB0: 2, 0xC0: 1, 0xD0: 1, 0xE0: 2}
    status, msg = None, []
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 0.5)
            if not r:
                continue
            chunk = os.read(fd, 256)
            for b in chunk:
                if b & 0x80:
                    status = b
                    msg = [b]
                    if size.get(b & 0xF0) == 1:
                        print(f"{time.strftime('%H:%M:%S')} {decode(msg)}   [{msg[0]:02X} {msg[1]:02X}]", flush=True)
                        msg = []
                    continue
                if status is None:
                    continue
                msg.append(b)
                if len(msg) == size.get(status & 0xF0, 0) + 1:
                    print(f"{time.strftime('%H:%M:%S')} {decode(msg)}   [{' '.join(f'{x:02X}' for x in msg)}]", flush=True)
                    msg = [status] if status < 0xF0 else []
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
