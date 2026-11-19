# namzero-lite

Motore audio realtime per chitarra su Raspberry Pi Zero 2W con XSONIC XTONE come
interfaccia audio: modelli NAM *lite*, catena a slot fissi con bypass, controllo
MIDI da footswitch.

Il piano di progetto completo è in [PLAN.md](PLAN.md). Dettagli su
[preset e parametri](docs/presets.md), [controllo MIDI](docs/midi.md),
[bridge BLE MIDI](docs/ble.md) e [tuning del sistema](docs/rt-setup.md).

**Stato**: catena completa e suonabile sul Pi (M2). Gli effetti ci sono tutti; il MIDI è
sulla mappa fissa dei CC, non sul learn.

## Cosa c'è già

- I/O ALSA full-duplex, periodo configurabile (default 64 frame @ 48 kHz)
- Thread realtime: `SCHED_FIFO`, `mlockall`, nessuna allocazione nel loop
- Framework a slot con bypass e **crossfade di 5 ms** (anti-click)
- Slot: gate, comp, drive, mod, amp NAM, IR, delay, riverbero
- Drive con tre modelli: `fuzz` (storico), `klon` (trasparente, blend col pulito) e
  `tubescreamer` (medi in avanti, basse strette)
- Mod: tremolo, univibe, flanger, phaser (ciclabili con `w`) e wah; il wah è spento
  finché non lo accende il pedale (auto-engage)
- Delay con tre modi (stereo, ping-pong, ping-pong stereo), ciclabili con `p`
- Tasti `+`/`-` per il volume dell'ampli; boost di volume a fine catena via MIDI
- Selezioni radio via CC per modulazioni, drive e delay (un tasto per scelta,
  ripremendo lo stesso si spegne)
- Preset JSON (`-p`) e salvataggio dello stato con `s`
- MIDI da XTONE: mappa in [config/midi.json](config/midi.json) — CC di ogni slot,
  espressione e auto-engage — uguale per tutti i preset, modificabile senza ricompilare
  ([dettagli](docs/midi.md))
- Contatore xrun con recupero automatico e statistiche periodiche

## Cosa manca (roadmap in PLAN.md)

Scelta dei preset dal vivo, MIDI learn, UI. Vedi PLAN.md per il dettaglio.

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
scripts/build.sh
```

`scripts/build.sh` compila **e** applica `cap_sys_nice,cap_ipc_lock` all'eseguibile,
così si suona senza sudo. Una build fatta a mano con `cmake` è identica ma **perde
le capability**: il programma parte lo stesso e avvisa *"priorità realtime non
disponibile"*, con più probabilità di xrun a buffer piccoli. In quel caso:

```bash
sudo setcap cap_sys_nice,cap_ipc_lock+ep build/namzero-lite
```

## Uso

```bash
# elenco dispositivi
aplay -l; arecord -l

# con le capability applicate da scripts/build.sh: niente sudo
./build/namzero-lite --device hw:XTONE --buffer 64 --rate 48000

# senza privilegi realtime (solo per test a buffer grandi)
./build/namzero-lite --device hw:XTONE --buffer 256 --no-rt
```

Statistiche ogni 5 secondi: blocchi elaborati, xrun, tempo massimo di
elaborazione del blocco.

## Opzioni

| Opzione | Default | Note |
|---------|---------|------|
| `--device` | `hw:XTONE` | PCM ALSA (capture e playback sullo stesso device) |
| `--rate` | `48000` | Hz |
| `--buffer` | `64` | frame per periodo |
| `--periods` | `3` | periodi nel buffer |
| `--gain` | `0.0` | dB sul master |
| `--prio` | `80` | priorità FIFO del thread audio |
| `--no-rt` | off | non impostare priorità realtime |

## Tuning del sistema

Vedi [docs/rt-setup.md](docs/rt-setup.md) e `scripts/setup_pi.sh`.
