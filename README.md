# namzero-lite

Motore audio realtime per chitarra su Raspberry Pi Zero 2W con XSONIC XTONE come
interfaccia audio di default (ora **configurabile dalla web UI**): modelli NAM
*lite*, catena a slot fissi con bypass, controllo MIDI da footswitch.

Il piano di progetto completo è in [PLAN.md](PLAN.md). Dettagli su
[preset e parametri](docs/presets.md), [controllo MIDI](docs/midi.md),
[bridge BLE MIDI](docs/ble.md), [G-Board](docs/g-board.md) e [tuning del sistema](docs/rt-setup.md).

**Stato**: catena completa e suonabile sul Pi, preset a slot, XTONE + G-Board
(PC 0-7 = preset, CC 50-57 = effetti, LED) + Chocolate via BLE. Il MIDI è su
mappa fissa in `midi.json` (il learn è in roadmap); la GUI di gestione dei
preset è il prossimo passo.

## Cosa c'è già

**Audio**
- I/O ALSA full-duplex su XTONE (default) o su qualunque PCM ALSA scelto dalla
  UI; periodo e periodi configurabili (default 64 frame @ 48 kHz)
- **Interfaccia audio configurabile** in Impostazioni: elenco device da
  `aplay -l`, PCM manuale, buffer 16..2048, periodi 2..8, canale d'ingresso 0/1,
  canale di uscita (stereo L/R oppure mono su canale 1/2), **Applica** = salva
  in `presets.json` + reload. Rate interno **fisso 48 kHz**
  (per device solo-44.1k si usa `plughw:CARD,DEV`). Se il device non si apre:
  fallback `default` -> `hw:XTONE`, poi modalita' offline senza crash loop, cosi'
  la UI puo' correggere ([docs/web.md](docs/web.md))
- Thread realtime: `SCHED_FIFO`, `mlockall`, nessuna allocazione nel loop; recupero
  automatico dagli xrun e statistiche
- Catena a slot fissi con bypass in crossfade di 5 ms (anti-click)
- Gate e comp applicati davvero dai parametri; **gate globale** in `presets.json`
  (soglia bassa e release lungo, per non tagliare le code)
- Drive: `fuzz`, `klon` (blend col pulito), `tubescreamer`. Il fuzz ha
  l'**Octave** stile Octavia (raddrizzamento a onda intera + passa-alto, non un
  pitch shifter) con **interruttore on/off separato dal mix 0..1**: da spento il
  mix resta salvato e lo slider non ha effetto; ignorato da klon e tubescreamer
- Filter: **wah** o **whammy** (pitch shifter granulare) fisso **prima del
  drive**, acceso solo dall'auto-engage del pedale. Il whammy trasporta da 0 a
  `range` semitoni (-24..+24, default +12) seguendo l'espressione, con mix
  dry/wet; cambio tipo/parametri senza click e latenza dichiarata ~30 ms.
- Mod: tremolo, univibe, flanger, phaser, in posizione `pre_amp` o `post_ir`
- Delay a tre modi (stereo, ping-pong, ping-pong stereo) e riverbero
- Boost di volume a fine catena in dB assoluti (default +6 dB), da CC 44 o tasto 8 del G-Board
- **EQ globale a 5 bande** (rack "Opzioni generali"): low shelf 80 Hz, peaking
  250/800/2.5k, high shelf 8k, -12..+12 dB; in fondo alla catena, salvato in
  `presets.json` con **Salva opzioni**
- **Volume globale** nello stesso rack (-24..+24 dB, default +6 dB): gain master
  sempre applicato, anche a EQ spento, si somma al boost
- **Looper** a fine catena con **Autostart** opzionale (`looper.autostart` in
  `presets.json`, default spento): con il flag ON il tasto REC da fermo mette il
  looper in attesa (`waiting`) e la registrazione parte alla prima nota, quando
  l'ingresso supera la soglia RMS per due blocchi consecutivi
  (`looper.autostart_threshold_db`, default -42 dBFS). Con il flag OFF REC
  registra subito, come sempre
- **Accordatore** sul segnale **dry** (prima della catena): rilevatore
  YIN/autocorrelazione a ~15 Hz su finestre da 4096 campioni, nota/ottava/cent e
  confidenza; riferimento A4 415..450 Hz e transpose -6..+6 in `presets.json`;
  **Muto** con fade ~4 ms per accordare in silenzio. Modale grande nella web UI,
  aggiornata via SSE. Costo misurato sul Pi: ~1,0 ms per analisi (~1,5% di un
  core a 15 Hz) ([docs/web.md](docs/web.md))
- Tasti `+`/`-` per il volume dell'ampli (salvato nel preset con `s`)

**Preset**
- `config/presets.json`: 8 slot (0-7) verso i file, opzioni globali (gate,
  boost, auto-engage, EQ, volume, tuner, looper), `start_slot`
- Combo preset nella web UI (lista da `GET /api/presets`); tasti `0`-`9`,
  navigazione blu dell'XTONE e, in **Program mode**, PC 0-7 della G-Board per
  cambiare slot; banner `[N] - file.json (modello.nam)` a ogni caricamento
- Salvataggio con `s` (on/off e trim dell'ampli), altri parametri intatti
- Verifica dei preset con `--print-config` e `scripts/verify_presets.py`

**MIDI**
- XTONE: tre gruppi (A verde, B blu, C rosso) con CC diversi; espressione
  ascoltata su **CC 7/4/1** (tutti i gruppi); navigazione preset su CC 22/24/25
- **iCON G-Board**: 8 pulsanti e 8 LED. In **Control/CC mode** i pulsanti 1-4
  sono mod, 5-7 drive, 8 boost; in **Program mode** i PC 0-7 selezionano gli
  slot 0-7 (re-exec, `.last_slot` e flash LED). LED aggiornati in tempo reale,
  note invariate ([mappa completa](docs/g-board.md))
- Selezioni radio spegnibili per mod, drive e delay (via CC; i PC-effetto
  legacy restano solo con `gboard.pc_effects: true` e `preset_pc: false`)
- **M-Vave Chocolate via BLE**: bridge Python separato (systemd) che scrive sulla
  socket del motore ([docs/ble.md](docs/ble.md))
- Ingressi multipli configurabili (`midi.json` → `devices`)
- Tutta la mappa in `config/midi.json`, modificabile senza ricompilare

**Sistema**
- **Web UI** locale (porta 8080): pedaliera con manopole live, stato
  bidirezionale via **SSE** (`GET /api/events`, polling come fallback),
  accordatore modale a 10-20 Hz, salvataggio preset, upload NAM/IR e sezione
  **Interfaccia audio** con stato del device/fallback ([docs/web.md](docs/web.md))
- **WiFi**: reti note con priorità e **AP di fallback** (`namzero-wifi`). Se
  all'avvio nessuna rete nota è raggiungibile entro ~10 s avvia un hotspot
  WPA2 con DHCP condiviso; in AP ricontrolla ogni ~50 s e torna in STA appena
  ricompare una rete nota (una sola radio). Stato e credenziali AP in
  Impostazioni ([docs/web.md](docs/web.md))
- Servizi systemd per motore, bridge e web UI, governor persistente, capability
  (`install.sh`); G-Board e bridge sono opzionali
- Build sul Pi (`build.sh`) o **cross-compilazione dal Mac** in container
  (`cross-build.sh` + `deploy.sh`)
- Test: C++ (ctest, incluso `tuner_test` sul rilevatore), stress runtime
  (`test/ctl_stress.py`: ctl + SSE), test runtime G-Board
  (`test/gboard_cc_runtime.py`, `test/gboard_pc_runtime.py`), test audio web
  (`test/audio_web_test.py`: parsing `aplay -l`, validazione, API) e runtime
  (`test/audio_runtime.py`: salvataggio + reload sul Pi), decoder bridge
  Python, verifica preset, decisioni WiFi/AP (`test/wifi_manager_test.py`,
  dry-run senza `nmcli`)

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

Dopo un deploy fatto senza sudo, `systemctl` non può fermare/riavviare il
servizio (NOPASSWD copre solo `setcap`). Per i test si avvia il motore a mano,
dalla directory del repo:

```bash
cd ~/namzero-lite
setsid nohup ./build/namzero-lite >/tmp/namzero-lite.log 2>&1 </dev/null &
```

La web UI si riavvia con `pkill -f "[w]eb/server.py"` (systemd la rilancia subito
col codice nuovo). Quando si torna alla gestione systemd:
`sudo systemctl restart namzero-lite`.

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

# interfaccia audio da riga di comando (override dell'avvio, non persistito)
./build/namzero-lite --device plughw:XTONE --buffer 128 --periods 4 --input 0 --output 0
```

Statistiche ogni 5 secondi: blocchi elaborati, xrun, tempo massimo di
elaborazione del blocco.

## Opzioni

| Opzione | Default | Note |
|---------|---------|------|
| `--device` | `hw:XTONE` | PCM ALSA (capture e playback sullo stesso device); vince su `presets.json` |
| `--rate` | `48000` | legacy: il motore usa sempre 48000 Hz |
| `--buffer` | `64` | frame per periodo, 16..2048 |
| `--periods` | `3` | periodi nel buffer, 2..8 |
| `--input` | `0` | canale di cattura usato come ingresso (0 = L, 1 = R) |
| `--output` | `0` | uscita: 0 = stereo L/R, 1 = mono sul canale 1, 2 = mono sul canale 2 |
| `--gain` | `0.0` | dB sul master |
| `--prio` | `80` | priorità FIFO del thread audio |
| `--no-rt` | off | non impostare priorità realtime |
| `-p, --preset` | — | carica un preset (altrimenti slot mode) |
| `--midi-log` | off | stampa ogni messaggio MIDI ricevuto |
| `--print-config` | off | stampa i valori effettivi e esce (verifica) |

## Tuning del sistema

Vedi [docs/rt-setup.md](docs/rt-setup.md) e `scripts/setup_pi.sh`.
