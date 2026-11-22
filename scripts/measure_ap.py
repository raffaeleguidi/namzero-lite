#!/usr/bin/env python3
"""Misura CPU/xrun del motore durante un test AP (hotspot) o STA.

Uso tipico (prima di uscire, l'AP parte da solo fuori casa):
    nohup python3 scripts/measure_ap.py --seconds 1800 >/tmp/ap-bench.txt 2>&1 &
Poi, al rientro, `cat /tmp/ap-bench.txt`.

Opzioni:
    --seconds N     durata (default 900)
    --interval S    periodo di campionamento (default 2)
    --log FILE      log del motore se gira a mano (default: journalctl del servizio)
    --out FILE      riepilogo anche su file (default: solo stdout)

Raccoglie: %CPU del motore (da /proc), load, temperatura, stato WiFi (STA/AP,
SSID, client AP), xrun e max blocco dal log (righe "attenzione: ... xrun").
Nessun sudo, nessuna modifica alla rete.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time


def sh(cmd, timeout=3):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except Exception:
        return ""


def engine_pid():
    out = sh(["pgrep", "-x", "namzero-lite"])
    return int(out.splitlines()[0]) if out else None


def cpu_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            return sum(int(x) for x in f.read().split()[13:15])
    except OSError:
        return None


def temp_c():
    t = sh(["vcgencmd", "measure_temp"])
    m = re.search(r"([\d.]+)", t)
    return float(m.group(1)) if m else None


def wifi_state():
    out = sh(["nmcli", "-t", "-f", "DEVICE,STATE,CONNECTION", "device", "status"])
    mode, ssid = "none", ""
    for line in out.splitlines():
        parts = line.split(":")
        if parts and parts[0] == "wlan0":
            mode = parts[1] if len(parts) > 1 else "?"
            ssid = ":".join(parts[2:]) if len(parts) > 2 else ""
    ap = mode.lower().startswith("connected") and "ap" in ssid.lower()
    clients = 0
    if ap:
        clients = len([l for l in sh(["iw", "dev", "wlan0", "station", "dump"]).splitlines()
                       if l.strip().startswith("Station")])
    return ("ap" if ap else "sta" if mode.lower().startswith("connected") else mode.lower()), ssid, clients


def engine_log_lines(since_epoch, log_path):
    if log_path:
        try:
            with open(log_path, errors="replace") as f:
                return f.readlines()
        except OSError:
            return []
    out = sh(["journalctl", "-u", "namzero-lite", "--since", f"@{int(since_epoch)}",
              "--no-pager"], timeout=10)
    return out.splitlines()


def parse_xruns(lines):
    total, max_block, max_pct, last_riepilogo = 0, 0.0, 0, None
    for line in lines:
        m = re.search(r"\+(\d+) xrun \| max blocco ([\d.]+) ms \((\d+)%", line)
        if m:
            total += int(m.group(1))
            max_block = max(max_block, float(m.group(2)))
            max_pct = max(max_pct, int(m.group(3)))
        m = re.search(r"riepilogo: (\d+) blocchi, (\d+) xrun", line)
        if m:
            last_riepilogo = (int(m.group(1)), int(m.group(2)))
    return total, max_block, max_pct, last_riepilogo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=900)
    ap.add_argument("--interval", type=float, default=2.0)
    ap.add_argument("--log", default="")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    start = time.time()
    pid = engine_pid()
    hz = os.sysconf("SC_CLK_TCK")
    t0 = cpu_ticks(pid) if pid else None
    s0 = sum(int(x) for x in open("/proc/stat").readline().split()[1:])
    samples = []
    modes = {}
    print(f"# misura AP/audio: {args.seconds}s, motore pid={pid}", flush=True)
    while time.time() - start < args.seconds:
        time.sleep(args.interval)
        pid2 = engine_pid()
        cpu = 0.0
        if pid2:
            t = cpu_ticks(pid2)
            if t is not None and t0 is not None:
                cpu = 100.0 * (t - t0) / hz / (time.time() - start)
        mode, ssid, clients = wifi_state()
        modes[mode] = modes.get(mode, 0) + 1
        load = os.getloadavg()[0]
        temp = temp_c()
        samples.append((time.time() - start, cpu, load, temp, mode, ssid, clients))
        if len(samples) % 15 == 0:
            print(f"t={samples[-1][0]:5.0f}s cpu={cpu:4.1f}% load={load:.2f} "
                  f"temp={temp if temp is not None else '-'}°C wifi={mode}/{ssid}", flush=True)

    total, max_block, max_pct, riep = parse_xruns(engine_log_lines(start, args.log))
    cpus = [s[1] for s in samples]
    temps = [s[3] for s in samples if s[3] is not None]
    lines = [
        "",
        "== RIEPILOGO AP BENCH ==",
        f"durata: {time.time() - start:.0f}s  campioni: {len(samples)}",
        f"motore: cpu media {sum(cpus)/len(cpus):.1f}%  max {max(cpus):.1f}%",
        f"load medio: {sum(s[2] for s in samples)/len(samples):.2f}  "
        f"temp max: {max(temps) if temps else '-'}°C",
        f"wifi: {modes}",
        f"xrun nel periodo: {total}  max blocco: {max_block:.2f} ms ({max_pct}% del periodo)",
    ]
    if riep:
        lines.append(f"riepilogo motore: {riep[0]} blocchi, {riep[1]} xrun totali dal suo avvio")
    if total == 0:
        lines.append("esito: NESSUN XRUN nel periodo")
    elif max_pct >= 100:
        lines.append("esito: xrun da sovraccarico (blocco >= periodo)")
    else:
        lines.append("esito: xrun occasionali, controlla i picchi")
    out = "\n".join(lines)
    print(out, flush=True)
    if args.out:
        with open(args.out, "a") as f:
            f.write(out + "\n")
    print("\nConsiglio: prova con AP acceso e telefono collegato che usa la web; "
          "poi riprova in STA e confronta.", flush=True)


if __name__ == "__main__":
    sys.exit(main())
