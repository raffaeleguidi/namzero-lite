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
- **Filter**: card dedicata prima del drive, con un selettore **Tipo**
  (`wah`/`whammy`). Con `wah` mostra rate, depth, q, mix, low_hz e high_hz; con
  `whammy` mostra range (semitoni, -24..+24) e mix. La manopola **Pos** resta
  sempre visibile: è la posizione del pedale di espressione (0 = unisono,
  1 = range pieno per il whammy). Non ha un pulsante on/off: mostra `off` oppure
  `auto` (acceso dall'auto-engage dell'espressione, uguale per entrambi i tipi).
  Il cambio tipo invia `set` con `slot:"filter"` e `type`; range e mix passano da
  `param` (`key:"range"` / `key:"mix"`). Default dello slot: `wah`, range 12,
  mix 1.
- **Mod**: solo i quattro tipi (`tremolo`, `univibe`, `flanger`, `phaser`) e la
  **posizione** nella catena (`pre-amplificatore` / `dopo IR`). Cambiare
  posizione invia `set` con `slot:"mod"` e `position`: il motore salva il preset
  e lo ricarica (~300 ms), quindi la UI resta in attesa del nuovo stato.
- **Live bidirezionale**: la pagina legge lo stato dal motore 4 volte al secondo;
  le manopole mandano `param`, gli switch mandano `set`. Se accendi un effetto
  dal pedale, la UI lo mostra.
- **EQ globale**: unità nel rack "Opzioni generali", fra Boost e Auto-engage, con
  cinque slider verticali compatti (80 / 250 / 800 / 2.5k / 8k Hz, -12..+12 dB,
  passo 0.5). Il pulsante on/off manda `{"cmd":"eq","on":...}`, ogni slider manda
  `{"cmd":"eq","band":i,"gain":dB}`: tutto live, senza ricaricare il preset.
  L'EQ è **sempre in catena**, in fondo (dopo riverbero e looper, prima del gain
  master/boost).
- **Volume globale**: slider "Volume" nella stessa unità dell'EQ (-24..+24 dB,
  passo 0.5, default **+6 dB**), comando `{"cmd":"eq","volume":dB}` con debounce
  come le altre manopole. È il gain di fine catena: si applica **sempre**, anche
  a EQ spento, e si somma al boost. **Salva opzioni** scrive `eq.on`,
  `eq.gains` e `eq.volume_db` in `config/presets.json`, così vale per tutti i
  preset.
- **Preset**: il titolo nello header è una **combo** con tutti gli slot
  configurati (`[slot]` resta a sinistra). La lista arriva da `GET /api/presets`,
  che legge `config/presets.json` e ignora gli slot vuoti/null. Scegliendo una
  voce la pagina manda `{"cmd":"slot","n":i}`: il motore ricarica il preset e la
  pagina mostra il rotellone. La combo segue lo slot live (`st.slot`), quindi si
  aggiorna anche cambiando preset da tastiera o dal pedale. Non c'è più
  l'input per rinominare: la rinomina resta disponibile solo via comando
  `rename` sul socket.
- **Salva preset**: scrive tutto lo stato live nel preset corrente.
- **Upload**: modelli NAM in `models/`, IR in `irs/` (il file va poi associato
  allo slot nel preset: selezione da slot in arrivo).

## Header e layout mobile

Sopra i **720 px** lo header mostra tutto su una riga: titolo, `[slot]` + combo
preset, undo, Salva, settings.

Sotto i 720 px lo header resta compatto su una riga: titolo, `[slot]` + combo
(la combo si restringe, `max-width:45vw`) e un pulsante **hamburger** a destra.
Undo, Salva e settings si spostano in un menu a tendina (`#actions`, che si apre
con la classe `.open`): si chiude cliccando fuori, con `Esc`, dopo un'azione o
quando la finestra torna larga. Sono gli stessi elementi con gli stessi handler.

Sempre sotto i 720 px:

- i campi numerici e le select passano a `font-size:16px`, così iOS non fa zoom
  al focus;
- i pulsanti on/off e i comandi del looper hanno un touch target di ~40-44 px;
- il rack impila le unità a larghezza piena e gli slider EQ restano toccabili
  (bande più larghe, thumb da 20 px);
- i pedali restano in una fila scrollabile **dentro `main`**: la pagina non
  scorre in orizzontale (`html, body { overflow-x:clip }`, che a differenza di
  `hidden` non rompe l'header sticky);
- header, rack e fondo usano `env(safe-area-inset-*)` con `viewport-fit=cover`,
  quindi la UI non finisce sotto notch/home indicator.

## Reti WiFi

In Settings si aggiungono/rimuovono le reti conosciute (`config/wifi.json`:
ssid, password, priorita'). Il servizio `namzero-wifi` (installato da
`install.sh`, gir as root) ogni 30 s cerca una rete raggiungibile tra quelle
note e si collega con `nmcli`. L'access point di fallback non e' ancora gestito.

## Caricamento

Cambio preset, cambio modello/IR e spostamento del mod ricostruiscono il motore
(re-exec, ~0,5-2 s): la pagina mostra un **rotellone** a tutto schermo e lo toglie
solo quando lo stato riporta il nuovo slot **e** il nuovo path preset (o, per i
cambi file/posizione, al primo stato dopo il riavvio). Timeout di sicurezza 6 s.

I file statici (`index.html`, `ui.js`) sono riletti da disco a ogni richiesta,
quindi una modifica alla UI si vede ricaricando la pagina. `GET /api/presets`
invece è codice Python: dopo un aggiornamento bisogna riavviare `namzero-web`.
Senza sudo si può verificare su un'istanza manuale:

```bash
NAMZERO_WEB_PORT=8090 python3 web/server.py &
curl -s http://127.0.0.1:8090/api/presets
```

Con sudo: `sudo systemctl restart namzero-web`.

## API

| Metodo | Percorso | Note |
|---|---|---|
| GET | `/api/state` | snapshot dello stato |
| GET | `/api/presets` | elenco preset: `[{"slot":0,"name":"vox_wah"}, ...]` da `config/presets.json` |
| POST | `/api/cmd` | comando JSON al motore (`param`, `set`, `slot`, `boost`, `eq`) |
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
- I client che **non mandano comandi da piu' di 10 s** vengono chiusi dal
  motore, cosi' una connessione appesa non occupa un descrittore (vale anche se
  stanno leggendo i broadcast). Viene chiuso anche chi non smaltisce l'uscita
  (buffer oltre 1 MiB).
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
