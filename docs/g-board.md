# iCON G-Board

Pedaliera USB MIDI con **8 pulsanti e 8 LED**, vista dal Pi come card
`hw:V103` (rawmidi `hw:2,0,0`). Arriva come dispositivo MIDI 1.0 full speed.

## Configurazione dei pulsanti

Dal file [`g-board midi settings.imap`](https://raw.githubusercontent.com/raffaeleguidi/midico/refs/heads/master/g-board%20midi%20settings.imap)
(mode `Program`, canale 1):

| Pulsante | PC | Effetto | LED |
|---|---|---|---|
| 1 | 4 | tremolo | 0 |
| 2 | 5 | univibe | 1 |
| 3 | 6 | flanger | 2 |
| 4 | 7 | phaser | 3 |
| 5 | 0 | fuzz | 4 |
| 6 | 1 | klon | 5 |
| 7 | 2 | tubescreamer | 6 |
| 8 | 3 | (libero) | 7 |

Tutti sul **canale 1**. I PC sono configurati in `midi.json`:

```json
"gboard": {
  "device": "hw:V103",
  "mod_pc":   [4, 5, 6, 7],
  "drive_pc": [0, 1, 2],
  "flash_ms": 150
}
```

La prima riga e' mod (radio button spegnibili: ripremere lo stesso tasto spegne),
la seconda e' drive (idem). I preset **non** si cambiano da qui: restano sui tasti
0-9 della tastiera e sulla navigazione blu dell'XTONE (CC 22/24/25).

Esempio di log:

```
  mod: tremolo on (PC4)
  mod: tremolo off (PC4)
  drive: fuzz on (PC0)
```

## LED

I LED si comandano con **Note On sul canale 1**, una nota per LED, velocity
**127 = acceso** e **0 = spento** (dal codice `midico2.js`, funzione `setLights`):

| LED | Nota | Messaggio per accenderlo |
|---|---|---|
| 1 | 0 | `90 00 7F` |
| 2 | 1 | `90 01 7F` |
| 3 | 2 | `90 02 7F` |
| 4 | 3 | `90 03 7F` |
| 5 | 4 | `90 04 7F` |
| 6 | 5 | `90 05 7F` |
| 7 | 6 | `90 06 7F` |
| 8 | 7 | `90 07 7F` |

Per spegnere: stessa nota con velocity 0 (`90 0N 00`). Il G-Board ascolta come
dispositivo MIDI (`hw:V103`); per pilotare i LED serve un'**uscita** MIDI dal Pi
verso quella porta, cosa che il motore oggi non ha.

Esempio con `amidi` (verifica manuale):

```bash
amidi -p hw:V103 -S "90 00 7F"   # LED 1 acceso
amidi -p hw:V103 -S "90 00 00"   # LED 1 spento
```

Nota: nel codice originale `setLights(start, end, on)` manda le note da `start`
a `end` e accende solo quella uguale a `on`; con note 0-3 accende il LED dello slot
(0-3), con 4-6 quello della scena, e la nota 7 fa da "home" lampeggiante.

## Collegamento al motore (implementato)

- **Ingresso**: i PC selezionano gli effetti come da tabella; il motore apre
  `hw:V103` insieme agli altri ingressi di `midi.json`.
- **Uscita**: `MidiOutput` su `gboard.device`; a ogni cambio di stato il motore
  manda Note On per tutti gli 8 LED: acceso solo quello dell'effetto selezionato
  nella riga mod e in quella drive. Al caricamento del preset i LED riflettono
  subito lo stato.
- **Cambio preset**: prima di ricaricare, il motore manda `90 <slot> 7F` per
  `flash_ms` (150 ms) e poi il nuovo processo aggiorna tutti i LED allo stato del
  preset. Solo gli slot 0-7 hanno un LED.
