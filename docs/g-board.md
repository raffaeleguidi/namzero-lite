# iCON G-Board

Pedaliera USB MIDI con **8 pulsanti e 8 LED**, vista dal Pi come card
`hw:V103` (rawmidi `hw:2,0,0`). E' **opzionale**: se non e' collegata il motore
parte e suona lo stesso (con due avvisi) e i LED non danno errore.

La config viva usa la G-Board in **Control/CC mode**: i pulsanti mandano CC
**50-57** e comandano gli **effetti** (mod, drive, boost). I **Program Change
sono ignorati** (`gboard.preset_pc: false`, `gboard.pc_effects: false`).

Il motore conserva anche il percorso Program mode (PC 0-7 -> slot 0-7), ma e'
spento nella config viva; per usarlo servirebbe `preset_pc: true`.
In **Control/CC mode** i LED usano note **1-based**: la nota N accende il LED
fisico N (pulsante N). Quindi mod 1-4, drive 5-7, boost 8. I numeri dei CC/PC
in ingresso e le note dei LED sono indipendenti e si configurano separatamente
in `midi.json`.

## Program mode (disattivato nella config viva)

Con `gboard.preset_pc: true` ogni Program Change **0-7** seleziona lo slot con
lo stesso numero; il percorso e' **lo stesso della tastiera** (`0`-`7`):
`switch_slot` + `current_slot`, messaggio `preset: slot n (PC)`, scrittura di
`config/.last_slot`, flash del LED dello slot per `flash_ms`, poi re-exec del
processo con il nuovo preset (~0,5 s). I PC sono **one-shot**: un Program
Change e' una pressione, senza debounce.

Nella config viva `preset_pc: false` e `pc_effects: false`: i PC non cambiano
preset e non comandano effetti. Il PC **8 e oltre** non e' mai un preset; con
`pc_effects: true` puo' ancora comandare un effetto legacy se e' mappato in
`mod_pc`/`drive_pc`/`boost_pc`.

## Control/CC mode: CC 50-57 = effetti

| Pulsante | CC | Funzione | LED (nota) | Config in `midi.json` |
|---|---|---|---|---|
| 1 | 54 | tremolo | 1 | `gboard.mod_cc[0]` |
| 2 | 55 | univibe | 2 | `gboard.mod_cc[1]` |
| 3 | 56 | flanger | 3 | `gboard.mod_cc[2]` |
| 4 | 57 | phaser | 4 | `gboard.mod_cc[3]` |
| 5 | 50 | fuzz | 5 | `gboard.drive_cc[0]` |
| 6 | 51 | klon | 6 | `gboard.drive_cc[1]` |
| 7 | 52 | tubescreamer | 7 | `gboard.drive_cc[2]` |
| 8 | 53 | boost on/off | 8 | `gboard.boost_cc` |

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

In pratica: la config viva ha `preset_pc: false` e `pc_effects: false`, quindi
tutti i PC sono ignorati e la pedaliera lavora solo in Control/CC mode. Per
riattivare i preset dai PC servirebbe `preset_pc: true`; per i vecchi effetti
via PC `preset_pc: false` e `pc_effects: true`.

## Mappa MIDI

### Ingresso (G-Board -> motore)

- **Program Change** (Program mode): con `preset_pc: true` i PC 0-7
  selezionano gli slot 0-7. Nella config viva i PC sono **ignorati**. Restano
  **one-shot**: ogni PC e' una pressione, senza debounce. Il cambio preset fa
  re-exec e lampeggia il LED dello slot.
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
  "preset_pc": false,
  "pc_effects": false,
  "flash_ms": 150
}
```

- `device`: porta rawmidi del G-Board (ingresso LED in uscita). Vuoto = niente LED.
- `mod_cc` / `drive_cc` / `boost_cc`: CC in ingresso, nell'ordine
  tremolo/univibe/flanger/phaser, fuzz/klon/tubescreamer e boost. `-1`
  disattiva una singola voce.
- `mod_led` / `drive_led` / `boost_led`: note MIDI dei LED, stesso ordine.
  `-1` lascia quel LED spento/non pilotato.
- `preset_pc`: default `false` nella config viva. Con `true` i PC 0-7
  selezionano gli slot 0-7 (Program mode = preset). Con `false` i PC non
  cambiano preset.
- `pc_effects`: default `false`. Con `true` i vecchi PC-effetto
  (`mod_pc`/`drive_pc`/`boost_pc`) tornano attivi. Se anche `preset_pc` e'
  `true`, i PC 0-7 fanno il preset (priorita' preset) e il motore avvisa
  all'avvio; gli effetti legacy restano solo per PC fuori 0-7.
- `preset_pc: false` + `pc_effects: false`: i PC sono **ignorati** (config viva).
- `mod_pc` / `drive_pc` / `boost_pc`: Program Change storici degli effetti,
  **deprecati** e non presenti nella config viva. Servono solo con
  `pc_effects: true`; `-1` li disattiva. Se un array `*_led` non c'e' nel file,
  per compatibilita' si usano i valori `*_pc` come note LED.
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

Con il motore avviato, un cambio effetto da CC (o un cambio preset dalla UI)
lascia una riga in `journalctl -u namzero-lite`, per esempio:

```
  boost: on (CC53)
  mod: tremolo on (CC54)
  preset: slot 0
slot 0: config/presets/vox_clean.json
```

Il vecchio log per-messaggio MIDI e gli script di sniffing sono stati rimossi:
per la diagnostica si usa lo stato della web UI (`/api/state`, blocco `gboard`)
e i test runtime qui sotto. Se un tasto non produce nessuna riga, il CC non e'
quello del file o il device non e' la porta `hw:V103`.

Test automatici sul Pi (iniettano i byte sulla socket BLE e controllano
stato/LED/debounce):

```bash
# CC 50-57 (mod/drive/boost), LED e debounce
python3 test/gboard_cc_runtime.py

# PC: con la config viva (entrambi i flag false) verifica che siano ignorati;
# --all prova anche gli altri tre casi cambiando config con backup e riavvio
python3 test/gboard_pc_runtime.py --mode ignored --repo ~/namzero-lite
```
