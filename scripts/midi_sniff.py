#!/usr/bin/env python3
"""Sniffer MIDI standalone: legge TUTTI i device ALSA rawmidi e decodifica.

Uso:
  scripts/midi_sniff.py                 # apre tutti i device
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
    if args.device:
        path = resolve(args.device)
        devs = [d for d in devs if d[0] == path]
        if not devs:
            print(f"device non valido: {args.device}", file=sys.stderr)
            return 1
    if not devs:
        print("nessun device MIDI ALSA trovato", file=sys.stderr)
        return 1
    fds = {}
    for path, hw, name in devs:
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            fds[fd] = {"label": f"{hw} {name}", "status": None, "msg": []}
            print(f"[sniff] {hw} {name}", file=sys.stderr)
        except OSError as e:
            print(f"[sniff] {hw} {name}: non apribile ({e.strerror}) - occupato?", file=sys.stderr)
    if not fds:
        return 1
    size = {0x80: 2, 0x90: 2, 0xA0: 2, 0xB0: 2, 0xC0: 1, 0xD0: 1, 0xE0: 2}
    try:
        while True:
            ready, _, _ = select.select(list(fds), [], [], 0.5)
            for fd in ready:
                st = fds[fd]
                try:
                    chunk = os.read(fd, 256)
                except OSError:
                    continue
                for b in chunk:
                    if b & 0x80:
                        st["status"] = b
                        st["msg"] = [b]
                        if size.get(b & 0xF0) == 1:
                            print(f"{time.strftime('%H:%M:%S')} [{st['label']}] {decode(st['msg'])}   [{' '.join(f'{x:02X}' for x in st['msg'])}]", flush=True)
                            st["msg"] = []
                        continue
                    if st["status"] is None:
                        continue
                    st["msg"].append(b)
                    if len(st["msg"]) == size.get(st["status"] & 0xF0, 0) + 1:
                        print(f"{time.strftime('%H:%M:%S')} [{st['label']}] {decode(st['msg'])}   [{' '.join(f'{x:02X}' for x in st['msg'])}]", flush=True)
                        st["msg"] = [st["status"]] if st["status"] < 0xF0 else []
    except KeyboardInterrupt:
        pass
    finally:
        for fd in fds:
            os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
