# iCON G-Board

Pedaliera USB MIDI con **8 pulsanti e 8 LED**, vista dal Pi come card
`hw:V103` (rawmidi `hw:2,0,0`). Arriva come dispositivo MIDI 1.0 full speed.

## Configurazione dei pulsanti

Dal file [`g-board midi settings.imap`](https://raw.githubusercontent.com/raffaeleguidi/midico/refs/heads/master/g-board%20midi%20settings.imap)
(mode `Program`, canale 1):

| Pulsante | Messaggio | Slot richiamato da `presets.json` |
|---|---|---|
| 1 | Program Change **4** | 4 |
| 2 | Program Change **5** | 5 |
| 3 | Program Change **6** | 6 |
| 4 | Program Change **7** | 7 |
| 5 | Program Change **0** | 0 |
| 6 | Program Change **1** | 1 |
| 7 | Program Change **2** | 2 |
| 8 | Program Change **3** | 3 |

Tutti i messaggi sono sul **canale 1**.

Poiché il motore interpreta i Program Change 0-9 come selezione dello slot
(vedi [midi.md](midi.md)), i tasti del G-Board richiamano **direttamente** i
preset: 4, 5, 6, 7 sui primi quattro tasti e 0, 1, 2, 3 sugli altri quattro.
Non serve nessuna mappatura CC.

Esempio di log atteso (`--midi-log`):

```
midi: programchange ch1 5
  preset: slot 5 (PC)
slot 5: config/presets/vox_ac30_edge.json
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

## Collegamento al motore

- **Ingresso**: i pulsanti mandano Program Change 0-7, gia' interpretati come
  selezione slot (vedi sopra).
- **Uscita (da fare)**: un `MidiOutput` verso `hw:V103` che mandi
  `90 <slot> 7F` accendendo il LED dello slot attivo, e velocity 0 sugli altri,
  piu' i LED degli effetti accesi se si vuole.
