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
- **Da verificare in M0**: classe di conformità USB (audio + MIDI), canali, sample rate nativi, se l'expression arriva come CC, se i LED sono pilotabili dal host.

### Budget di latenza
- 48 kHz, periodi da 64 frame → 1,33 ms per blocco.
- Round-trip atteso con XTONE: **8–12 ms**, accettabile per chitarra.
- Obiettivo: nessun xrun in 10 minuti continui a 64 frame.

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
| 3. drive | boost / overdrive | tipo, drive, tone, level | oversampling 2× opzionale |
| 4. mod | modulazione | tipo: tremolo / wah / phaser / chorus / auto-wah | wah = port di GxWah (Guitarix, GPL) |
| 5. amp | NAM lite | file modello, input gain, output gain | swap al confine di blocco |
| 6. ir | cabinet IR | file IR, mix | convoluzione partizionata 1024 tap, primi 64 tap FIR diretti |
| 7. delay | delay | tempo (ms o tap), feedback, mix, tone nel loop | buffer circolare |
| 8. reverb | riverbero | algoritmo, size, damp, mix | FDN o Freeverb |
| — | out | master gain, soft clip | protezione finale |

**Bypass**: non è un salto, è un crossfade lineare di **5 ms** tra segnale processato e dry dello slot. Vale anche per il cambio preset (anti-click su delay/reverb/IR), senza che questo diventi un crossfade tra modelli.

**Anti-click sul cambio modello**: rampa di 2–5 ms sul gain di uscita dello slot amp durante lo swap.

---

## 5. Modello dati

### Preset (schema fisso, versionato)

```json
{
  "schema": 1,
  "name": "Clean Brunito",
  "slots": {
    "gate":  { "on": true,  "params": { "threshold_db": -55, "attack_ms": 2, "release_ms": 120 } },
    "comp":  { "on": true,  "params": { "threshold_db": -18, "ratio": 3.0, "attack_ms": 10, "release_ms": 150, "makeup_db": 3 } },
    "drive": { "on": false, "params": { "type": "overdrive", "drive": 0.35, "tone": 0.5, "level": 0.7 } },
    "mod":   { "on": false, "params": { "type": "tremolo", "rate_hz": 4.5, "depth": 0.6 } },
    "amp":   { "on": true,  "params": { "file": "models/tweed_lite.nam", "in_gain": 0.0, "out_gain": -2.0 } },
    "ir":    { "on": true,  "params": { "file": "irs/4x12_sm57.wav", "mix": 1.0 } },
    "delay": { "on": false, "params": { "time_ms": 380, "feedback": 0.35, "mix": 0.25, "tone": 0.5 } },
    "reverb":{ "on": false, "params": { "size": 0.6, "damp": 0.4, "mix": 0.2 } }
  }
}
```

- Validazione completa all'ingresso; un preset invalido viene rifiutato con log, mai caricato parzialmente.
- Procedura: parse → costruzione `Preset` preallocato → swap del puntatore atomico.

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

CC osservati nel gruppo B (default): fs1 = 10, fs2 = 20, fs3 = 11.

**Scelta per il progetto**: modalità **3** (toggle). Mappa 1:1 sul bypass di uno slot senza
dover tracciare il rilascio; gli altri switch sono esclusivi tra loro e il LED dello slot
riflette lo stato senza logica aggiuntiva.

3 gruppi × 3 switch = **9 binding**, che tornano a coprire gli 8 slot + 1 riserva.
Il cambio gruppo è un gesto fisico sul pedale: il software vede solo CC diversi, non deve
conoscere il concetto di gruppo.

**Espressione**: non ancora verificata (nessun messaggio catturato in M0). Attenzione al
possibile conflitto: il CC 11 è quello standard dell'expression, ma nel gruppo B è anche
assegnato a fs3. Da testare.

### MIDI learn
1. Si entra in learn da UI (M9) o da combo fisica all'avvio (es. switch 1+2 premuti).
2. Si seleziona l'azione target (bypass di uno slot, tap tempo, ecc.).
3. Il sistema resta in ascolto del primo CC non mappato; poi associa e persiste.
4. Feedback: LED lampeggiante durante l'attesa se l'XTONE accetta comandi LED dal host, altrimenti solo UI/log.
5. Il learn è sospeso durante il playing reale? No: è un'operazione esplicita, ma disattiva temporaneamente l'auto-engage del wah.

### Expression e auto-engage
- CC expression → posizione 0–127, smoothing 10 ms nel thread controllo.
- **Auto-engage**: engage se posizione > 15 %, release se < 3 %, con isteresi per evitare sfarfallio; opzionale timeout di inattività. Engage/disengage con rampa di 5–10 ms.
- Mappatura espressione → frequenza del filtro wah (scala logaritmica, 300 Hz – 2,2 kHz).
- Tutto lo stato dell'auto-engage vive nel thread controllo; il thread audio riceve solo il valore di frequenza e il flag di bypass.

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
| IR | convoluzione partizionata 1024 tap, primi 64 diretti, FFT uniforme | ~1–2 % CPU |
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
| M0 | Setup Pi + XTONE | report con formati, canali, CC reali, LED | XTONE enumera come audio; MIDI visibile |
| M1 | Benchmark NAM C++ | numeri RTF a 64/128, 48k/44.1k | gate go/no-go superato |
| M2 | Scheletro RT | processo con passthrough full-duplex + IR, contatori xrun | 10 min, 0 xrun a 64 |
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

---

## 14. Prossimo passo concreto

**M0**: accendere il Pi, installare l'OS 64-bit, collegare l'XTONE e raccogliere l'output dei comandi di verifica della sezione 9. Senza quel report non si può né dimensionare la catena né scrivere il codice MIDI.

**Poi M1**: build del benchmark sul core C++ e decisione go/no-go.
