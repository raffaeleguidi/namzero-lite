# namzero-lite

Motore audio realtime per chitarra su Raspberry Pi Zero 2W con XSONIC XTONE come
interfaccia audio: modelli NAM *lite*, catena a slot fissi con bypass, controllo
MIDI da footswitch.

Il piano di progetto completo è in [PLAN.md](PLAN.md). Dettagli su
[preset e parametri](docs/presets.md), [controllo MIDI](docs/midi.md),
[bridge BLE MIDI](docs/ble.md), [G-Board](docs/g-board.md) e [tuning del sistema](docs/rt-setup.md).

**Stato**: catena completa e suonabile sul Pi, preset a slot, XTONE + G-Board
(PC e LED) + Chocolate via BLE. Il MIDI è su mappa fissa in `midi.json` (il
learn è in roadmap); la GUI di gestione dei preset è il prossimo passo.

## Cosa c'è già

**Audio**
- I/O ALSA full-duplex su XTONE, periodo configurabile (default 64 frame @ 48 kHz)
- Thread realtime: `SCHED_FIFO`, `mlockall`, nessuna allocazione nel loop; recupero
  automatico dagli xrun e statistiche
- Catena a slot fissi con bypass in crossfade di 5 ms (anti-click)
- Gate e comp applicati davvero dai parametri; **gate globale** in `presets.json`
  (soglia bassa e release lungo, per non tagliare le code)
- Drive: `fuzz`, `klon` (blend col pulito), `tubescreamer`
- Mod: tremolo, univibe, flanger, phaser e wah (auto-engage dal pedale)
- Delay a tre modi (stereo, ping-pong, ping-pong stereo) e riverbero
- Boost di volume a fine catena in dB assoluti (default +6 dB), da CC 44 o tasto 8 del G-Board
- Tasti `+`/`-` per il volume dell'ampli (salvato nel preset con `s`)

**Preset**
- `config/presets.json`: 8 slot (0-7) verso i file, gate globale, `start_slot`
- Tasti `0`-`9` e navigazione blu dell'XTONE per cambiare slot; banner
  `[N] - file.json (modello.nam)` a ogni caricamento
- Salvataggio con `s` (on/off e trim dell'ampli), altri parametri intatti
- Verifica dei preset con `--print-config` e `scripts/verify_presets.py`

**MIDI**
- XTONE: tre gruppi (A verde, B blu, C rosso) con CC diversi; espressione
  ascoltata su **CC 7/4/1** (tutti i gruppi); navigazione preset su CC 22/24/25
- **iCON G-Board**: 8 pulsanti e 8 LED; pulsanti 1-4 mod, 5-7 drive, 8 boost;
  LED aggiornati in tempo reale e lampeggio dello slot al cambio preset
  ([mappa completa](docs/g-board.md))
- Selezioni radio spegnibili per mod, drive e delay (via CC o PC)
- **M-Vave Chocolate via BLE**: bridge Python separato (systemd) che scrive sulla
  socket del motore ([docs/ble.md](docs/ble.md))
- Ingressi multipli configurabili (`midi.json` → `devices`)
- Tutta la mappa in `config/midi.json`, modificabile senza ricompilare

**Sistema**
- **Web UI** locale (porta 8080): pedaliera con manopole live, stato
  bidirezionale, salvataggio preset e upload NAM/IR ([docs/web.md](docs/web.md))
- Servizi systemd per motore, bridge e web UI, governor persistente, capability
  (`install.sh`); G-Board e bridge sono opzionali
- Build sul Pi (`build.sh`) o **cross-compilazione dal Mac** in container
  (`cross-build.sh` + `deploy.sh`)
- Test: C++ (ctest), decoder bridge Python, verifica preset

## Prossimi passi

- **GUI di gestione** (web locale sul Pi o app desktop): elenco degli slot,
  editor dei parametri del preset, **upload di modelli NAM e file IR**, salvataggio
  di `presets.json` e anteprima dei file disponibili.
- **MIDI learn** per assegnare i CC dal pedale invece della mappa fissa.
- Cambio preset **a caldo** (~200 ms) al posto del ri-exec attuale (~300 ms).
- LED per gli altri slot (gate/comp/delay/reverb) quando ci saranno LED liberi.

Dettaglio e milestone in [PLAN.md](PLAN.md).

## Build

Sul Pi:

```bash
sudo apt install -y build-essential cmake libasound2-dev
./scripts/build.sh        # compila e testa (niente sudo)
./scripts/install.sh      # capability, governor, servizi (sudo)
```

Dal Mac (cross-compilazione aarch64 in container, più veloce e senza OOM):

```bash
./scripts/cross-build.sh  # -> build-arm64/namzero-lite
./scripts/deploy.sh       # copia binario + config e riapplica setcap
```

`build.sh` non usa sudo; `install.sh` applica `cap_sys_nice,cap_ipc_lock`
all'eseguibile (così si suona senza sudo), mette il governor su `performance` e
installa la unit del bridge BLE. Una build fatta a mano con `cmake` è identica ma
**perde le capability**: il programma parte lo stesso e avvisa *"priorità
realtime non disponibile"*, con più probabilità di xrun a buffer piccoli. In quel
caso basta `./scripts/install.sh`.

## Uso

```bash
# elenco dispositivi
aplay -l; arecord -l

# slot mode: parte dall'ultimo slot usato (config/.last_slot)
./build/namzero-lite

# un preset specifico
./build/namzero-lite -p config/presets/vox_wah.json

# vedere i messaggi MIDI mentre si prova
./build/namzero-lite --midi-log

# senza privilegi realtime (solo per test a buffer grandi)
./build/namzero-lite --no-rt --buffer 256
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
| `-p, --preset` | — | carica un preset (altrimenti slot mode) |
| `--midi-log` | off | stampa ogni messaggio MIDI ricevuto |
| `--print-config` | off | stampa i valori effettivi e esce (verifica) |

## Tuning del sistema

Vedi [docs/rt-setup.md](docs/rt-setup.md) e `scripts/setup_pi.sh`.
