# namzero-lite — Piano di progetto

> Trasposizione funzionale di *coyopedal* (NAM lite + switching MIDI + effetti integrati) su Raspberry Pi Zero 2W con XSONIC XTONE alimentato esternamente come interfaccia audio.
>
> **Stato:** piano approvato, in attesa di M0 (setup hardware).
> **Versione:** 1.0 — 2026-11-17

---

## 1. Obiettivo

Riprodurre l'esperienza d'uso del coyopedal — modello NAM, catena di effetti a slot fissi con bypass, controllo da footswitch MIDI — su hardware diverso e molto più limitato, senza degradare la sensazione di suonare.

**Requisiti funzionali**

| # | Requisito |
|---|-----------|
| F1 | Motore NAM con modelli *lite* (WaveNet lite o LSTM) a 48 kHz |
| F2 | Catena a 8 slot fissi: `gate → comp → drive → mod → amp → ir → delay → reverb` |
| F3 | Bypass indipendente per ogni slot, con transizione senza click |
| F4 | Preset a schema costante (slot e parametri sempre gli stessi) |
| F5 | 3 footswitch XTONE su 3 banchi (blu/verde/rossa) = 9 binding, assegnabili via MIDI learn |
| F6 | Expression pedal → wah (auto-engage su soglia), opzionale |
| F7 | Cambio preset secco, non live; nessun crossfade richiesto |
| F8 | Persistenza di preset e mappa MIDI tra riavvii |

**Non-obiettivi (v1)**

- Modelli NAM standard / WaveNet pesanti.
- Automazione continua di parametri via BLE (jitter 10–30 ms: inadatto).
- Cambio preset durante l'esecuzione live.
- Editor grafico completo; al massimo una UI web minimale in M9.

---

## 2. Vincoli e componenti

### Raspberry Pi Zero 2W
- SoC BCM2710A1: 4× Cortex-A53 @ 1 GHz, 512 MB LPDDR2.
- WiFi 802.11n 2.4 GHz + BT 4.2 sullo **stesso chip** (CYW43438): radio condivisa, non usare WiFi e BT insieme durante il playing.
- Un solo port micro-USB dati (OTG) + alimentazione separata.
- Nessuna ethernet: il deploy iniziale richiede WiFi o un adattatore USB.

### XSONIC XTONE
- Interfaccia audio USB, alimentazione esterna (non grava sul Pi).
- 3 footswitch fisici, expression pedal, 3 configurazioni/banchi (blu, verde, rossa).
- **Verificato in M0**: USB high speed, full-duplex su `card 1, device 0`.
  Playback S32_LE (24 bit) o S16_LE, 2 canali, 44,1–192 kHz, endpoint asincrono.
  Capture S32_LE (24 bit), 2 canali, stesse frequenze. Guitar in e mic sono i due canali.
  MIDI: port `hw:1,0,0` (`XTONE MIDI 1`), CC su canale 1 — mappa completa nella sezione 6.
  **Ancora ignoto**: se i LED siano pilotabili dal host.

### Budget di latenza
- 48 kHz, periodi da 64 frame → 1,33 ms per blocco.
- Round-trip atteso con XTONE: **8–12 ms**, accettabile per chitarra.
- Obiettivo: nessun xrun in 10 minuti continui a 64 frame.
- Formato: S32_LE con 24 bit utili. I campioni arrivano allineati a sinistra, quindi
  `float = int32 / 2^31` è la conversione corretta in entrambe le direzioni.

---

## 3. Architettura software

Un solo processo, due thread, nessun lock nel path audio.

```
        ┌────────────────────────── processo namzero-lite ──────────────────────────┐
        │                                                                           │
 ALSA   │  ┌──────────── Thread audio (CPU3, FIFO 80, mlockall) ────────────────┐   │
h/w in ─┼─►│ readi → gate → comp → drive → mod → amp → ir → delay → reverb →    │──►│─ h/w out
        │  │        → out gain + soft clip                                    │   │
        │  │  legge comandi da ring SPSC, parametri da atomici                │   │
        │  └──────────────────────────────────────────────────────────────────┘   │
        │            ▲ ring buffer SPSC          ▲ atomic<shared_ptr<Preset>>       │
        │  ┌─────────┴──────── Thread controllo (CPU0) ───────────────────────┐    │
        │  │ MIDI (ALSA seq) · learn · caricamento preset/modelli · watchdog  │    │
        │  │ xrun · log · (M9) HTTP UI                                        │    │
        │  └──────────────────────────────────────────────────────────────────┘    │
        └───────────────────────────────────────────────────────────────────────────┘
```

**Regole non negoziabili del thread audio**

- Nessuna allocazione, nessun `new`/`malloc`, nessuna I/O, nessun lock.
- Solo `snd_pcm_readi`/`writei` (o mmap+poll) e accessi ad array preallocati.
- Buffer di lavoro allocati all'avvio per la dimensione massima prevista.
- Cambio preset e cambio modello NAM: preparati dal thread controllo in strutture inattive, pubblicati con swap atomico al confine di blocco.
- Watchdog: contatore xrun; dopo N xrun consecutivi → `snd_pcm_prepare` e log, senza uscire dal processo.

---

## 4. Catena a slot fissi

Ordine immutabile. Ogni slot ha uno stato `on/off` e un set di parametri a schema costante.

| Slot | Contenuto v1 | Parametri principali | Note |
|------|--------------|----------------------|------|
| 1. gate | noise gate | threshold, attack, release, isteresi | envelope follower, no lookahead |
| 2. comp | compressore | threshold, ratio, attack, release, makeup | feed-forward |
| 3. drive | boost / overdrive / fuzz | tipo: `fuzz` (storico) o `klon`; drive, tone, level, blend | il Klon mescola il pulito: trasparente |
| 4. mod | modulazione | tipo: tremolo / wah / phaser / chorus / auto-wah | wah = port di GxWah (Guitarix, GPL) |
| 5. amp | NAM lite | file modello, input gain, output gain | swap al confine di blocco |
| 6. ir | cabinet IR | file IR | partizionata FFT (blocco 64), **zero latenza**, ~4% CPU per 1024 tap |
| 7. delay | delay | tempo (ms o tap), feedback, mix, tone nel loop | buffer circolare |
| 8. reverb | riverbero | algoritmo, size, damp, mix | FDN o Freeverb |
| — | out | master gain, soft clip | protezione finale |

**Bypass**: non è un salto, è un crossfade lineare di **5 ms** tra segnale processato e dry dello slot. Vale anche per il cambio preset (anti-click su delay/reverb/IR), senza che questo diventi un crossfade tra modelli.

**Anti-click sul cambio modello**: rampa di 2–5 ms sul gain di uscita dello slot amp durante lo swap.

---

## 5. Modello dati

### Wah (implementato)

Slot `mod` con cinque tipi: `tremolo`, `wah`, `univibe`, `flanger`, `phaser`. Il wah è un filtro a stato variabile TPT
(Cytomic) in bandpass, frequenza su scala logaritmica dalla posizione del pedale
(350–2200 Hz di default, Q 3). Sta **prima** dell'amplificatore, come un pedale vero.

**Q che scende verso la punta**: a Q pieno la risonanza sulle alte, proprio dove il
filtro si apre, diventa un fischio "acido" invece del carattere del wah. Il Q effettivo
è `q × (1 − 0.45·posizione)` (minimo 0.6), e la compensazione di guadagno usa il Q
effettivo. Misurato sulla frequenza centrale: guadagno 5.5× al tacco, 2.5× alla punta
(prima era ~5.5× anche alla punta).

Controllo: `w` scorre i tipi (tremolo → univibe → flanger → phaser → wah), `,` e `.`
muovono la posizione del wah, oppure `type`/`position` nel preset.

**Effetti modulati** (stesso slot, prima dell'amplificatore):
- `univibe` — 4 celle di sfasamento (allpass del primo ordine) con LFO 200 Hz–2 kHz;
  la lampada modula anche l'ampiezza, da cui il "respiro".
- `flanger` — delay 0.5–6.5 ms modulato, retroazione con tanh (niente autooscillazione
  che si incastra), mix 50/50.
- `phaser` — 4 celle come l'univibe ma con retroazione.
- Parametri comuni: `rate_hz`, `depth`, `fx_mix` (quota di segnale trattato, default 0.5),
  `feedback` (flanger/phaser, default 0.35).
Con il MIDI attivo l'espressione lo pilota dal vivo (CC 7), con **auto-engage**: il wah
è spento finché il pedale non sale sopra soglia, e si rispegne da solo al rilascio.

**Proprietà dello slot mod**: il wah non è mai acceso "di suo". Quando l'auto-engage lo
accende, lo stato precedente dello slot (acceso/spento e tipo, cioè tremolo o wah) viene
messo da parte e ripristinato identico allo spegnimento. I tasti `m` e `w` non toccano lo
slot mentre il pedale lo sta usando: altrimenti il ripristino riporterebbe uno stato che
l'utente ha cambiato nel frattempo.

### Drive (implementato)

Tre modelli nello stesso slot, scelti con `k` (che li scorre), con `"type"` nel preset o
`--drive-type`:

- `fuzz` — il boost storico: pre-guadagno 1×–31×, tanh, filtro di tono. Spinge presto.
  Ha un **low-shelf a 110 Hz** (`low_db`, default +5 dB) in uscita: senza basse il
  clipping suona sottile, più "zanzara" che fuzz.
- `klon` — ispirato al Klon Centaur: guadagno più basso (1×–13×), pre-enfasi sulle alte,
  clipping a diodi asimmetrico e **blend con il segnale pulito** (`blend`, default 0.40).
  È il blend che lo rende trasparente: il clipping c'è, ma non mangia la chitarra.
  In più ha la **gobba sulle medie** a 800 Hz (`mid_db`, default +4 dB) e un makeup di
  **+3 dB** in uscita: senza, il pedale risultava piatto e sembrava abbassare il volume
  dell'ampli. Un DC blocker in uscita toglie la continua del clipping asimmetrico.

Verificato con un test: a parità di drive il Klon ha distorsione armonica più bassa del
fuzz (0.15 contro 0.25), a drive 0 non distorce (THD < 2%) e a drive 0.4 il livello sale
di ~2.5×; la gobba dà +58% a 800 Hz e lascia 4 kHz quasi fermo; il low-shelf del fuzz dà
+53% a 80 Hz.

- `tubescreamer` — il passa-alto a **720 Hz prima del clipping** è quello che gli dà le
  basse strette e la gobba sulle medie; clipping simmetrico, tono che taglia le alte
  (700 Hz–4.2 kHz). Misurato a drive 0.4: guadagno 1.6× a 100 Hz contro 4.2× a 800 Hz,
  THD 0.05, quindi più pulito sia del fuzz sia del Klon.

### Delay (implementato)

Stereo, tempo 1 ms–2 s con lettura interpolata (cambiare tempo non fa scatti), feedback
0–0,95, mix, e **passa-basso nell'anello** di retroazione: ogni ripetizione perde un po' di
alte, come in un delay analogico. Slot `delay` tra IR e riverbero, tasto `l`.

Modalità stereo: `stereo`, `ping_pong` (ingresso mono, eco alternato) e
`ping_pong_stereo` (ingresso stereo preservato, solo la retroazione si incrocia).
Tap tempo con il tasto `t` (media pesata degli intervalli).

Costo: catena completa con delay e riverbero **0,65–0,80 ms**; su VOX senza IR 0,42–0,43 ms.

### LV2 — da esplorare

Fattibile: `liblilv` e i pacchetti (`calf-plugins`, `guitarix-lv2`, `caps`, `zam-plugins`,
`lsp-plugins-lv2`) sono nei repo. Un host minimale (~300-400 righe) darebbe chorus, phaser,
flanger, EQ **e il wah di Guitarix**, che era l'idea originale.
Rimandato per due motivi: il costo CPU di plugin pensati per desktop va misurato e puo'
sfondare il budget; e un plugin che alloca nel callback audio fa tacere il pedale.
Tenuto come opzione futura, non come strada principale.

### Controllo da tastiera (implementato)

Mentre il motore gira, senza toccare i file:

| tasto | azione |
|---|---|
| `g` | bypass gate |
| `c` | bypass compressore |
| `d` | bypass drive |
| `m` | bypass tremolo (slot mod) |
| `i` | bypass IR |
| `r` | bypass riverbero |
| `s` | salva lo stato nel preset |
| `q` | esce |

Il salvataggio riscrive **solo** i campi `on` degli slot: nome e parametri che il motore non
conosce restano intatti, quindi un `s` non distrugge il file. Richiede un terminale (TTY):
da `ssh` funziona, da systemd no.

### Preset (implementato)

Un unico oggetto JSON che specchia la struttura `Config` del motore. I campi assenti
restano al default, quindi un preset puo' essere parziale.

```json
{
  "version": 1,
  "name": "Marshall Plexi -> Celestion",
  "audio": { "device": "hw:XTONE", "rate": 48000, "period_frames": 64, "periods": 3,
             "gain_db": 0.0 },
  "slots": {
    "gate":   { "on": true,  "threshold_db": -55, "attack_ms": 2, "release_ms": 120 },
    "comp":   { "on": true,  "threshold_db": -18, "ratio": 3, "attack_ms": 10,
                "release_ms": 150, "makeup_db": 3 },
    "drive":  { "on": false, "amount": 0.15, "tone": 0.5, "level_db": 0 },
    "amp":    { "on": true,  "file": "models/plexi_a2.nam", "slim": 0.49,
                "trim_db": -12 },
    "ir":     { "on": true,  "file": "irs/ir_lr_vp28.wav", "taps": 2048 },
    "reverb": { "on": true,  "mix": 0.25, "size": 0.55, "damp": 0.45 }
  },
  "stats": false
}
```

- `-p <file>` / `--preset <file>`: **alternativo** ai flag. Combinarli e' un errore
  esplicito: due percorsi di configurazione che convivono divergono in silenzio.
- Il parser e' `nlohmann/json` vendorizzato in `third_party/`, cosi' i preset funzionano
  anche in un build senza NAM.
- `trim_db` dello slot amp non ha un default universale: dipende dal modello e da come e'
  stato catturato. Va scelto all'ascolto, modello per modello.

### Config

```json
{
  "schema": 1,
  "audio": { "device": "hw:1,0", "rate": 48000, "period_frames": 64, "periods": 3 },
  "midi": {
    "banks": { "blue": {}, "green": {}, "red": {} },
    "expression": { "cc": 11, "smoothing_ms": 10 },
    "wah_autoengage": { "enabled": true, "engage_at": 0.15, "release_at": 0.03, "timeout_s": 0 }
  },
  "bindings": [
    { "cc": 20, "action": "slot.drive.bypass", "bank": "blue" },
    { "cc": 21, "action": "slot.delay.bypass", "bank": "blue" }
  ]
}
```

---

## 6. Controllo MIDI

### Configurazione: `config/midi.json`

Tutta la mappa MIDI sta in un file unico, **non nei preset**: vale per qualunque preset
e si cambia senza ricompilare. Prima di questa scelta i parametri dell'auto-engage
stavano dentro `vox_wah.json`, quindi cambiare preset cambiava anche il comportamento
del wah — esattamente ciò che non deve succedere.

```json
{
  "enabled": true,
  "device": "hw:XTONE",
  "switches": "any",
  "expression": { "cc": 7, "min_raw": 0, "max_raw": 127, "invert": false },
  "wah_autoengage": { "threshold": 0.10, "release_threshold": 0.05,
                      "engage_ms": 100, "release_ms": 500, "invert": true },
  "cc": { "gate": 25, "comp": 22, "drive": 20, "mod": 10,
          "amp": 23, "ir": 30, "delay": 11, "reverb": 24 }
}
```

- Percorso: `config/midi.json` (o `midi.json` accanto all'eseguibile), sovrascrivibile
  con `--midi-config <file>`. File assente = default compilati, nessun errore.
- Precedenza: default compilati < `midi.json` < flag da riga di comando
  (`--midi-device`, `--no-midi`, `--expr-invert`, `--wah-invert`).
- Validazione: il file viene **rifiutato** con messaggio chiaro se un CC è fuori range,
  se due azioni usano lo stesso CC o se un CC coincide con quello dell'espressione.
  `-1` disattiva un binding.
- Un preset che contiene ancora un blocco `"midi"` viene accettato ma il blocco è
  ignorato, con avviso: la mappa è una sola.

Implementazione dettagliata, diagnostica e problemi tipici: [docs/midi.md](docs/midi.md).

### Modello reale dell'XTONE (verificato in M0)

Due assi indipendenti di configurazione, entrambi sul canale MIDI 1 (stato `B0`):

- **Gruppi** (A verde, B blu, C rosso): si cambiano con **pressione lunga** di fs1/fs2/fs3.
  Cambiano solo i **numeri di CC**, non il comportamento.
- **Modalità** (1 verde, 2 blu, 3 rossa): si cambiano con combinazioni A+B, A+C, B+C e
  lampeggio del LED. Cambiano il **comportamento**:

| Modalità | Comportamento switch |
|----------|----------------------|
| 1 (verde) | 7F a ogni pressione |
| 2 (blu) | 7F seguito da 00 |
| 3 (rossa) | toggle: 7F alla prima pressione, 00 alla successiva |

Le modalità si attivano con le combinazioni (**fs1+fs2** → verde 1, **fs2+fs3** → rossa 3,
**fs1+fs3** → blu 2) e **non cambiano i numeri di CC**, solo i valori emessi.

**Verificato (M0)**: la pressione lunga su fs1/fs2/fs3 seleziona il gruppo. Nove CC distinti:

| Gruppo | LED | fs1 | fs2 | fs3 |
|--------|-----|-----|-----|-----|
| A | verde (pressione lunga fs1) | CC 10 | CC 20 | CC 11 |
| B | blu (pressione lunga fs2) | CC 22 | CC 24 | CC 25 |
| C | rossa (pressione lunga fs3) | CC 30 | CC 43 | CC 42 |

Tutti su canale 1, stato `B0`.

**Scelta per il progetto**: binding via **MIDI learn**, non codificati a priori: la tabella qui
sopra è il default proposto, ma l'apprendimento ha sempre la precedenza. Così un cambio di
firmware o di gruppo non rompe nulla.

**Politica di edge**: il valore emesso alterna `7F` e `00` a seconda dello stato interno dello
switch (comportamento toggle). Regola: **toggle su qualunque messaggio ricevuto**, con
cooldown di **250 ms** per CC. Il cooldown assorbe sia le modalità che mandano solo `7F` sia
quelle che mandano `7F` alla pressione e `00` al rilascio, senza doppi toggle.

Conseguenza: il pedale non ha stato, la verità sul bypass è solo lato software. All'avvio gli
slot partono dallo stato del preset caricato e il primo messaggio lo inverte: nessuna
desincronizzazione possibile.

**Default proposto** (8 slot + 1 riserva):

| Gruppo | fs1 | fs2 | fs3 |
|--------|-----|-----|-----|
| A | drive bypass | mod bypass | delay bypass |
| B | comp bypass | reverb bypass | gate bypass |
| C | ir bypass | tap tempo | (riserva) |

**Espressione (verificata)**: CC dipendente dal gruppo — **7** (verde), **4** (blu), **1** (rosso).
La corsa utile è **68–127**, non 0–127: il pedale copre solo la metà alta del range MIDI.
Serve normalizzazione, altrimenti un parametro mappato sul valore grezzo partirebbe a metà corsa.

Regole:

- Il config elenca i tre CC candidati `[1, 4, 7]`; qualunque CC della lista è considerato
  espressione, quindi il cambio gruppo non rompe la mappatura.
- Normalizzazione `(v - min) / (max - min)` con **auto-calibrazione**: alla prima escursione
  completa si memorizzano min e max osservati e si persistono nel config.
- Default di calibrazione: `min 68`, `max 127`.
- Smoothing 10 ms nel thread di controllo.
- Nessun conflitto con i footswitch: i loro CC sono 10/20/11, 22/24/25, 30/43/42.

**Hardware del pedale**: M-Audio M-Gear EXP. Ha una rotella e uno switch che ne cambiano il
comportamento (range, curva, eventuale inversione). Non è un oggetto a comportamento fisso:
la mappa 68–127 rilevata vale per la configurazione attuale. La calibrazione fine (min/max,
curva lineare o logaritmica, inversione) è **rimandata**: l'autocalibrazione copre le
variazioni di range, la curva eventualmente no. Da affrontare quando il wah esiste.

### MIDI learn
1. Si entra in learn da UI (M9) o da combo fisica all'avvio (es. switch 1+2 premuti).
2. Si seleziona l'azione target (bypass di uno slot, tap tempo, ecc.).
3. Il sistema resta in ascolto del primo CC non mappato; poi associa e persiste.
4. Feedback: LED lampeggiante durante l'attesa se l'XTONE accetta comandi LED dal host, altrimenti solo UI/log.
5. Il learn è sospeso durante il playing reale? No: è un'operazione esplicita, ma disattiva temporaneamente l'auto-engage del wah.

### Expression e auto-engage
- CC expression (default 7) → posizione 0–1, normalizzata su `expr_min`/`expr_max`. Sul M-Gear attuale la corsa misurata è **4–127** (log del 18/11), quindi `expr_min` sta a 0: con 68 metà pedalata non faceva nulla. `expr_invert` rovescia la mappatura posizione (e con essa la corsa del filtro).
- **Auto-engage**: accende se l'attivazione resta sopra **10%** per 100 ms, rispegne se resta sotto **5%** per 500 ms: due soglie (isteresi) e due tempi (accendere subito, spegnere senza tagliare una nota che sfuma; 100/500 ms sono i default, configurabili).
- **Polarità del gate** (`wah_autoengage.invert` / `--wah-invert`): di norma l'attivazione è la posizione (tacco 0 → punta 1) e il wah si accende verso la punta. Con `invert` l'attivazione è `1 − posizione`: la **punta è la zona spenta** e il wah si accende appena si lascia. Non è `expr_invert`: quello rovescia la mappatura del pedale (e il filtro), questo solo il punto di scatto del gate.
- Il gate resta spento finché il pedale non manda la **prima posizione**: all'avvio non si conosce dove sta il pedale, e accendersi su un valore inventato sarebbe peggio che restare spenti.
- **Footswitch**: esistono due famiglie di controller e vanno dichiarate in `midi.switches`:
  - `any` (default, XTONE con switch a stato): **ogni messaggio** è una pressione, anche con valore 0. Il valore alterna 127/0 da una pressione all'altra e non c'è un rilascio separato.
  - `edge` (switch momentanei): conta solo il fronte di salita (127), il rilascio (0) non produce mai un toggle, quindi la durata della pressione non conta.
  Il debounce è di 150 ms in entrambi i casi. Sbagliare modo significa ignorare metà delle pressioni (`any` usato come `edge`) oppure accendere e rispegnere subito tenendo premuto (`edge` usato come `any`).
- **Feedback**: ogni toggle da footswitch stampa lo stato (`mod: on (fs1)`, `drive: off (fs2, fuzz)`, `delay: on (fs3)`). Prima erano muti e non si distingueva "non arriva niente" da "arriva ma il blocco non cambia".
- **Regola dello slot mod**: il wah è sempre spento all'avvio; lo accende solo l'auto-engage. Accendendosi salva lo stato dello slot (acceso/spento, tremolo/wah) e lo ripristina identico allo spegnimento.
- Mappatura espressione → frequenza del filtro wah (scala logaritmica, 350 Hz – 2,2 kHz).
- Tutto lo stato dell'auto-engage vive nel thread controllo; il thread audio riceve solo il valore di frequenza e il flag di bypass.
- `--midi-log` stampa **ogni** messaggio ricevuto (cc, note, program change, pitch bend) con canale e valori: serve a misurare un controller sconosciuto senza indovinare. Mostrare solo i CC rendeva invisibile il caso "gli switch mandano note".

---

## 7. Effetti: note di implementazione

| Effetto | Approccio | Costo atteso |
|---------|-----------|--------------|
| gate | envelope follower + gain smoothing, isteresi sul threshold | trascurabile |
| comp | RMS/peak detector, gain computer statico, smoothing attack/release | trascurabile |
| drive | waveshaper + tone (LP/HP) + makeup; 2× oversampling opzionale | basso |
| tremolo | LFO sinusoidale, depth | trascurabile |
| wah | filtro a stato variabile (port GxWah), modelli CryBaby/Vox | basso |
| phaser | 4–6 stadi all-pass + LFO | basso |
| chorus | linea ritardata modulata 5–30 ms + mix | basso |
| auto-wah | envelope follower → frequenza filtro | basso |
| IR | partizionata FFT uniforme, blocco 64, nessuna latenza aggiunta | ~4 % CPU ogni 1024 tap |
| delay | buffer circolare, interpolazione lineare, feedback filtrato | basso |
| reverb | FDN 4–8 linee o Freeverb | basso-medio |

---

## 8. Sistema operativo e tuning

- **OS**: Raspberry Pi OS Lite 64-bit (aarch64: il Zero 2W lo supporta).
- **Kernel**: PREEMPT_RT se disponibile nei repo; fallback su kernel standard con:
  - `isolcpus=3 nohz_full=3 rcu_nocbs=3` in `cmdline.txt`
  - `threadirqs`
  - `usbcore.autosuspend=-1`
- **Disabilitare**: HDMI, audio onboard, console seriale, WiFi durante il playing, Bluetooth in v1.
- **limits.conf**: `@audio - rtprio 95`, `@audio - memlock unlimited`.
- **systemd**: unit con `CPUSchedulingPolicy=fifo`, `CPUSchedulingPriority=80`, `Restart=always`, `CPUAffinity=3`.
- **Rootfs**: read-only + `tmpfs` per `/var` e log; journald volatile. Protegge la SD e riduce il rischio di corruzione su spegnimento brusco.
- **Alimentazione**: alimentatore dedicato 5 V/2,5 A per il Pi, XTONE con il suo; eventuale hub USB powered in mezzo se emergono problemi di enumerazione.
- **Termica**: dissipatore sul Zero 2W; monitorare `vcgencmd measure_temp` e `vcgencmd get_throttled`.

---

## 9. Benchmark e gate go/no-go

**Decisione architetturale**: il benchmark va fatto sul **core C++ (NeuralAmpModelerCore / Eigen)**, non su PyTorch. È il codice che girerà in produzione e sui 512 MB evita l'inferno di installazione (torch richiede swap ≥ 2 GB e diverse ore). Lo script Python resta solo come sanity check rapido.

### Inventario modelli disponibili

Tre famiglie, per canali e dilazioni:

| sigla | canali | kernel | dilazioni | peso | giudizio |
|---|---|---|---|---|---|
| XS | 8 | 6 | 7 (1→729) | 250 KB | il target "lite" |
| S | 16 | 3 | 10 | 275–400 KB | standard leggero, da misurare |
| C | 32 | 3 | 20 | 2,2 MB | fuori scala per il Zero |

Modello scelto per il gate: `SLAMMIN_VOX_AC30_TB_V5_TC0_B10_T10_CRANKED_XS`, copiato
sul Pi come `models/vox_ac30_xs.nam`.

### Modello di riferimento (scelto dall'utente)

`presets/nam/Marshall Plexi Super Lead 1959 EL34/MARSHALL Plexi Super Lead 1959 EL34 CH II High.nam`
— NAM **0.7.0**, architettura `SlimmableContainer` con due sottomodelli:

| | canali | layer | dilazioni | pesi |
|---|---|---|---|---|
| lite (`max_value` 0,5) | 3 | 23 | 1,3,7,17,41… | 1.871 |
| full (`max_value` 1,0) | 8 | 23 | idem | 12.146 |

**Trappola**: la selezione usa `val < max_value`, quindi `--slim 0.5` attiva il modello
**full**. Il lite richiede un valore **< 0,5**.

Misure con il core ufficiale (2 s di audio, blocco 64):

| configurazione | tempo | RTF | costo per blocco |
|---|---|---|---|
| lite (3 canali) | 299 ms | **0,15** | ~0,20 ms |
| full (8 canali) | 1892 ms | 0,95 | ~1,26 ms |

L'IR scelta — `presets/ir/Celestion G12-EVH - EVH 5150III 4x12`, mono 48 kHz 24 bit — è
lunga **0,5 s = 24.001 tap**: usarla intera costerebbe ~7 ms per blocco. Va troncata.
Misure della convoluzione:

| tap | durata | costo blocco |
|---|---|---|
| 512 | 10,7 ms | 0,23 ms |
| 1024 | 21,3 ms | 0,33 ms |
| 2048 | 42,7 ms | 0,51 ms |

**IR scelta**: `G12-EVH LL 5150III 4x12 SM57 1.00in 0.0in CL7603.wav` — micro più vicino
(più carattere del cono, meno stanza) e preamp a stadio singolo, il più neutro delle tre
varianti. Sul Pi è `irs/celestion.wav`.

**Percorso veloce A2** — la scoperta che ha cambiato i numeri: il core contiene
`NAM/wavenet/a2_fast.cpp`, una implementazione hand-optimized per i modelli con la firma
A2 (23 layer, kernel `6×14,15,15,6×7`, dilazioni `1,3,7,17,41,101,239` ripetute, LeakyReLU
0,01, head kernel 16). Si attiva con `-DNAM_ENABLE_A2_FAST`, che nel build ufficiale e' ON
di default e nel nostro mancava. Senza: RTF 0,86 sul lite. Con: **RTF 0,15**.

Misure dello slot amp in isolamento (`test/bench_nam_slot.cpp`):

| configurazione | per blocco | RTF |
|---|---|---|
| lite, senza fast path | 1,149 ms | 0,86 |
| lite, con fast path | **0,203 ms** | **0,15** |
| full, con fast path | 1,270 ms | 0,95 |

Catena completa (NAM lite + IR 2048 + gate + comp + drive): **0,71 ms per blocco = 53%**
del budget a 64 frame.

**Toward the pedal**: con `--ir irs/ir_lr_vp28.wav` (la piu' brillante delle 80),
riverbero attivo e `--amp-gain -12` (valore scelto all'ascolto dall'utente), la catena
gira a **0,72-0,93 ms** senza xrun dopo l'avvio.

**Log**: le statistiche periodiche sono **spente di default** — il motore parla solo se
compare un problema (xrun dopo l'avvio, o blocco oltre l'85% del periodo). `--stats`
riattiva il report ogni 5 secondi. Le righe periodiche sono utili in sviluppo e moleste
quando si suona.

**Stabilita' verificata**: 449.894 blocchi in 10 minuti con la catena completa, 2 xrun,
entrambi all'avvio. Stesso criterio di accettazione di M2, ma con tutto acceso.

**Integrazione NAM**: il core non espone una libreria, quindi i suoi sorgenti si compilano
dentro il progetto come target `nam_core` (statica), attivato da `-DNAM_DIR=<checkout>`.
Lo slot amp resta C++17: gli header NAM vivono in `nam_slot.cpp` dietro un Pimpl, e solo
quel file viene compilato come C++20.

**Budget con NAM lite + IR 2048 tap**: ~0,71 ms su 1,33 → 53%. Il progetto sta in piedi
con il core ufficiale, senza motore di inferenza dedicato.

### Protocollo
1. Build del core C++ sul Pi (o cross-compile).
2. Caricare il modello lite di riferimento.
3. Misurare RTF e ms/blocco a 48 kHz con blocco 64 e 128.
4. Condizioni realistiche: `chrt -f 80 taskset -c 3`, 60 s di audio dopo warmup di 200 blocchi.
5. Ripetere con 44,1 kHz se il risultato è al limite.

### Soglie

| RTF NAM | Verdetto |
|---------|----------|
| ≤ 0,50 | comodo, si può salire di complessità |
| 0,50–0,70 | accettabile con blocco 128 e kernel RT |
| > 0,80 | si scende a 44,1 kHz o si cambia famiglia di modello |

- **Budget totale catena ≤ 0,75**: il NAM deve lasciare almeno il 25–30 % per effetti e IR.
- **Stabilità**: 10 minuti a blocco 64, accettazione = 0 xrun.

### Comandi di verifica (M0)

```bash
aplay -l; arecord -l              # enumerazione XTONE
cat /proc/asound/card*/stream0    # rate e formati nativi
amidi -l; aconnect -l             # switch ed expression come CC?
lsusb -v 2>/dev/null | grep -A2 XSONIC
```

---

## 10. Milestone

| # | Milestone | Deliverable | Criterio di accettazione |
|---|-----------|-------------|--------------------------|
| M0 | Setup Pi + XTONE | report formati, canali, CC reali | **fatto** — audio, MIDI e mappa CC verificati |
| M1 | Benchmark NAM C++ | numeri RTF a 64/128, 48k/44.1k | gate go/no-go superato |
| M2 | Scheletro RT | passthrough full-duplex, contatori xrun | **fatto** — 449.969 blocchi, 2 xrun di avvio, max blocco 0,02 ms |
| M3 | Slot statici | gate, comp, drive + bypass con rampa | nessun click udibile in bypass |
| M4 | Mod + wah | tremolo/phaser/chorus, port GxWah, expression, auto-engage | wah segue la pedaliera senza sfarfallio |
| M5 | Delay + reverb | i due slot, persistenza parametri | nessun artefatto a buffer 64 |
| M6 | Amp NAM | integrazione core, swap modello, preset che cambiano modello | swap senza click, RTF misurato coerente con M1 |
| M7 | Preset + learn | 9 binding, config, ricarica al riavvio | mapping persistente dopo reboot |
| M8 | Sistema | systemd, rootfs ro, boot rapido, log | boot < 20 s, nessun errore al riavvio |
| M9 | Opzionali | UI web leggera, BLE telefono | — |

---

## 11. Rischi e mitigazioni

| Rischio | Impatto | Mitigazione |
|---------|---------|-------------|
| NAM lite troppo pesante | progetto non fattibile | gate M1 prima di scrivere DSP; 44,1k; modelli più piccoli; blocco 128 |
| Glitch USB / xrun | inutilizzabile in prova | kernel RT, autosuspend off, hub powered, cavo OTG corto |
| XTONE non class-compliant per MIDI | niente footswitch | verifica M0; fallback intercettazione HID o controller BLE |
| WiFi/BT su radio condivisa | UI instabile | MIDI via USB; WiFi spento in playing |
| Throttling termico / sottotensione | drop-out | dissipatore, alimentazione separata, log throttled |
| Usura SD | corruzione | rootfs ro + log volatili + backup config |
| Installazione torch su 512 MB | setup bloccato | benchmark su core C++, niente torch in produzione |

---

## 12. Struttura repo

```
namzero-lite/
├── PLAN.md                 questo documento
├── README.md               overview + quickstart
├── docs/
│   ├── architecture.md     dettaglio thread, ring buffer, swap preset
│   ├── bench.md            protocollo e risultati benchmark
│   └── rt-setup.md         kernel RT, cmdline, limits, systemd
├── bench/
│   ├── bench_core.cpp      benchmark sul core C++ (gate M1)
│   └── bench_nam.py        sanity check veloce
├── src/
│   ├── main.cpp
│   ├── audio/              alsa_io, rt_thread, xrun_watchdog
│   ├── dsp/                gate, comp, drive, mod, wah, ir, delay, reverb, nam
│   └── control/            midi, learn, config, preset
├── config/
│   ├── config.example.json
│   └── presets/
├── models/                 *.nam (gitignore)
├── irs/                    *.wav (gitignore)
├── scripts/
│   ├── setup_pi.sh
│   └── bench.sh
└── systemd/
    └── namzero-lite.service
```

**Stack**: C++17, CMake, ALSA (`libasound2-dev`), Eigen (per il core NAM), JSON (nlohmann). Nessuna dipendenza Python in produzione.

---

## 13. Decisioni aperte

1. **Sample rate**: 48 kHz vs 44,1 kHz — dipende da M1.
2. **LED di stato**: pilotabili dal host o solo UI web.
3. **Wah**: nello slot mod (proposto) o slot dedicato.
4. **Gate e comp**: bypassabili come gli altri (proposto) o sempre attivi.
5. **Famiglia modello lite**: WaveNet lite vs LSTM — da confrontare su qualità e RTF.
6. **Banchi**: le tre configurazioni blu/verde/rossa sono banchi indipendenti di CC o solo estetica? Verifica in M0.
7. **Percorso SSH al Pi** per il benchmark: host/utente o tunnel.
8. **Curva dell'espressione**: il M-Gear EXP ha rotella e switch che ne cambiano il
   comportamento; la curva definitiva si sceglie a wah funzionante.

---

## 14. Misure sul campo (Raspberry Pi Zero 2W, XTONE, 48 kHz)

Tutte a 64 frame (budget 1,33 ms), senza sudo grazie alle capability
`cap_sys_nice,cap_ipc_lock`:

| Configurazione | max blocco | quota budget |
|---|---|---|
| passthrough | 0,02 ms | 1,5 % |
| gate + comp + drive | 0,05 ms | 3,8 % |
| IR 1024 tap (partizionata FFT) | 0,30 ms | 22 % |
| catena completa | 0,33 ms | 25 % |
| IR 1024 tap (forma diretta, abbandonata) | 1,15 ms | 86 % |

- Stabilita': 449.969 blocchi in 10 minuti con 2 xrun, entrambi all'avvio.
- La convoluzione partizionata e' 22x piu' veloce della forma diretta sul Mac e
  ~3,5x sul Pi (l'FFT non ha ancora tabelle di twiddle precalcolate).
- Margine residuo per il motore NAM: **~0,9 ms per blocco**.

## 15. Prossimo passo concreto

**M0**: accendere il Pi, installare l'OS 64-bit, collegare l'XTONE e raccogliere l'output dei comandi di verifica della sezione 9. Senza quel report non si può né dimensionare la catena né scrivere il codice MIDI.

**Poi M1**: build del benchmark sul core C++ e decisione go/no-go.
