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

I LED si accendono e si spengono ricevendo **specifiche note MIDI** (comportamento
del firmware del G-Board). Il file `.imap` qui sopra descrive solo i messaggi dei
pulsanti: la mappa delle note per i LED **non è inclusa**.

Per ricavarla, con il G-Board collegato:

```bash
amidi -l                       # individuare hw:V103
amidi -p hw:V103 -S "90 00 7F" # Note On canale 1, nota 0, velocity 127
amidi -p hw:V103 -S "80 00 7F" # Note Off della stessa nota
```

e osservare quale LED cambia, ripetendo con note diverse. Una volta nota la
mappa, il motore può accendere il LED dello slot attivo (o degli effetti accesi):
serve però un'**uscita MIDI** verso `hw:V103`, che oggi non c'è — il motore è
solo in ingresso. È il passo naturale dopo la mappa degli ingressi.
