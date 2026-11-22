# iCON G-Board

Pedaliera USB MIDI con **8 pulsanti e 8 LED**, vista dal Pi come card
`hw:V103` (rawmidi `hw:2,0,0`). E' **opzionale**: se non e' collegata il motore
parte e suona lo stesso (con due avvisi) e i LED non danno errore.

La G-Board e' stata riconfigurata: i pulsanti ora mandano **Control Change
50-57** invece dei Program Change storici. Il motore accetta entrambi:

- i **CC 50-57** sono la configurazione attuale, con debounce sul fronte 127;
- i **PC 0-7** restano accettati per retrocompatibilita' (one-shot, come prima).

I **LED non cambiano**: continuano a essere pilotati con Note On sulle note
4-7 (mod), 0-2 (drive) e 3 (boost). I numeri dei CC in ingresso e le note dei
LED sono indipendenti e si configurano separatamente in `midi.json`.

## Mappa attuale

| Pulsante | CC (attuale) | PC (legacy) | Funzione | LED (nota) | Config in `midi.json` |
|---|---|---|---|---|---|
| 1 | 54 | 4 | tremolo | 4 | `gboard.mod_cc[0]` / `gboard.mod_pc[0]` |
| 2 | 55 | 5 | univibe | 5 | `gboard.mod_cc[1]` / `gboard.mod_pc[1]` |
| 3 | 56 | 6 | flanger | 6 | `gboard.mod_cc[2]` / `gboard.mod_pc[2]` |
| 4 | 57 | 7 | phaser | 7 | `gboard.mod_cc[3]` / `gboard.mod_pc[3]` |
| 5 | 50 | 0 | fuzz | 0 | `gboard.drive_cc[0]` / `gboard.drive_pc[0]` |
| 6 | 51 | 1 | klon | 1 | `gboard.drive_cc[1]` / `gboard.drive_pc[1]` |
| 7 | 52 | 2 | tubescreamer | 2 | `gboard.drive_cc[2]` / `gboard.drive_pc[2]` |
| 8 | 53 | 3 | boost on/off | 3 | `gboard.boost_cc` / `gboard.boost_pc` |

Il **filter (wah)** non ha ne' un CC ne' un LED: si accende solo con
l'auto-engage dell'espressione. I quattro tasti mod coprono
tremolo/univibe/flanger/phaser.

## Mappa MIDI

### Ingresso (G-Board -> motore)

- **Control Change** (configurazione attuale): i CC della tabella. Un
  messaggio con valore **127** e' una pressione; il rilascio (**0**) non fa
  nulla. Il debounce e' di 150 ms e agisce sul fronte di salita, quindi tenere
  premuto o ricevere un rimbalzo non produce un secondo toggle.
- **Program Change** (retrocompatibilita'): i PC della tabella. Restano
  **one-shot**: ogni PC e' una pressione, senza debounce.
- I CC dei mod e dei drive sono **radio button spegnibili**: premere un tasto
  diverso cambia effetto e lo accende; ripremere lo stesso lo spegne.
- Il tasto 8 (CC 53 / PC 3) e' un **interruttore** del boost di fine catena:
  acceso resta acceso, spento resta spento.
- Questi messaggi non selezionano preset: quelli si cambiano con i tasti
  `0`-`9` della tastiera o con la navigazione blu dell'XTONE (CC 22/24/25).

### Uscita (motore -> LED)

- **Note On**, canale 1, con le note configurate in `gboard.mod_led`,
  `gboard.drive_led`, `gboard.boost_led` (default 4-7 / 0-2 / 3), velocity
  **127 = acceso / 0 = spento**.
- Le note dei LED sono **indipendenti** dai CC/PC: si puo' cambiare il CC di un
  pulsante senza toccare il LED, e viceversa.
- Il motore rimanda lo stato completo degli 8 LED:
  - a ogni caricamento di preset;
  - a ogni cambio di mod/drive/boost (CC o PC del G-Board, tastiera);
  - al cambio preset: prima manda `90 <slot> 7F` per `flash_ms` (150 ms), poi
    il nuovo processo aggiorna tutti i LED allo stato del preset.
- Solo gli slot 0-7 hanno un LED; il gate, il comp, il delay e il riverbero non
  hanno LED.
- In `/api/state`, il blocco `gboard` riporta la mappa configurata e l'ultimo
  stato mandato ai LED (`gboard.leds`: `[{note, on}, ...]`), utile per la
  diagnostica e per i test anche senza pedaliera collegata.

## Configurazione

```json
"gboard": {
  "device": "hw:V103",
  "mod_cc":    [54, 55, 56, 57],
  "drive_cc":  [50, 51, 52],
  "boost_cc":  53,
  "mod_led":   [4, 5, 6, 7],
  "drive_led": [0, 1, 2],
  "boost_led": 3,
  "mod_pc":    [4, 5, 6, 7],
  "drive_pc":  [0, 1, 2],
  "boost_pc":  3,
  "flash_ms": 150
}
```

- `device`: porta rawmidi del G-Board (ingresso LED in uscita). Vuoto = niente LED.
- `mod_cc` / `drive_cc` / `boost_cc`: CC in ingresso, nell'ordine
  tremolo/univibe/flanger/phaser, fuzz/klon/tubescreamer e boost. `-1`
  disattiva una singola voce.
- `mod_led` / `drive_led` / `boost_led`: note MIDI dei LED, stesso ordine.
  `-1` lascia quel LED spento/non pilotato.
- `mod_pc` / `drive_pc` / `boost_pc`: Program Change storici, ancora accettati.
  `-1` li disattiva. Per compatibilita', se un array `*_led` non c'e' nel file,
  si usano i valori `*_pc` come note LED (era il comportamento precedente).
- `flash_ms`: durata del lampeggio del LED dello slot al cambio preset.
- `allow_cc_conflicts`: default `false`. Di norma un `*_cc` che coincide con
  un CC gia' usato (espressione, azioni, select, boost, looper, navigazione
  22/24/25) fa fallire il caricamento con un errore chiaro, perche' una
  pressione farebbe partire due azioni. Metterlo a `true` solo se il doppio
  effetto e' voluto.

**Riconfigurare il pedale**: con il software iCON si assegnano ai pulsanti i CC
50-57 (e, se serve, i PC 4-7/0-2/3 restano come fallback). Dopo la modifica
aggiorna `midi.json` con gli stessi numeri: la coppia pedaliera + file deve
restare allineata. I LED non richiedono di essere riprogrammati: il motore
manda sempre le note 4-7/0-2/3, salvo diversa scelta in `midi.json`.

## Verifica

Con il motore avviato e `--midi-log`:

```
midi  1234.5 ms  cc          ch1   54 127
  mod: tremolo on (CC54)
midi  1300.0 ms  cc          ch1   54   0
midi  1400.0 ms  cc          ch1   53 127
  boost: on (CC53)
midi  1500.0 ms  programchange ch1    4   0
  mod: tremolo off (PC4)
```

Il rilascio (`0`) compare nel log ma non produce la riga `  mod: ...`: e'
normale. Se un tasto non produce nessuna riga, il CC non e' quello del file o
il device non e' la porta `hw:V103`.

Test automatico sul Pi (inietta i CC sulla socket BLE e controlla
stato/LED/debounce/PC):

```bash
python3 test/gboard_cc_runtime.py
```
