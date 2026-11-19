#!/usr/bin/env python3
"""Verifica che ogni preset arrivi davvero negli slot.

Per ogni file in config/presets/ lancia il binario con --print-config, che stampa
i valori letti DAGLI SLOT (non dal Config letto dal JSON: la differenza e' il bug
che ci ha fatto perdere mezz'ora col gate). Confronta solo le chiavi presenti nel
preset, cosi' i default restano liberi di cambiare.

    ./scripts/verify_presets.py [percorso-binario]

Esce con 1 se c'e' almeno una discrepanza.
"""

import glob
import json
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./build/namzero-lite"
TOL = 1e-3

# (chiave nel dump, percorso nel preset, tipo)
MAPPING = [
    ("cfg.gate.threshold_db", ("slots", "gate", "threshold_db"), float),
    ("cfg.gate.attack_ms", ("slots", "gate", "attack_ms"), float),
    ("cfg.gate.release_ms", ("slots", "gate", "release_ms"), float),

    ("cfg.comp.threshold_db", ("slots", "comp", "threshold_db"), float),
    ("cfg.comp.ratio", ("slots", "comp", "ratio"), float),
    ("cfg.comp.attack_ms", ("slots", "comp", "attack_ms"), float),
    ("cfg.comp.release_ms", ("slots", "comp", "release_ms"), float),
    ("cfg.comp.makeup_db", ("slots", "comp", "makeup_db"), float),

    ("cfg.drive.type", ("slots", "drive", "type"), str),
    ("cfg.drive.amount", ("slots", "drive", "amount"), float),
    ("cfg.drive.tone", ("slots", "drive", "tone"), float),
    ("cfg.drive.level_db", ("slots", "drive", "level_db"), float),
    ("cfg.drive.blend", ("slots", "drive", "blend"), float),
    ("cfg.drive.mid_db", ("slots", "drive", "mid_db"), float),
    ("cfg.drive.low_db", ("slots", "drive", "low_db"), float),

    ("cfg.mod.type", ("slots", "mod", "type"), str),
    ("cfg.mod.rate_hz", ("slots", "mod", "rate_hz"), float),
    ("cfg.mod.depth", ("slots", "mod", "depth"), float),
    ("cfg.mod.position", ("slots", "mod", "position"), float),
    ("cfg.mod.q", ("slots", "mod", "q"), float),
    ("cfg.mod.mix", ("slots", "mod", "mix"), float),
    ("cfg.mod.low_hz", ("slots", "mod", "low_hz"), float),
    ("cfg.mod.high_hz", ("slots", "mod", "high_hz"), float),
    ("cfg.mod.fx_mix", ("slots", "mod", "fx_mix"), float),
    ("cfg.mod.feedback", ("slots", "mod", "feedback"), float),

    ("cfg.amp.file", ("slots", "amp", "file"), str),
    ("cfg.amp.slim", ("slots", "amp", "slim"), float),
    ("cfg.amp.trim_db", ("slots", "amp", "trim_db"), float),

    ("cfg.ir.file", ("slots", "ir", "file"), str),
    ("cfg.ir.taps", ("slots", "ir", "taps"), int),

    ("cfg.delay.time_ms", ("slots", "delay", "time_ms"), float),
    ("cfg.delay.feedback", ("slots", "delay", "feedback"), float),
    ("cfg.delay.mix", ("slots", "delay", "mix"), float),
    ("cfg.delay.tone", ("slots", "delay", "tone"), float),
    ("cfg.delay.mode", ("slots", "delay", "mode"), str),

    ("cfg.reverb.mix", ("slots", "reverb", "mix"), float),
    ("cfg.reverb.size", ("slots", "reverb", "size"), float),
    ("cfg.reverb.damp", ("slots", "reverb", "damp"), float),
]


def dig(obj, path):
    for key in path:
        if not isinstance(obj, dict) or key not in obj:
            return None
        obj = obj[key]
    return obj


def active_amp(preset):
    amp = dig(preset, ("slots", "amp")) or {}
    return bool(amp.get("on")) and bool(amp.get("file"))


def active_ir(preset):
    ir = dig(preset, ("slots", "ir")) or {}
    return bool(ir.get("on")) and bool(ir.get("file"))


def parse_dump(text):
    values = {}
    for line in text.splitlines():
        if line.startswith("cfg."):
            key, _, value = line.partition("=")
            values[key] = value
    return values


def check_preset(path, preset):
    proc = subprocess.run(
        [BIN, "-p", path, "--print-config"],
        capture_output=True, text=True, timeout=120,
    )
    if proc.returncode != 0:
        return [f"il binario esce con {proc.returncode}: {proc.stderr.strip()[:300]}"]

    values = parse_dump(proc.stdout)
    if not values:
        return ["nessuna riga cfg. nell'output (--print-config non supportato?)"]

    problems = []
    checked = 0
    for dump_key, preset_path, kind in MAPPING:
        expected = dig(preset, preset_path)
        if expected is None:
            continue
        # amp e ir esistono come slot solo se attivi e con file: altrimenti non
        # c'e' niente da applicare e i valori del preset restano dichiarativi.
        if preset_path[1] == "amp" and not active_amp(preset):
            continue
        if preset_path[1] == "ir" and not active_ir(preset):
            continue

        checked += 1
        if dump_key not in values:
            problems.append(f"{dump_key}: manca nel dump")
            continue

        if kind is str:
            if values[dump_key] != str(expected):
                problems.append(f"{dump_key}: atteso {expected!r}, slot ha {values[dump_key]!r}")
        elif kind is int:
            if int(values[dump_key]) != int(expected):
                problems.append(f"{dump_key}: atteso {expected}, slot ha {values[dump_key]}")
        else:
            got = float(values[dump_key])
            if abs(got - float(expected)) > TOL:
                problems.append(f"{dump_key}: atteso {expected}, slot ha {got:.4f}")

    return checked, problems


def main():
    presets = sorted(glob.glob("config/presets/*.json"))
    if not presets:
        print("nessun preset in config/presets/", file=sys.stderr)
        return 1

    failures = 0
    for path in presets:
        with open(path, encoding="utf-8") as handle:
            preset = json.load(handle)

        result = check_preset(path, preset)
        if isinstance(result, list):
            print(f"FAIL {path}")
            for problem in result:
                print(f"     {problem}")
            failures += 1
            continue

        checked, problems = result
        if problems:
            print(f"FAIL {path} ({checked} parametri)")
            for problem in problems:
                print(f"     {problem}")
            failures += 1
        else:
            print(f"ok   {path} ({checked} parametri)")

    print()
    if failures:
        print(f"{failures} preset con problemi")
        return 1
    print(f"tutti i {len(presets)} preset applicano i parametri agli slot")
    return 0


if __name__ == "__main__":
    sys.exit(main())
