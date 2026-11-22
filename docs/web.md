# Web UI

Pagina locale servita dal Pi (nessuna dipendenza Python) che parla col socket di
controllo del motore.

## Avvio

```bash
python3 web/server.py          # http://<ip-del-pi>:8080
```

Come servizio: `./scripts/install.sh` installa e avvia `namzero-web` (porta 8080).

## Cosa c'è

- **Pedaliera**: gate, comp, filter, drive, mod, amp, IR, delay, riverbero, boost;
  ogni slot ha le manopole dei parametri principali e, dove ha senso, lo stomp.
- **Filter**: card dedicata prima del drive, con rate, depth, q, mix, low_hz,
  high_hz e position. Non ha un pulsante on/off: mostra `off` oppure `auto`
  (acceso dall'auto-engage dell'espressione). Le manopole sono sempre editabili.
- **Mod**: solo i quattro tipi (`tremolo`, `univibe`, `flanger`, `phaser`) e la
  **posizione** nella catena (`pre-amplificatore` / `dopo IR`). Cambiare
  posizione invia `set` con `slot:"mod"` e `position`: il motore salva il preset
  e lo ricarica (~300 ms), quindi la UI resta in attesa del nuovo stato.
- **Live bidirezionale**: la pagina legge lo stato dal motore 4 volte al secondo;
  le manopole mandano `param`, gli switch mandano `set`. Se accendi un effetto
  dal pedale, la UI lo mostra.
- **Slot**: pulsanti 0-7 (gli 8 preset della flash LED; la tastiera accetta 0-9 ma 8/9 sono vuoti).
- **Salva preset**: scrive tutto lo stato live nel preset corrente.
- **Upload**: modelli NAM in `models/`, IR in `irs/` (il file va poi associato
  allo slot nel preset: selezione da slot in arrivo).

## Reti WiFi

In Settings si aggiungono/rimuovono le reti conosciute (`config/wifi.json`:
ssid, password, priorita'). Il servizio `namzero-wifi` (installato da
`install.sh`, gir as root) ogni 30 s cerca una rete raggiungibile tra quelle
note e si collega con `nmcli`. L'access point di fallback non e' ancora gestito.

## API

| Metodo | Percorso | Note |
|---|---|---|
| GET | `/api/state` | snapshot dello stato |
| POST | `/api/cmd` | comando JSON al motore (`param`, `set`, `slot`, `boost`) |
| POST | `/api/save` | salva il preset |
| GET | `/api/files?kind=models|irs` | elenco file |
| GET/POST | `/api/wifi` | reti conosciute |
| POST | `/api/upload?kind=models|irs` | corpo = file, header `X-Filename` |

Il socket di controllo è `/tmp/namzero-lite-ctl.sock` (righe JSON).

## Socket di controllo (multi-client)

Il motore ascolta su `/tmp/namzero-lite-ctl.sock` (righe JSON). La web UI apre
una connessione per richiesta: manda il comando, poi `{"cmd":"state"}`, legge
l'ultima riga disponibile e chiude.

- Il motore accetta **tutte** le connessioni pendenti a ogni poll (backlog 64,
  fino a 128 client simultanei), legge da ognuno senza bloccarsi e tiene un
  buffer separato per client: un client lento non ferma gli altri.
- Lo stato e' in **broadcast**: la risposta a `state` e ogni snapshot periodico
  (circa 50 ms, solo quando lo stato cambia) vanno a tutti i client; chi si
  collega riceve subito uno snapshot, senza aspettare il poll successivo.
- I client **inattivi da piu' di 10 s** vengono chiusi dal motore, cosi' una
  connessione appesa non occupa un descrittore. Viene chiuso anche chi non
  smaltisce l'uscita (buffer oltre 1 MiB).
- Al cambio preset il motore fa re-exec: la socket viene chiusa e riaperta in
  ~0,3-0,5 s. Il client deve riconnettersi (la web UI ritenta per ~1 s).

## Cambio preset e systemd

Il re-exec usa `execv`, quindi **il PID non cambia**: systemd (`Type=simple`,
`Restart=on-failure`) non vede un riavvio e non incrementa `NRestarts`. Se il
motore esce davvero per un errore (device audio perso, `execv` fallito) esce con
codice 1, cosi' `Restart=on-failure` lo rilancia; un'uscita richiesta con
`q`/Ctrl-C resta codice 0 e il servizio si ferma.

Un'uscita 0 (`q`/Ctrl-C, SIGTERM gestito) non fa scattare `Restart=on-failure`;
un errore (codice 1) o un crash con segnale non gestito si'.

Senza sudo `deploy.sh` non puo' fermare il servizio con `systemctl`: usa
`pkill` (SIGTERM, gestito: uscita pulita). Se systemd rilancia il motore (per
esempio se il segnale arriva durante l'avvio), il processo resta col binario
vecchio: il deploy lo rileva e lo segnala. In quel caso serve
`sudo systemctl restart namzero-lite`.
