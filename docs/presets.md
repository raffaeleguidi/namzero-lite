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
                "level_db": 0, "blend": 0.40, "mid_db": 4.0, "low_db": 5.0,
                "octave": 0.0, "octave_on": false },
    "filter": { "on": false, "type": "wah", "range": 12, "rate_hz": 1.0,
                "depth": 0.5, "q": 3.0, "mix": 1.0,
                "low_hz": 350, "high_hz": 2200, "position": 0.5 },
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
`--periods` e `--input`; questi override valgono per l'avvio corrente e **non**
vengono persistiti.

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
| `octave` | 0.0 | solo fuzz: 0..1, quota di ottava stile Octavia |
| `octave_on` | false | solo fuzz: interruttore dell'ottava, separato dal mix |

`octave` raddrizza il fuzz a onda intera, toglie la continua con un passa-alto a
200 Hz e miscela il risultato al fuzz: non è un pitch shifter, è l'armonica a
2f tipica dell'Octavia. A `0` il fuzz suona come prima; a `1` si sente solo
l'ottava. Il parametro è significativo solo con `type: "fuzz"`: con `klon` e
`tubescreamer` viene ignorato ma resta salvato. I preset senza `octave` valgono
`0` (migrazione automatica).

`octave_on` è l'interruttore: l'ottava si applica **solo** con
`octave_on: true` e `type: "fuzz"`. Con `octave_on: false` il ramo è bypassato
anche se `octave` è 1: il valore del mix resta salvato e torna effettivo appena
si riaccende, senza essere azzerato. I preset vecchi senza la chiave partono
spenti (`false`), conservando il mix che avevano. In catena lo spegnimento fa
un fade di ~8 ms, quindi non produce click. Da web UI il mix è la manopola
Octave e l'interruttore è il toggle Octave ON/OFF accanto; via socket si
comandano con `param` `key:"octave"` e `key:"octave_on"` (bool, con clamp;
`set` accetta anche `octave_on`).

### filter (wah / whammy)

Blocco fisso **prima del drive**: e' una seconda istanza di `ModSlot`, sempre
presente in catena, con due tipi:

- `wah` (default): filtro a stato variabile comandato dal pedale;
- `whammy`: pitch shifter granulare comandato dal pedale. Posizione 0 = unisono,
  posizione 1 = `range` semitoni (positivo = piu' acuto, negativo = piu' grave,
  ammesso -24..+24).

Non ha un interruttore: `on` viene forzato a `false` in lettura e riscritto
`false` in salvataggio. Si accende solo con l'auto-engage dell'espressione (vedi
`docs/midi.md`), che vale per entrambi i tipi: quando il pedale supera la soglia
il filter entra in catena e il tipo scelto decide cosa fa.

| Chiave | Default | Note |
|--------|---------|------|
| `on` | false | ignorato: il filter nei preset e' sempre spento |
| `type` | `wah` | `wah` o `whammy`; i preset vecchi senza questa chiave restano wah |
| `range` | 12 | solo whammy: semitoni a pedale a fondo, -24..+24 |
| `rate_hz` | 1.0 | non usato dal wah ne' dal whammy, esposto per completezza |
| `depth` | 0.5 | non usato dal wah ne' dal whammy |
| `q` | 3.0 | solo wah: risonanza (il Q effettivo scende verso la punta) |
| `mix` | 1.0 | quanto bagnato; vale sia per il wah sia per il whammy (1 = 100% wet) |
| `low_hz` / `high_hz` | 350 / 2200 | solo wah: estremi dello sweep |
| `position` | 0.5 | posizione del pedale 0..1 (per il whammy: 0 = unisono, 1 = range) |

Se il blocco manca valgono i default; `filter` e' comunque in catena e spento.
`type` viene validato: un valore sconosciuto torna a `wah` con un avviso su
stderr. Anche `range` viene limitato a -24..+24 in lettura, nei comandi live e
nel salvataggio.

Il pitch shifter del whammy usa un buffer circolare di ~120 ms, due testine di
lettura con crossfade di ~30 ms e interpolazione lineare; il rapporto e' letto da
un atomico e smussato, quindi muovere il pedale non produce click. La latenza
dichiarata e' la finestra di crossfade (~30 ms a 48 kHz): si somma alla latenza
della catena solo quando il filter e' acceso e il tipo e' whammy.

Come ogni pitch shifter a due testine, il crossfade non e' un vocoder: su una
sinusoide di 440 Hz a +12 semitoni il picco spettrale e' a ~873 Hz invece di 880
(~-0.8%), mentre a 0 semitoni l'unisono e' esatto. E' il compromesso tipico di
questo algoritmo (un leggero "warble" da granular); per l'intonazione perfetta
servirebbe un pitch shifter in frequenza, molto piu' costoso. Il test unitario
`pitch_shifter_test` misura frequenza, unisono, mix dry e cambio tipo.

Nota sull'auto-engage: la sua polarita' e' separata da `filter.position` e vive
in `midi.json` (`wah_autoengage.invert`) oppure nel blocco `autoengage` di
`presets.json`. Con `invert: true` (il valore attuale della config viva) il
filter si accende verso il **tacco**, come il wah; per il whammy conviene
`invert: false`, altrimenti il filter si spegne (dopo `release_ms`) proprio
mentre il pedale sale di tonalita'. Non e' una regola del motore: e' una scelta
della mappa, e il tipo `wah`/`whammy` non la cambia.

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
| `audio` | `device`, `buffer`, `periods`, `input` | interfaccia audio usata dal motore |
| `gate` | `threshold_db`, `attack_ms`, `release_ms` | parametri del gate globale |
| `autoengage` | `threshold`, `release_threshold`, `engage_ms`, `release_ms` | auto-engage del wah |
| `boost` | `on`, `db` | boost di fine catena |
| `eq` | `on`, `gains`, `volume_db` | EQ grafico globale a 5 bande + volume globale |
| `tuner` | `reference_hz`, `transpose` | accordatore: riferimento del La4 e trasposizione della lettura |

### audio

Interfaccia audio globale, usata dal motore all'avvio e modificabile dalla web
UI (**Impostazioni → Interfaccia audio → Applica**). E' l'ultimo elemento di
sistema configurabile senza ricompilare.

```json
"audio": {
  "device": "hw:XTONE",
  "buffer": 64,
  "periods": 3,
  "input": 0
}
```

| Chiave | Default | Note |
|--------|---------|------|
| `device` | `hw:XTONE` | PCM ALSA, per **nome** (`hw:XTONE`, `hw:CARD=XTONE,DEV=0`, `plughw:...`, `default`) |
| `buffer` | 64 | frame per periodo, 16..2048 (clamp) |
| `periods` | 3 | periodi nel buffer, 2..8 (clamp) |
| `input` | 0 | canale di cattura usato come ingresso: 0 = L, 1 = R (0..1) |

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

## Cosa non e' un preset

- **La mappa MIDI** sta in `config/midi.json` e vale per tutti i preset. Un
  blocco `"midi"` dentro un preset viene ignorato con un avviso.
- **Il salvataggio con `s`** (o il bottone *Salva preset* della web UI) riscrive
  tutto lo stato live: on/off e parametri degli slot, `mod.position`, `filter`
  (tipo, range, mix e sempre `on: false`). Nome e chiavi sconosciute restano
  intatti.
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
