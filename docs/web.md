# Web UI

Pagina locale servita dal Pi (nessuna dipendenza Python) che parla col socket di
controllo del motore.

## Avvio

```bash
python3 web/server.py          # http://<ip-del-pi>:8080
```

Come servizio: `./scripts/install.sh` installa e avvia `namzero-web` (porta 8080).

## Cosa c'è

- **Pedaliera**: gate, comp, drive, mod, amp, IR, delay, riverbero, boost;
  ogni slot ha lo stomp e le manopole dei parametri principali.
- **Live bidirezionale**: la pagina legge lo stato dal motore 4 volte al secondo;
  le manopole mandano `param`, gli switch mandano `set`. Se accendi un effetto
  dal pedale, la UI lo mostra.
- **Slot**: pulsanti 0-9 (stessi slot della tastiera e dell'XTONE).
- **Salva preset**: scrive tutto lo stato live nel preset corrente.
- **Upload**: modelli NAM in `models/`, IR in `irs/` (il file va poi associato
  allo slot nel preset: selezione da slot in arrivo).

## API

| Metodo | Percorso | Note |
|---|---|---|
| GET | `/api/state` | snapshot dello stato |
| POST | `/api/cmd` | comando JSON al motore (`param`, `set`, `slot`, `boost`) |
| POST | `/api/save` | salva il preset |
| GET | `/api/files?kind=models|irs` | elenco file |
| POST | `/api/upload?kind=models|irs` | corpo = file, header `X-Filename` |

Il socket di controllo è `/tmp/namzero-lite-ctl.sock` (righe JSON).
