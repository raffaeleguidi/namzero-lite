# iCON G-Board

Pedaliera USB MIDI con **8 pulsanti e 8 LED**, vista dal Pi come card
`hw:V103` (rawmidi `hw:2,0,0`). E' **opzionale**: se non e' collegata il motore
parte e suona lo stesso (con due avvisi) e i LED non danno errore.

## Configurazione del dispositivo

Il G-Board resta con la sua configurazione (mode `Program`, canale 1), salvata
in [`config/g-board.imap`](../config/g-board.imap):

| Pulsante | PC | Funzione | LED (nota) | Config in `midi.json` |
|---|---|---|---|---|
| 1 | 4 | tremolo | 4 | `gboard.mod_pc[0]` |
| 2 | 5 | univibe | 5 | `gboard.mod_pc[1]` |
| 3 | 6 | flanger | 6 | `gboard.mod_pc[2]` |
| 4 | 7 | phaser | 7 | `gboard.mod_pc[3]` |
| 5 | 0 | fuzz | 0 | `gboard.drive_pc[0]` |
| 6 | 1 | klon | 1 | `gboard.drive_pc[1]` |
| 7 | 2 | tubescreamer | 2 | `gboard.drive_pc[2]` |
| 8 | 3 | boost on/off | 3 | `gboard.boost_pc` |

Il LED di ogni pulsante e' quello con la **stessa nota del PC** (dal codice
originale `midico2.js`: `setLights(start, end, on)` manda Note On 0-7 e accende
la nota uguale a `on`).

## Mappa MIDI

### Ingresso (G-Board -> motore)

- **Program Change**, canale 1: i 8 pulsanti come da tabella.
- I PC dei mod e dei drive sono **radio button spegnibili**: premere un tasto
  diverso cambia effetto e lo accende; ripremere lo stesso lo spegne.
- Il tasto 8 (PC 3) e' un **interruttore** del boost di fine catena: acceso
  resta acceso, spento resta spento.
- I Program Change 4-7 e 0-2 non selezionano preset: quelli si cambiano con i
  tasti `0`-`9` della tastiera o con la navigazione blu dell'XTONE (CC 22/24/25).

### Uscita (motore -> LED)

- **Note On**, canale 1, **note 0-7**, velocity **127 = acceso / 0 = spento**.
- Il motore rimanda lo stato completo degli 8 LED:
  - a ogni caricamento di preset;
  - a ogni cambio di mod/drive/boost (PC del G-Board, CC, tastiera);
  - al cambio preset: prima manda `90 <slot> 7F` per `flash_ms` (150 ms), poi
    il nuovo processo aggiorna tutti i LED allo stato del preset.
- Solo gli slot 0-7 hanno un LED; il gate, il comp, il delay e il riverbero non
  hanno LED.

## Configurazione

```json
"gboard": {
  "device": "hw:V103",
  "mod_pc":   [4, 5, 6, 7],
  "drive_pc": [0, 1, 2],
  "boost_pc": 3,
  "flash_ms": 150
}
```

- `device`: porta rawmidi del G-Board (ingresso LED in uscita). Vuoto = niente LED.
- `mod_pc` / `drive_pc`: PC che selezionano i tipi, nell'ordine
  tremolo/univibe/flanger/phaser e fuzz/klon/tubescreamer.
- `boost_pc`: PC dell'interruttore boost (`-1` = nessuno).
- `flash_ms`: durata del lampeggio del LED dello slot al cambio preset.

Se un giorno cambi la configurazione del dispositivo, aggiorna questi array: la
coppia `.imap` + `midi.json` deve restare allineata.

## Verifica

Con il motore avviato e `--midi-log`:

```
midi: programchange ch1 4
  mod: tremolo on (PC4)
midi: programchange ch1 3
  boost: on (PC3)
```

Se un tasto non produce nessuna riga, la config del dispositivo non e' quella
del `.imap` (o il G-Board non e' la porta `hw:V103`).
