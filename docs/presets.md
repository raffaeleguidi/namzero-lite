# Preset

Un preset e' un file JSON in `config/presets/` che descrive **tutto** lo stato del
motore: dispositivo audio, parametri di ogni slot, comportamento. Si carica con
`-p`/`--preset`; i flag da riga di comando e `-p` sono alternativi (mescolarli
renderebbe ambiguo chi vince, e i due percorsi divergerebbero in silenzio).

Il preset viene letto **una volta sola, all'avvio**: modificare il file con il
programma in esecuzione non cambia niente. Per sentire le modifiche va riavviato.

## Schema

Tutte le chiavi sono opzionali: quello che manca resta al default. Il file di
esempio piu' completo e' `config/presets/vox_delay.json`.

```json
{
  "version": 1,
  "name": "VOX AC30 + tremolo + delay",
  "audio": { "device": "hw:XTONE", "rate": 48000,
             "period_frames": 64, "periods": 3, "gain_db": 0.0 },
  "slots": {
    "gate":   { "on": true,  "threshold_db": -55, "attack_ms": 2, "release_ms": 120 },
    "comp":   { "on": false, "threshold_db": -18, "ratio": 3,
                "attack_ms": 10, "release_ms": 150, "makeup_db": 3 },
    "drive":  { "on": false, "type": "fuzz", "amount": 0.15, "tone": 0.5,
                "level_db": 0, "blend": 0.40, "mid_db": 4.0, "low_db": 5.0 },
    "mod":    { "on": true,  "type": "tremolo", "rate_hz": 4.5, "depth": 0.5,
                "position": 0.5, "q": 3.0, "mix": 1.0,
                "low_hz": 350, "high_hz": 2200, "fx_mix": 0.5, "feedback": 0.35 },
    "amp":    { "on": true,  "file": "models/vox_ac30_a2.nam",
                "slim": 0.49, "trim_db": -6 },
    "ir":     { "on": false, "file": "", "taps": 2048 },
    "delay":  { "on": true,  "time_ms": 380, "feedback": 0.32, "mix": 0.22,
                "tone": 0.45, "mode": "stereo" },
    "reverb": { "on": true,  "mix": 0.2, "size": 0.5, "damp": 0.45 }
  },
  "stats": false
}
```

### audio

| Chiave | Default | Note |
|--------|---------|------|
| `device` | `hw:XTONE` | PCM ALSA, **per nome** (l'indice cambia al boot) |
| `rate` | 48000 | Hz |
| `period_frames` | 64 | frame per periodo |
| `periods` | 3 | periodi nel buffer |
| `gain_db` | 0.0 | gain master, dopo la catena |

### gate

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `threshold_db` | -55 | si apre sopra; si chiude sotto il 60% (isteresi ~4.4 dB) |
| `attack_ms` | 2 | vale per inviluppo e rampa di apertura |
| `release_ms` | 120 | vale per inviluppo e chiusura: lungo = tiene le code |

### comp

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `threshold_db` | -18 | |
| `ratio` | 3 | |
| `attack_ms` / `release_ms` | 10 / 150 | |
| `makeup_db` | 3 | |

### drive

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `type` | `fuzz` | `fuzz`, `klon`, `tubescreamer` |
| `amount` | 0.15 | 0..1, quanto spinge |
| `tone` | 0.5 | 0..1, filtro dopo il clipping |
| `level_db` | 0 | |
| `blend` | 0.40 | solo klon: quota di saturo nel mix |
| `mid_db` | 4.0 | solo klon: gobba a 800 Hz |
| `low_db` | 5.0 | solo fuzz: low-shelf a 110 Hz |

### mod

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `type` | `tremolo` | `tremolo`, `univibe`, `flanger`, `phaser`, `wah` |
| `rate_hz` | 4.5 | LFO di tremolo/univibe/flanger/phaser |
| `depth` | 0.6 | 0..1 |
| `position` | 0.5 | solo wah: posizione del pedale 0..1 |
| `q` | 3.0 | solo wah: risonanza (il Q effettivo scende verso la punta) |
| `mix` | 1.0 | solo wah: quanto bagnato |
| `low_hz` / `high_hz` | 350 / 2200 | solo wah: estremi dello sweep |
| `fx_mix` | 0.5 | solo univibe/flanger/phaser: quota di wet |
| `feedback` | 0.35 | solo flanger/phaser |

Il wah **non** si accende da preset: all'avvio, con MIDI attivo, viene forzato
spento e lo comanda solo il pedale (vedi `docs/midi.md`). Il tipo `wah` in un
preset serve a scegliere i parametri del filtro, non ad accenderlo.

### amp / ir

| Chiave | Default | Note |
|--------|---------|------|
| `amp.file` | vuoto | modello `.nam`; se manca, lo slot e' un gain muto |
| `amp.slim` | 0.49 | sotto 0.5 il modello puo' usare il sottomodello lite |
| `amp.trim_db` | 0 | trim sopra al gain dichiarato dal modello |
| `ir.file` | vuoto | WAV della cassa |
| `ir.taps` | 2048 | limite di tap della convoluzione |

`amp` e `ir` vengono applicati solo se `on` e `file` sono valorizzati: altrimenti
lo slot non esiste nella catena (o e' un passthrough) e i parametri restano
dichiarativi.

### delay

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `time_ms` | 350 | 1 ms..2 s |
| `feedback` | 0.35 | 0..0.95 |
| `mix` | 0.25 | |
| `tone` | 0.5 | filtro nel loop |
| `mode` | `stereo` | `stereo`, `ping_pong`, `ping_pong_stereo`; dal vivo si cicla con `p` |

### reverb

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `mix` | 0.25 | |
| `size` | 0.55 | |
| `damp` | 0.45 | |

### stats

`true` stampa ogni 5 s blocchi elaborati, xrun e tempo massimo per blocco.
Default: silenzioso, parla solo se qualcosa non va.

## Cosa non e' un preset

- **La mappa MIDI** sta in `config/midi.json` e vale per tutti i preset. Un
  blocco `"midi"` dentro un preset viene ignorato con un avviso.
- **Il salvataggio con `s`** riscrive i campi `on` degli slot e il volume
  dell'ampli (`slots.amp.trim_db`, regolato con `+` e `-`): nome, tipi, altri
  parametri e chiavi sconosciute restano intatti. Non salva gli altri cambi fatti
  dal vivo (modo del delay, tap tempo, tipo di drive/mod).
- **Il preset non si ricarica da solo**: vale quanto detto sopra, si legge
  all'avvio.

## Verifica che i parametri arrivino davvero

Il modo per non fidarsi (ci e' gia' sfuggito una volta: gate e comp leggevano i
valori dal JSON ma non li applicavano allo slot, che restava sui default):

```bash
./build/namzero-lite -p config/presets/vox_wah.json --print-config
```

Stampa `cfg.<slot>.<parametro>=<valore>` leggendo **dagli slot**, non dal Config
del parser. Il confronto con i file lo fa lo script:

```bash
python3 scripts/verify_presets.py            # usa ./build/namzero-lite
python3 scripts/verify_presets.py /percorso/binario
```

Per ogni preset controlla solo le chiavi presenti nel file (i default sono liberi
di cambiare), con tolleranza 1e-3 sui float, e dice quali parametri non sono
arrivati. Esce con 1 se qualcosa non torna. `--print-config` non apre il device
audio e non prende il lock: si puo' lanciare anche mentre si suona.

Per aggiungere un effetto nuovo: getter sullo slot, riga nella stampa di
`--print-config`, riga nella `MAPPING` dello script. Cosi' il controllo resta
automatico invece di essere un'ispezione a occhio.

## Sorgente di verità della config

Da quando la web UI salva i preset, **la config viva è quella sul Pi**: lui è la
sorgente di verità, il repo si allinea al Pi e non il contrario.

- `scripts/deploy.sh` **non sovrascrive** `config/` (preset, `presets.json`,
  `midi.json`): aggiunge solo i file nuovi. Il flag `PUSH_CONFIG=1` esiste ma si
  usa **solo su richiesta esplicita**.
- Gli allineamenti dovuti a cambi di struttura (nuovi campi, migrazioni) si fanno
  **puntualmente**, con modifiche mirate a entrambi i lati.
- Per riportare nel repo la config del Pi (senza i segreti WiFi/TONE3000):
  `rsync pi@<pi>:~/namzero-lite/config/presets.json config/` , `presets/` e
  `midi.json`. `config/wifi.json` e `config/tone3000.json` restano solo sul Pi.
