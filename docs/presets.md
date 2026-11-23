# Preset

Un preset e' un file JSON in `config/presets/` che descrive lo stato degli slot
e il comportamento del motore. L'**interfaccia audio** non e' piu' una scelta del
singolo preset: vive nel blocco `audio` di `config/presets.json` (vedi sotto), e'
globale e si applica a tutti i preset. Un preset si carica con `-p`/`--preset`;
i flag da riga di comando e `-p` sono alternativi (mescolarli renderebbe ambiguo
chi vince, e i due percorsi divergerebbero in silenzio).

Il preset viene letto **una volta sola, all'avvio**: modificare il file con il
programma in esecuzione non cambia niente. Per sentire le modifiche va riavviato.

## Schema

Tutte le chiavi sono opzionali: quello che manca resta al default. Il file di
esempio piu' completo e' `config/presets/vox_delay.json`.

```json
{
  "version": 1,
  "name": "VOX AC30 + tremolo + delay",
  "slots": {
    "gate":   { "on": true,  "threshold_db": -55, "attack_ms": 2, "release_ms": 120 },
    "comp":   { "on": false, "threshold_db": -18, "ratio": 3,
                "attack_ms": 10, "release_ms": 150, "makeup_db": 3 },
    "drive":  { "on": false, "type": "fuzz", "amount": 0.15, "tone": 0.5,
                "level_db": 0, "blend": 0.40, "mid_db": 4.0, "low_db": 5.0 },
    "filter": { "on": false, "rate_hz": 1.0, "depth": 0.5, "q": 3.0,
                "mix": 1.0, "low_hz": 350, "high_hz": 2200,
                "position": 0.5 },
    "mod":    { "on": true,  "type": "tremolo", "rate_hz": 4.5, "depth": 0.5,
                "fx_mix": 0.5, "feedback": 0.35, "position": "pre_amp" },
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

### audio (compatibilita' nel singolo preset)

I file preset possono ancora contenere un blocco `audio`: viene usato solo
avviando con `-p` **senza** blocco `audio` in `config/presets.json` (per esempio
un preset portabile). Con la config viva normale il blocco globale vince su
`device`, `period_frames` e `periods`.

| Chiave | Default | Note |
|--------|---------|------|
| `device` | `hw:XTONE` | PCM ALSA, **per nome** (l'indice cambia al boot) |
| `rate` | 48000 | **ignorato**: il motore usa sempre 48000 Hz |
| `period_frames` | 64 | frame per periodo |
| `periods` | 3 | periodi nel buffer |
| `gain_db` | 0.0 | gain master, dopo la catena |

Da riga di comando gli stessi valori si passano con `--device`, `--buffer`,
`--periods`, `--input` e `--output`; questi override valgono per l'avvio corrente
e **non** vengono persistiti.

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

L'ottava del fuzz (`octave`/`octave_on`) è stata rimossa: un preset che la
contiene non si rompe, i due campi vengono ignorati con un avviso su stderr e
spariscono al primo salvataggio.

### filter (wah)

Blocco fisso **prima del drive**: è una seconda istanza di `ModSlot`, sempre
presente in catena, con il solo tipo **wah** (filtro a stato variabile comandato
dal pedale).

Il tipo non è più configurabile. Un preset vecchio con `type: "whammy"` o con
`range` non fa fallire il caricamento: il filter resta wah e i due campi vengono
ignorati con un avviso su stderr. Al primo salvataggio vengono rimossi dal file.

Non ha un interruttore: `on` viene forzato a `false` in lettura e riscritto
`false` in salvataggio. Si accende solo con l'auto-engage dell'espressione (vedi
`docs/midi.md`).

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | ignorato: il filter nei preset è sempre spento |
| `rate_hz` | 1.0 | non usato dal wah, esposto per completezza |
| `depth` | 0.5 | non usato dal wah |
| `q` | 3.0 | risonanza (il Q effettivo scende verso la punta) |
| `mix` | 1.0 | quanto bagnato (1 = 100% wet) |
| `low_hz` / `high_hz` | 350 / 2200 | estremi dello sweep |
| `position` | 0.5 | posizione del pedale 0..1 |

Se il blocco manca valgono i default; `filter` è comunque in catena e spento.

Nota sull'auto-engage: la sua polarità è separata da `filter.position` e vive in
`midi.json` (`wah_autoengage.invert`) oppure nel blocco `autoengage` di
`presets.json`. Con `invert: true` (il valore attuale della config viva) il
filter si accende verso il **tacco**, come il wah. Non è una regola del motore:
è una scelta della mappa.

### mod

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | |
| `type` | `tremolo` | `tremolo`, `univibe`, `flanger`, `phaser` |
| `rate_hz` | 4.5 | LFO di tremolo/univibe/flanger/phaser |
| `depth` | 0.6 | 0..1 |
| `position` | `pre_amp` | `pre_amp` (default) o `post_ir`: dove sta il mod nella catena |
| `fx_mix` | 0.5 | quota di wet |
| `feedback` | 0.35 | solo flanger/phaser |

Il **wah non e' piu' un tipo di mod**: sta nel blocco `filter`. Un preset vecchio
con `mod.type: "wah"` viene letto come tremolo spento, senza fallire.

`mod.position` si applica **al caricamento del preset**, perche' la catena si
costruisce all'avvio. Dal vivo il comando `set` con `slot:"mod"` e `position`
salva il preset e lo ricarica (~300 ms), come il cambio di un file amp/IR. Un
`position` numerico nei preset vecchi (era la posizione del pedale wah) viene
ignorato.

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

## Opzioni generali in presets.json

`config/presets.json` non è solo la mappa degli slot: contiene anche le opzioni
globali, valide per tutti i preset. Le scrive la web UI con **Salva opzioni**
(comando `save_options`); il tasto `s` aggiorna le stesse chiavi quando salva un
preset. Il deploy **non** sovrascrive questo file.

| Blocco | Chiavi | Note |
|--------|---------|------|
| `audio` | `device`, `buffer`, `periods`, `input`, `output` | interfaccia audio usata dal motore |
| `gate` | `threshold_db`, `attack_ms`, `release_ms` | parametri del gate globale |
| `autoengage` | `threshold`, `release_threshold`, `engage_ms`, `release_ms` | auto-engage del wah |
| `boost` | `on`, `db` | boost di fine catena |
| `eq` | `on`, `gains`, `volume_db` | EQ grafico globale a 5 bande + volume globale |
| `tuner` | `reference_hz`, `transpose` | accordatore: riferimento del La4 e trasposizione della lettura |
| `looper` | `autostart`, `autostart_threshold_db`, `max_seconds` | looper: attesa della prima nota e durata massima |

### audio

Interfaccia audio globale, usata dal motore all'avvio e modificabile dalla web
UI (**Impostazioni → Interfaccia audio → Applica**). E' l'ultimo elemento di
sistema configurabile senza ricompilare.

```json
"audio": {
  "device": "hw:XTONE",
  "buffer": 64,
  "periods": 3,
  "input": 0,
  "output": 0
}
```

| Chiave | Default | Note |
|--------|---------|------|
| `device` | `hw:XTONE` | PCM ALSA, per **nome** (`hw:XTONE`, `hw:CARD=XTONE,DEV=0`, `plughw:...`, `default`) |
| `buffer` | 64 | frame per periodo, 16..2048 (clamp) |
| `periods` | 3 | periodi nel buffer, 2..8 (clamp) |
| `input` | 0 | canale di cattura usato come ingresso: 0 = L, 1 = R (0..1) |
| `output` | 0 | canale di uscita: 0 = stereo L/R, 1 = mono sul canale 1, 2 = mono sul canale 2 (0..2) |

Con `output` 1/2 il loop di playback fa il mix mono `(L+R)/2`, lo scrive sul
canale scelto e azzera l'altro; con 0 l'uscita resta lo stereo della catena.

Il **rate non e' configurabile**: tutta la catena (NAM, IR, delay, accordatore)
assume **48000 Hz**. Un device che accetta solo 44.1 kHz si indica con
`plughw:CARD,DEV` (es. `plughw:XTONE`): ALSA converte, il motore resta a 48 kHz.
Il valore `rate` eventualmente presente nei vecchi preset viene ignorato con un
avviso.

**Apertura e fallback**: il motore prova il device configurato, poi `default`,
poi `hw:XTONE` (i doppioni si saltano). Se uno dei successivi si apre, il motore
suona e lo stato riporta `audio.fallback: true` con il device reale. Se **nessuno**
si apre, il motore resta attivo **senza audio** (modalita' offline): la web UI
resta raggiungibile, `/api/state` mostra `audio.open: false` con l'errore e da
Impostazioni si puo' correggere il device e premere **Applica**. Senza questa
modalita' systemd riavvierebbe il motore in loop e non ci sarebbe modo di
rimandare il reload dalla UI.

La scrittura e' fatta dalla web UI (`POST /api/audio`) in modo atomico (file
temporaneo + `os.replace`), con backup `config/presets.json.bak`. **Salva
opzioni** (comando `save_options`) e il tasto `s` riscrivono lo stesso blocco
quando salvano le altre opzioni globali.

### eq

EQ grafico **globale**, sempre presente in catena in fondo (dopo riverbero e
looper, prima del gain master/boost). A 0 dB è piatto e trasparente.

| Chiave | Default | Note |
|--------|---------|------|
| `on` | true | on/off dell'intero blocco |
| `gains` | `[0,0,0,0,0]` | dB per banda, in ordine: 80, 250, 800, 2500, 8000 Hz |
| `volume_db` | **+6.0** | gain master globale, -24..+24 dB; si applica **sempre**, anche a EQ spento, e si somma al boost |

Le bande sono un low shelf a 80 Hz, tre peaking a 250/800/2500 Hz (Q 0.7) e un
high shelf a 8000 Hz. I guadagni stanno fra -12 e +12 dB (passo 0.5). Se il
blocco `eq` manca valgono i default (acceso, piatto, +6 dB): è così che la
config viva esistente continua a funzionare dopo l'aggiornamento.

Il volume globale è indipendente da `eq.on`: il gain master è
`10^(gain_db/20) * 10^(volume_db/20) * (boost ? 10^(boost_db/20) : 1)`.
Lo stato live espone `eq.volume` (dB) e `master_gain` (lineare), utile ai test.

Dal vivo il comando `{"cmd":"eq","on":bool}` accende/spegne, mentre
`{"cmd":"eq","band":0..4,"gain":dB}` cambia una banda e
`{"cmd":"eq","volume":dB}` regola il volume globale, senza ricaricare il preset.
Per renderli permanenti serve **Salva opzioni** (o `s`): il motore li rilegge da
`presets.json` al prossimo avvio. `defaults` con `slot:"eq"` riporta anche il
volume a +6 dB.

### tuner

Blocco dell'accordatore, valido per tutti i preset. **Non** è un effetto della
catena: il motore analizza sempre il segnale **dry** (input prima di
gate/comp/filter/drive/...), quindi l'accordatore funziona anche con il preset
distorto o con l'uscita muta.

```json
"tuner": {
  "reference_hz": 440.0,
  "transpose": 0
}
```

| Chiave | Default | Note |
|--------|---------|------|
| `reference_hz` | **440.0** | frequenza del La4, 415.0..450.0 Hz (clamp); il valore 415.3 è il La4 un semitono sotto |
| `transpose` | **0** | semitoni, -6..+6; cambia **solo la lettura della nota**, non il suono |

Il riferimento effettivo usato per nota e cent è
`reference_hz * 2^(transpose/12)`. Due modi equivalenti per chi suona un semitono
sotto: `transpose: -1` con riferimento 440, oppure `reference_hz: 415.3` con
transpose 0. In entrambi i casi un La4 reale a 415.3 Hz viene letto come **A4 a
0 cent**; con `transpose: -1` e 440 Hz reali la lettura è **A#4 0 cent**.

Nota sulla gamma: la specifica iniziale indicava 430..450 Hz, ma chiedeva anche
che 415.3 Hz fosse utilizzabile; il limite inferiore effettivo è quindi 415.0 Hz.
Tutti i valori fuori intervallo vengono clampati dal motore e dalla UI.

`on` e `mute` **non** stanno in `presets.json`: dopo un riavvio (o un cambio
preset, che fa re-exec) l'accordatore riparte spento e non muto. È voluto: non
si deve restare in silenzio senza capire perché. I comandi live sono
`{"cmd":"tuner","on":bool}`, `{"cmd":"tuner","mute":bool}`,
`{"cmd":"tuner","reference_hz":float}` e `{"cmd":"tuner","transpose":int}`;
**Salva opzioni** scrive solo `reference_hz` e `transpose`. Se il blocco manca
valgono i default (440 / 0), quindi la config viva esistente continua a
funzionare dopo l'aggiornamento.

### looper

Opzioni del looper a fine catena, valide per tutti i preset. Il buffer è
preallocato all'avvio e `max_seconds` ne fissa la durata massima.

```json
"looper": {
  "autostart": false,
  "autostart_threshold_db": -42.0,
  "max_seconds": 60
}
```

| Chiave | Default | Note |
|--------|---------|------|
| `autostart` | **false** | con `true`, ● da fermo con loop vuoto mette il looper in **attesa** e la registrazione parte alla prima nota invece di partire subito |
| `autostart_threshold_db` | **-42.0** | soglia RMS dell'ingresso dello slot (dBFS), -90..0 (clamp); il trigger richiede due blocchi consecutivi sopra soglia |
| `max_seconds` | **60** | durata massima del loop in secondi, 1..120 (clamp); il buffer stereo è allocato in `prepare()` |

Con `autostart` **false** il comportamento è quello storico: ● da fermo
registra subito. Con `autostart` **true** lo stato live diventa `armed`
(`waiting` nella web UI) e non registra finché l'ingresso non supera la soglia;
un secondo tocco su ● annulla, ■ e ⌫ tornano a fermo. Se il blocco manca
valgono i default, quindi la config viva esistente continua a funzionare dopo
l'aggiornamento.

Dal vivo `{"cmd":"looper","action":"toggle|stop|clear"}` comanda il looper,
`{"cmd":"looper","autostart":bool}` e
`{"cmd":"looper","threshold_db":float}` cambiano le opzioni senza
ricaricare il preset (`autostart_threshold_db` è accettato come alias).
**Salva opzioni** (o `s`) scrive il blocco `looper`. Lo stato live espone
`looper.state` (`idle`, `rec`, `play`, `dub`, `armed`), `pos`, `len`, `max`,
`armed` (solo overdub quantizzato) e `autostart`, `threshold_db` (con
l'alias `autostart_threshold_db`).

## Cosa non e' un preset

- **La mappa MIDI** sta in `config/midi.json` e vale per tutti i preset. Un
  blocco `"midi"` dentro un preset viene ignorato con un avviso.
- **Il salvataggio con `s`** (o il bottone *Salva preset* della web UI) riscrive
  tutto lo stato live: on/off e parametri degli slot, `mod.position`, `filter`
  (parametri wah e sempre `on: false`). Nome e chiavi sconosciute restano
  intatti; i campi delle feature rimosse (`drive.octave`, `filter.type`,
  `filter.range`) vengono cancellati.
- **Il preset non si ricarica da solo**: vale quanto detto sopra, si legge
  all'avvio.

## Verifica che i parametri arrivino davvero

Il modo per non fidarsi (ci e' gia' sfuggito una volta: gate e comp leggevano i
valori dal JSON ma non li applicavano allo slot, che restava sui default):

```bash
./build/namzero-lite -p config/presets/vox_clean.json --print-config
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
