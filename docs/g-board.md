# iCON G-Board

Pedaliera USB MIDI con **8 pulsanti e 8 LED**, vista dal Pi come card
`hw:V103` (rawmidi `hw:2,0,0`). E' **opzionale**: se non e' collegata il motore
parte e suona lo stesso (con due avvisi) e i LED non danno errore.

La G-Board ha **due modi**, e il motore li tratta in modo diverso:

- **Program mode** (Program Change): i pulsanti mandano PC **0-7** e
  selezionano i **preset negli slot 0-7**;
- **Control/CC mode** (Control Change): i pulsanti mandano CC **50-57** e
  comandano gli **effetti** (mod, drive, boost).

I **LED non cambiano** in nessuno dei due modi: sono sempre Note On sulle note
4-7 (mod), 0-2 (drive) e 3 (boost). I numeri dei CC/PC in ingresso e le note dei
LED sono indipendenti e si configurano separatamente in `midi.json`.

## Program mode: PC 0-7 = preset 0-7

Con `gboard.preset_pc: true` (default), ogni Program Change **0-7** seleziona
lo slot con lo stesso numero:

| PC ricevuto | Slot | Funzione | Config |
|---|---|---|---|
| 0 | 0 | preset dello slot 0 | `gboard.preset_pc` |
| 1 | 1 | preset dello slot 1 | `gboard.preset_pc` |
| … | … | … | … |
| 7 | 7 | preset dello slot 7 | `gboard.preset_pc` |

Il percorso e' **lo stesso della tastiera** (`0`-`7`): `switch_slot` +
`current_slot`, messaggio `preset: slot n (PC)`, scrittura di
`config/.last_slot`, flash del LED dello slot per `flash_ms`, poi re-exec del
processo con il nuovo preset (~0,5 s). I PC sono **one-shot**: un Program
Change e' una pressione, senza debounce.

Il PC **8 e oltre non e' un preset**. Con `pc_effects: false` (default) viene
ignorato; con `pc_effects: true` puo' ancora comandare un effetto legacy se e'
mappato in `mod_pc`/`drive_pc`/`boost_pc`.

## Control/CC mode: CC 50-57 = effetti

| Pulsante | CC | Funzione | LED (nota) | Config in `midi.json` |
|---|---|---|---|---|
| 1 | 54 | tremolo | 4 | `gboard.mod_cc[0]` |
| 2 | 55 | univibe | 5 | `gboard.mod_cc[1]` |
| 3 | 56 | flanger | 6 | `gboard.mod_cc[2]` |
| 4 | 57 | phaser | 7 | `gboard.mod_cc[3]` |
| 5 | 50 | fuzz | 0 | `gboard.drive_cc[0]` |
| 6 | 51 | klon | 1 | `gboard.drive_cc[1]` |
| 7 | 52 | tubescreamer | 2 | `gboard.drive_cc[2]` |
| 8 | 53 | boost on/off | 3 | `gboard.boost_cc` |

Il **filter (wah)** non ha ne' un CC ne' un LED: si accende solo con
l'auto-engage dell'espressione. I quattro tasti mod coprono
tremolo/univibe/flanger/phaser.

## PC-effetto legacy (deprecati)

I vecchi PC 0-7 usati come effetti (mod 4-7, drive 0-2, boost 3) restano nel
codice solo per compatibilita'. Sono **spenti di default**:

- `pc_effects: false` (default): i campi `mod_pc`/`drive_pc`/`boost_pc` sono
  ignorati;
- `pc_effects: true` e `preset_pc: false`: tornano a funzionare come prima,
  **one-shot** (nessun debounce), senza cambiare preset;
- `preset_pc: true` **e** `pc_effects: true`: i PC 0-7 hanno la **priorita'
  preset** e cambiano slot; le azioni effetti legacy restano possibili solo per
  PC **diversi da 0-7**. All'avvio il motore stampa un avviso a runtime.

In pratica: per usare i preset dai PC lasciare `preset_pc: true` e
`pc_effects: false` (configurazione consigliata); per i vecchi effetti via PC
usare `preset_pc: false` e `pc_effects: true`.

## Mappa MIDI

### Ingresso (G-Board -> motore)

- **Program Change** (Program mode): con `preset_pc: true` i PC 0-7
  selezionano gli slot 0-7. Restano **one-shot**: ogni PC e' una pressione,
  senza debounce. Il cambio preset fa re-exec e lampeggia il LED dello slot.
- **Control Change** (Control/CC mode): i CC della tabella. Un messaggio con
  valore **127** e' una pressione; il rilascio (**0**) non fa nulla. Il
  debounce e' di 150 ms e agisce sul fronte di salita, quindi tenere premuto o
  ricevere un rimbalzo non produce un secondo toggle.
- I CC dei mod e dei drive sono **radio button spegnibili**: premere un tasto
  diverso cambia effetto e lo accende; ripremere lo stesso lo spegne.
- Il tasto 8 (CC 53) e' un **interruttore** del boost di fine catena: acceso
  resta acceso, spento resta spento.
- I CC degli effetti e le note LED **non cambiano** con il nuovo uso dei PC.

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
- In `/api/state`, il blocco `gboard` riporta la mappa configurata, i flag
  `preset_pc`/`pc_effects` e l'ultimo stato mandato ai LED (`gboard.leds`:
  `[{note, on}, ...]`), utile per la diagnostica e per i test anche senza
  pedaliera collegata.

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
  "preset_pc": true,
  "pc_effects": false,
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
- `preset_pc`: default `true`. Con `true` i PC 0-7 selezionano gli slot 0-7
  (Program mode = preset). Con `false` i PC non cambiano preset.
- `pc_effects`: default `false`. Con `true` i vecchi PC-effetto
  (`mod_pc`/`drive_pc`/`boost_pc`) restano attivi. Se anche `preset_pc` e'
  `true`, i PC 0-7 fanno il preset (priorita' preset) e il motore avvisa
  all'avvio; gli effetti legacy restano solo per PC fuori 0-7.
- `preset_pc: false` + `pc_effects: false`: i PC sono **ignorati**.
- `mod_pc` / `drive_pc` / `boost_pc`: Program Change storici degli effetti,
  **deprecati**. Servono solo con `pc_effects: true`; `-1` li disattiva. Per
  compatibilita', se un array `*_led` non c'e' nel file, si usano i valori
  `*_pc` come note LED (era il comportamento precedente).
- `flash_ms`: durata del lampeggio del LED dello slot al cambio preset.
- `allow_cc_conflicts`: default `false`. Di norma un `*_cc` che coincide con
  un CC gia' usato (espressione, azioni, select, boost, looper, navigazione
  22/24/25) fa fallire il caricamento con un errore chiaro, perche' una
  pressione farebbe partire due azioni. Metterlo a `true` solo se il doppio
  effetto e' voluto.

I flag `preset_pc` e `pc_effects` devono essere booleani: un valore di tipo
sbagliato fa rifiutare `midi.json` all'avvio, invece di essere ignorato in
silenzio.

**Riconfigurare il pedale**: con il software iCON si sceglie il modo. In
**Program mode** ai pulsanti si assegnano i Program Change 0-7 (un pulsante per
slot); in **Control/CC mode** si assegnano i CC 50-57. Dopo la modifica
aggiorna `midi.json` con gli stessi numeri: la coppia pedaliera + file deve
restare allineata. I LED non richiedono di essere riprogrammati: il motore
manda sempre le note 4-7/0-2/3, salvo diversa scelta in `midi.json`.

## Verifica

Con il motore avviato e `--midi-log`, un cambio preset da PC:

```
midi  1234.5 ms  programchange ch1    0   0
  preset: slot 0 (PC)
slot 0: config/presets/vox_wah.json
```

Un CC-effetto resta invariato:

```
midi  1400.0 ms  cc          ch1   53 127
  boost: on (CC53)
midi  1500.0 ms  cc          ch1   54 127
  mod: tremolo on (CC54)
```

Il rilascio (`0`) compare nel log ma non produce la riga `  mod: ...`: e'
normale. Se un tasto non produce nessuna riga, il CC non e' quello del file o
il device non e' la porta `hw:V103`.

Test automatici sul Pi (iniettano i byte sulla socket BLE e controllano
stato/LED/debounce):

```bash
# CC 50-57, LED e debounce (i PC-effetto legacy vengono saltati se disabilitati)
python3 test/gboard_cc_runtime.py

# PC 0-7 -> slot, PC ignorati, PC-effetto legacy; cambia config con backup e
# riavvia il servizio per coprire i quattro casi, poi ripristina tutto
python3 test/gboard_pc_runtime.py --all --repo ~/namzero-lite
```
