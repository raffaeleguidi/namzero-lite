# Controllo MIDI

Documento d'implementazione: come il motore legge i segnali dell'XTONE, dove sta la
configurazione e perché il comportamento è lo stesso su tutti i preset.

## 1. File di configurazione: `config/midi.json`

Tutta la mappa MIDI vive qui, **non nei preset**. Prima stava nel blocco `"midi"` di
`vox_wah.json`, e questo faceva sì che cambiando preset cambiasse anche l'auto-engage
del wah. Ora un preset non può più influenzare il MIDI: se ne contiene ancora un blocco
`"midi"` viene caricato lo stesso, ma il blocco è **ignorato con un avviso**.

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

| Campo | Significato |
|-------|-------------|
| `enabled` | `false` equivale a `--no-midi` |
| `device` | nome del rawmidi; **per nome** (`hw:XTONE`) perché l'indice di scheda cambia al boot |
| `switches` | `any` (switch a stato, un messaggio per pressione) o `edge` (momentanei, 127 premuto / 0 rilasciato) |
| `expression.cc` | control change del pedale di espressione |
| `expression.min_raw` / `max_raw` | estremi grezzi del pedale, per normalizzare 0–1 |
| `expression.invert` | rovescia la corsa del pedale (e quindi del filtro) |
| `wah_autoengage.*` | soglie e tempi del gate, vedi §4 |
| `cc` | un CC per azione; `-1` disattiva quel binding |

**Percorso**: `config/midi.json`, oppure `midi.json` accanto all'eseguibile, oppure
quello passato con `--midi-config <file>`. Se il file non esiste si usano i default
compilati (solo `mod` 10, `drive` 20, `delay` 11 — il comportamento storico).

**Precedenza**: default compilati → `midi.json` → flag da riga di comando
(`--midi-device`, `--no-midi`, `--expr-invert`, `--wah-invert`).

**Validazione**: il file viene rifiutato, con messaggio, se un CC è fuori da 0–127, se
due azioni usano lo stesso CC o se un CC coincide con quello dell'espressione. Un file
sbagliato deve fermare l'avvio: un binding muto è più difficile da diagnosticare di un
errore esplicito.

## 2. Azioni e CC

Gli slot della catena, nell'ordine in cui vengono comandati:

| Azione | CC nel file di esempio | Note |
|--------|------------------------|------|
| `gate` | 25 | banco B dell'XTONE |
| `comp` | 22 | banco B |
| `drive` | 20 | banco A (fs2) |
| `mod` | 10 | banco A (fs1): tremolo; se il wah è in mano al pedale il tasto è ignorato |
| `amp` | 23 | non mappato dall'XTONE, libero |
| `ir` | 30 | banco C |
| `delay` | 11 | banco A (fs3) |
| `reverb` | 24 | banco B |
| espressione | 7 | pedale, banco A |
| `mod_select` | 32/33/34/35 | **seleziona** tremolo/univibe/flanger/phaser (un tasto per effetto); non e' un CC di slot |
| `drive_select` | 36/37/38 | **seleziona** fuzz/klon/tubescreamer (radio button) |
| `delay_select` | 45/46/47 | **seleziona** stereo/ping-pong/ping-pong stereo (radio button) |
| `boost` | 44 | **aumenta** il volume di `db` dB a fine catena (default +6); ripremendo torna al livello originale |

I gruppi dell'XTONE cambiano **solo i numeri di CC**: A = 10/20/11, B = 22/24/25,
C = 30/43/42. Con questa mappa i tre banchi coprono tutti gli slot tranne `amp`, che
non è assegnato dall'XTONE (il CC 23 è libero per un controller esterno).


### XTONE: i tre gruppi (A verde, B blu, C rosso)

L'XTONE cambia i CC che manda a seconda del gruppo selezionato:

| | A (verde) | B (blu) | C (rosso) |
|---|---|---|---|
| FS A | CC 10 | CC 22 | CC 30 |
| FS B | CC 20 | CC 24 | CC 43 |
| FS C | CC 11 | CC 25 | CC 42 |
| EXP | CC 7 | CC 4 | CC 1 |

Con la mappa attuale di `config/midi.json`:

| Gruppo | FS A | FS B | FS C | EXP |
|---|---|---|---|---|
| A (verde) | mod | drive | delay | espressione ✔ |
| B (blu) | comp | reverb | gate | **CC 4: non mappato** |
| C (rosso) | ir | libero (43) | libero (42) | **CC 1: non mappato** |

Il **gruppo blu** e' usato per la navigazione dei preset: **CC 22 = slot
precedente, CC 24 = slot successivo, CC 25 = primo slot**. Per questo gate, comp
e reverb non hanno piu' un CC (erano 25/22/24): restano comandabili da tastiera
(`g`, `c`, `r`) e si possono rimappare, per esempio sui CC liberi 43/42 del
gruppo rosso.

Due conseguenze:

- l'**espressione** (wah/volume) funziona solo nel gruppo A, perché in B e C il
  pedale manda CC 4 e CC 1: per usarla ovunque servono tre CC di espressione
  (modifica da fare se serve);
- i gruppi B e C hanno CC liberi (43, 42) e CC gia' usati: sono i candidati per
  la navigazione dei preset, se non si vuole usare il Program Change.

I tre gruppi sono la base su cui decidere la mappa: A per gli stomp, B/C per la
navigazione, oppure PC per i preset e CC per gli stomp.

### Radio button: `mod_select`, `drive_select`, `delay_select`

Le tre mappe funzionano tutte allo stesso modo:

- slot **spento** + tasto → si accende con quella scelta;
- slot **acceso con un'altra scelta** + tasto → passa a quella (mutuamente esclusivi);
- slot **acceso proprio con quella scelta** + tasto → si spegne.

In pratica sono gruppi di radio button con la possibilità di tornare a "nessuno".
Ogni selezione imposta anche i parametri suggeriti del tipo (rate/feedback per le
modulazioni), così il tasto produce subito un suono sensato.

## 3. Dai byte rawmidi allo slot

```
rawmidi (hw:XTONE)
   └── MidiInput::poll()          non bloccante, running status
        └── MidiEvent             status + due dati, per tutti i "channel voice"
             └── tabella cc→azione (128 voci, costruita all'avvio)
                  └── SwitchDebounce        fronte/tempo, 150 ms
                       └── chain.set_enabled(slot, …)   atomico, crossfade 5 ms
```

- Gli ingressi sono **due**: il rawmidi dell'XTONE e la socket del bridge BLE
  (`midi.ble.socket`, vedi [ble.md](ble.md)). Il parser e' lo stesso, quindi il
  comportamento non cambia da dove arriva il messaggio.
- Il parser accetta anche **note, program change, pitch bend**: non servono per suonare,
  ma `--midi-log` li mostra. Prima il parser scartava tutto ciò che non era CC e un
  controller che mandava note sembrava muto.
- Il clock realtime (≥ `0xF8`) e i messaggi di sistema vengono ignorati; il running
  status è supportato.
- Il thread MIDI fa `poll` ogni 2 ms e non alloca nel ciclo.
- La tabella `cc → azione` è un array di 128 interi costruito una volta: il thread non
  fa catene di confronti. Le collisioni sono già state rifiutate dal loader.
- I toggle scrivono lo stato dell'app e `chain.set_enabled`; il thread audio legge solo
  atomici. Tastiera e MIDI condividono un mutex perché scrivono lo stesso stato.

## 4. Auto-engage del wah

Macchina a stati in `src/control/wah_autoengage.h`, isolata per poterla testare senza
audio:

- si accende se l'**attivazione** resta sopra `threshold` (10%) per `engage_ms` (100 ms);
- si spegne se resta sotto `release_threshold` (5%) per `release_ms` (500 ms);
- due soglie = isteresi di posizione, due tempi = accendere subito e spegnere senza
  tagliare una nota che sfuma.

**Polarità** (`wah_autoengage.invert`): di norma attivazione = posizione (tacco 0 →
punta 1) e il wah si accende verso la punta. Con `invert` l'attivazione è `1 − posizione`:
la punta diventa la zona spenta e il wah si accende appena la si lascia. È diverso da
`expression.invert`, che rovescia la mappa del pedale (e quindi la corsa del filtro).

**Proprietà dello slot `mod`**: il wah non è mai acceso di suo.

1. All'avvio, se il MIDI è aperto e il preset ha `mod` acceso in tipo `wah`, viene
   spento: lo accende solo il pedale.
2. Quando il pedale lo accende, `ModArbiter` salva lo stato dello slot (acceso/spento e
   tipo) e lo ripristina identico allo spegnimento: se c'era il tremolo, torna il
   tremolo.
3. Mentre il pedale tiene il wah, tasti e footswitch non toccano lo slot, altrimenti il
   ripristino riporterebbe uno stato cambiato nel frattempo.
4. Finché il pedale non manda la **prima posizione** il gate resta spento: all'avvio non
   si sa dove sta il pedale.

## 5. Diagnostica

```bash
./build/namzero-lite -p config/presets/vox_wah.json --midi-log
```

Stampa ogni messaggio ricevuto con tipo, canale e valori:

```
midi  2433.9 ms  cc          ch1   10 127
midi  2440.1 ms  cc          ch1    7  64  pos 0.50  gate 0.50
```

Per l'espressione aggiunge `pos` (dopo normalizzazione e inversione) e `gate`
(l'attivazione su cui lavora l'auto-engage), più eventuali note `(corsa invertita)` e
`(punta = spento)`. È lo strumento con cui si misura un controller sconosciuto: valori
grezzi, range reale e punto di scatto.

Ogni toggle stampa anche lo stato, così si distingue "non arriva niente" da "arriva ma
il blocco non cambia":

```
  mod: on (CC10)
  drive: off (CC20, fuzz)
  delay: on (CC11)
  wah: acceso dal pedale
  wah: spento dal pedale
```

## 6. Switch: `any` contro `edge`

Due famiglie di controller, comportamento opposto:

| Modo | Messaggi per pressione | Regola |
|------|------------------------|--------|
| `any` (default) | uno, con valore che alterna 127/0 | ogni messaggio è una pressione, **anche con valore 0** |
| `edge` | due: 127 premuto, 0 rilasciato | conta solo il fronte di salita |

Sbagliare modo: con `edge` su switch a stato si ignora metà delle pressioni (i valori 0);
con `any` su switch momentanei il rilascio conta come seconda pressione e il blocco si
accende e rispegne subito. Il debounce è 150 ms in entrambi i casi; in `edge`, se il
controller non manda mai il rilascio, due pressioni si distinguono dopo 600 ms.

## 7. Test

Senza ALSA, quindi eseguibili anche fuori dal Pi:

| Test | Copre |
|------|-------|
| `switch_debounce_test` | modi `any`/`edge`, rimbalzi, pressione tenuta a lungo, soli-127 |
| `wah_autoengage_test` | soglie, isteresi, tempi, polarità invertita, ripristino dello slot |
| `midi_config_test` | default, file completo, `-1`, doppioni, conflitto con l'espressione, JSON rotto |
| `drive_klon_test` | modello Klon (non MIDI, ma sta nello stesso slot comandato dai CC) |

```bash
cmake --build build -j1 && cd build && ctest
```

## 8. Problemi tipici

| Sintomo | Causa probabile |
|---------|-----------------|
| Nessun messaggio nel log | device sbagliato (`amidi -l`), `enabled: false`, cavo/OTG |
| Il log mostra `noteon`/`programchange` | gli switch non mandano CC: il binding per CC non può scattare |
| Il blocco si accende e si spegne subito | switch momentanei con `switches: any` → mettere `edge` |
| Metà delle pressioni ignorate | switch a stato con `switches: edge` → mettere `any` |
| Il wah non si spegne mai | manca `wah_autoengage.invert` per la convenzione "punta = spento" |
| Il wah non si accende | il gate aspetta la prima posizione del pedale, o il pedale è sulla punta con `invert` |
| `Operation not permitted` all'avvio | non è MIDI: mancano le capability realtime, vedi README |
