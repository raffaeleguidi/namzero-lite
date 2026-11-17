# namzero-lite

Motore audio realtime per chitarra su Raspberry Pi Zero 2W con XSONIC XTONE come
interfaccia audio: modelli NAM *lite*, catena a slot fissi con bypass, controllo
MIDI da footswitch.

Il piano di progetto completo è in [PLAN.md](PLAN.md).

**Stato**: M0/M2 in corso. Lo scheletro compila ed è eseguibile ma è ancora
privo di effetti, core NAM e MIDI.

## Cosa c'è già

- I/O ALSA full-duplex, periodo configurabile (default 64 frame @ 48 kHz)
- Thread realtime: `SCHED_FIFO`, `mlockall`, nessuna allocazione nel loop
- Framework a slot con bypass e **crossfade di 5 ms** (anti-click)
- Slot di gain di esempio + master gain con soft clip
- Contatore xrun con recupero automatico e statistiche periodiche

## Cosa manca (roadmap in PLAN.md)

Slot IR, gate, comp, drive, mod, delay, reverb · core NAM · preset · MIDI learn.

## Connessione al Pi

La rete non espone mDNS: `namzero-lite.local` **non risolve**. Il nome viene risolto dal
router (Fritz!Box):

```bash
ssh pi@namzero-lite            # nome semplice, fornito dal Fritz!Box
ssh pi@namzero-lite.fritz.box  # FQDN del router, se il primo non basta
```

Copia del progetto dal Mac:

```bash
rsync -a --exclude build --exclude .git ~/Projects/namzero-lite/ pi@namzero-lite:~/namzero-lite/
```

## Build (sul Pi)

```bash
sudo apt install -y build-essential cmake libasound2-dev
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

## Uso

```bash
# elenco dispositivi
aplay -l; arecord -l

# passthrough con periodo 64 frame sull'XTONE
sudo taskset -c 3 ./build/namzero-lite --device hw:XTONE --buffer 64 --rate 48000

# senza privilegi realtime (solo per test a buffer grandi)
./build/namzero-lite --device hw:XTONE --buffer 256 --no-rt
```

Statistiche ogni 5 secondi: blocchi elaborati, xrun, tempo massimo di
elaborazione del blocco.

## Opzioni

| Opzione | Default | Note |
|---------|---------|------|
| `--device` | `hw:1,0` | PCM ALSA (capture e playback sullo stesso device) |
| `--rate` | `48000` | Hz |
| `--buffer` | `64` | frame per periodo |
| `--periods` | `3` | periodi nel buffer |
| `--gain` | `0.0` | dB sul master |
| `--prio` | `80` | priorità FIFO del thread audio |
| `--no-rt` | off | non impostare priorità realtime |

## Tuning del sistema

Vedi [docs/rt-setup.md](docs/rt-setup.md) e `scripts/setup_pi.sh`.
