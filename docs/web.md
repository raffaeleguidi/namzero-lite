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
- **Drive**: selettore del tipo (`fuzz`/`klon`/`tubescreamer`) e manopole
  amount, tone, blend, level, mid, low. Con tipo `fuzz` compaiono la manopola
  **Octave** (0..1) e, accanto, il piccolo interruttore **Octave ON/OFF**:
  l'ottava è quella stile Octavia (raddrizzamento a onda intera + passa-alto,
  non un pitch shifter). Il mix passa da `param` con `key:"octave"` (clamp 0..1),
  l'interruttore da `param` con `key:"octave_on"` (bool; accetta anche 1/0, con
  clamp). I due comandi sono indipendenti: spegnendo l'interruttore lo slider
  **resta visibile e conserva il suo valore**, ma l'ottava non viene applicata;
  riaccendendolo si ritrova il mix com'era. Il valore resta nel preset anche
  sugli altri tipi, dove viene ignorato. Cambiando tipo il render mostra/nasconde
  subito manopola e interruttore.
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
- **Live bidirezionale**: la pagina riceve lo stato dal motore su
  `GET /api/events` (SSE, Server-Sent Events): il motore manda uno snapshot in
  broadcast quando cambia e la UI lo disegna subito. Le manopole mandano
  `param`, gli switch mandano `set`. Il polling `GET /api/state` ogni 400 ms
  resta come fallback se `EventSource` non è disponibile o cade. Se accendi un
  effetto dal pedale, la UI lo mostra.
- **Accordatore**: voce **Accordatore** nel rack "Opzioni generali"; apre una
  modale grande e centrata (nota gigante, ottava, ago dei cent -50..+50 verde in
  centro, frequenza, riferimento effettivo, confidenza, toggle **Muto**).
  L'analisi è sul segnale **dry** e si aggiorna a 10-20 Hz via SSE. Si chiude
  con la X, il pulsante Chiudi, `Esc` o un clic sullo sfondo; chiudendo, il
  motore spegne l'accordatore e toglie il muto, così non si resta in silenzio.
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
  il piccolo toggle **Octave ON/OFF** del drive sale a ~40 px di altezza;
- la riga Octave (slider + toggle) resta sulla stessa riga e non va a capo: lo
  slider continua a mostrare il mix conservato anche a ottava spenta;
- il rack impila le unità a larghezza piena e gli slider EQ restano toccabili
  (bande più larghe, thumb da 20 px);
- i pedali restano in una fila scrollabile **dentro `main`**: la pagina non
  scorre in orizzontale (`html, body { overflow-x:clip }`, che a differenza di
  `hidden` non rompe l'header sticky);
- header, rack e fondo usano `env(safe-area-inset-*)` con `viewport-fit=cover`,
  quindi la UI non finisce sotto notch/home indicator.

## Accordatore

La voce **Accordatore** sta nel rack "Opzioni generali" (accanto a **Salva
opzioni**). La modale occupa quasi tutto lo schermo anche su iPhone verticale:
overlay scuro, nota gigante con ottava, ago dei cent fra -50 e +50 che diventa
verde entro ±5 cent, frequenza in Hz, riferimento effettivo e confidenza.

- **Sorgente**: il motore copia l'input **prima** della catena in un ring buffer
  lock-free; un thread dedicato analizza finestre da 4096 campioni (85 ms a
  48 kHz) ~15 volte al secondo con una YIN. Prima della decimazione x4 c'è un
  filtro anti-alias FIR (63 tap, taglio ~3,6 kHz a 48 kHz): senza, le armoniche
  dei cantini fra 6 e 12 kHz si ripiegavano nella banda e la CMNDF perdeva la
  nota o la spostava. Il segnale processato (drive, delay, riverbero...) non
  entra mai nell'analisi.
- **Accuratezza cantini e gate**: su Si3 (246,94 Hz) e Mi4 (329,63 Hz) con
  attacco, decadimento e armoniche realistici la lettura resta entro pochi cent
  per tutta la durata del decay; `tuner_test` verifica almeno il 90% di 1,5 s.
  Il gate RMS è **adattivo** con pavimento a circa -70 dBFS (prima era fisso a
  -56 dBFS): la coda debole dei cantini non sparisce più appena scende sotto
  la vecchia soglia. Il rumore di fondo stimato alza la soglia solo piano e
  solo finché resta sotto -54 dBFS; un ronzio stabile a basso livello viene
  quindi imparato e scartato dopo qualche secondo, invece di restare come nota
  fantasma.
- **Hold**: quando la confidenza scende (corda che si spegne o rumore) la nota
  e l'ago restano per **~600 ms** con la confidenza in fade, poi la UI torna a
  "—". Una nuova nota forte (confidenza ≥0,90) sostituisce subito la
  precedente; una più debole deve confermarsi per due analisi (~130 ms). Così
  non si vede la corda vecchia quando se ne suona un'altra. Spegnendo
  l'accordatore l'hold viene azzerato, quindi riaccendendolo in silenzio non
  ricompare l'ultima nota.
- **Aggiornamento**: gli eventi SSE arrivano dal broadcast del motore. La
  lettura viene pubblicata solo se cambia oltre una piccola isteresi (0.02% di
  frequenza, 5% di confidenza), quindi niente spam: al massimo ~15-20 eventi/s.
- **Muto**: `{"cmd":"tuner","mute":true}` porta l'uscita a zero con una rampa
  breve (~4 ms); l'analisi resta sul dry e continua a funzionare. Senza muto
  l'accordatore non cambia il suono. `on` e `mute` non sono persistiti: il
  motore riparte spento e non muto.
- **Riferimento e transpose**: si impostano in `settings.html` (o via comando
  `tuner`) e si persistono in `config/presets.json` → blocco `tuner`. Il
  riferimento effettivo è `reference_hz * 2^(transpose/12)`; il transpose cambia
  **solo la lettura**, non il suono. Per chi suona un semitono sotto:
  `transpose: -1` con riferimento 440 **oppure** `reference_hz: 415.3`: in
  entrambi i casi un La reale a 415.3 Hz viene letto **A4 0 cent**. Con
  `transpose: -1` un La a 440 Hz viene invece letto **A#4 0 cent**.
- **Gamma**: il riferimento accetta 415.0..450.0 Hz (default 440, passo 0.1).
  La specifica indicava 430..450, ma 415.3 Hz deve essere utilizzabile per
  l'esempio del semitono sotto, quindi il limite inferiore è 415.0. I valori
  fuori intervallo sono clampati.
- **CPU**: misurata sul Pi Zero 2W con `./build/tuner_bench` (2000 analisi su
  G3 e su Mi4, entrambi con armoniche): **~0,90 ms per analisi**, cioè **~1,3%
  di un core a 15 Hz** (~1,8% a 20 Hz). Il filtro anti-alias costa poco perché
  si calcolano solo le uscite decimate e l'accumulo è in float (vettorizzato
  NEON); la YIN in float è più veloce della versione precedente in double,
  quindi il totale non è peggiorato (prima: ~1,02 ms, ~1,5%). In silenzio il
  gate RMS esce prima della YIN, quindi il costo è ancora minore. La misura si
  rifà con
  `./scripts/cross-build.sh && scp build-arm64/tuner_bench pi@…:/tmp/ && /tmp/tuner_bench`.
- **Impostazioni**: `settings.html` ha i campi "Riferimento A4 (Hz)" e
  "Transpose (semitoni)" con il riferimento effettivo calcolato live. Il
  pulsante **Salva accordatore** manda prima `tuner` (live) e poi
  `save_options` (persistenza); anche il cambio campo (blur/Invio) salva. La
  voce **Salva opzioni** della pedaliera persiste gli stessi valori.

## Interfaccia audio

La sezione **Interfaccia audio** di `settings.html` e' l'ultimo pezzo di sistema
configurabile senza ricompilare: device ALSA, buffer, periodi, canale d'ingresso
e pulsante **Applica**.

- **Device**: select popolata da `GET /api/audio/devices`, che parsa `aplay -l`
  (card, device, nome, id) e mostra per ogni card sia `hw:...` sia
  `plughw:...`. `default` e' sempre in testa; il campo **PCM manuale** accetta
  qualunque nome ALSA (per esempio `hw:CARD=XTONE,DEV=0`).
- **Buffer / periodi**: 16..2048 frame e 2..8 periodi. Fuori range il POST
  risponde 400 e la UI non salva. Il motore fa comunque clamp all'avvio.
- **Frequenza**: mostrata fissa a **48000 Hz**. Non e' configurabile: NAM, IR,
  delay e accordatore assumono 48 kHz. Per un device che accetta solo 44.1 kHz
  si sceglie `plughw:CARD,DEV`: ALSA converte, il motore resta a 48 kHz.
- **Ingresso**: canale della capture usato dalla catena, 0 = L, 1 = R. AlsaIo
  apre sempre 2 canali; il loop audio prende il canale scelto e lo duplica su
  L/R. Per interfacce con piu' di 2 canali serve un nome/plugin ALSA che ne
  esponga 2.
- **Stato**: la riga in cima mostra il device **realmente aperto**, rate,
  buffer/periodi e ingresso. Se il configurato non si apre ma parte un fallback
  mostra `FALLBACK`; se nessun device si apre mostra l'errore in rosso.

#### Come raggiungere la web dall'AP

- `http://namzero-lite:8080` — funziona se `install.sh` ha creato il drop-in
  `/etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf` (`address=/namzero-lite/10.42.0.1`):
  i client dell'AP usano il DNS dell'AP e risolvono il nome;
- `http://namzero-lite.local:8080` — mDNS (iPhone/macOS; Android dipende);
- `http://10.42.0.1:8080` — IP dell'AP, sempre valido.

## API

`GET /api/audio` restituisce la config salvata e lo stato del motore:

```json
{
  "ok": true,
  "device": "hw:XTONE",
  "buffer": 64,
  "periods": 3,
  "input": 0,
  "rate": 48000,
  "inputs": [{"index": 0, "label": "Canale 1 (L)"},
             {"index": 1, "label": "Canale 2 (R)"}],
  "status": {"config_device": "hw:XTONE", "device": "hw:XTONE",
             "open": true, "fallback": false, "rate": 48000,
             "buffer": 64, "periods": 3, "input": 0,
             "error": "", "boot_id": 123456789}
}
```

`status` e' il blocco `audio` di `/api/state`; `boot_id` cambia a ogni re-exec
(il PID no) e serve alla UI per capire quando il motore e' davvero rinato.

`GET /api/audio/devices` restituisce:

```json
{"devices": [
  {"card": null, "device": null, "name": "PCM di sistema (ALSA default)",
   "id": "default", "pcm": "default", "plughw": "default",
   "label": "default — PCM di sistema"},
  {"card": 1, "device": 0, "name": "USB Audio", "id": "XTONE",
   "pcm": "hw:XTONE", "plughw": "plughw:XTONE",
   "label": "XTONE — USB Audio (card 1, device 0)"}
], "error": ""}
```

Il parsing e' testuale su `aplay -l` (regex sulle righe `card N: ID [Nome],
device M: ...`): niente `grep` fragili, le righe `Subdevices` vengono ignorate.
Se `aplay` manca o fallisce si restituisce comunque `default` e il campo
`error`.

`POST /api/audio` accetta `{"device": "...", "buffer": 64, "periods": 3,
"input": 0}`:

1. valida (device non vuoto, senza spazi/controlli, max 128 caratteri; buffer
   16..2048; periods 2..8; input 0..1) e risponde **400** con `{"error": "..."}`
   se qualcosa non va;
2. salva **solo** il blocco `audio` in `config/presets.json`, in modo atomico
   (file temporaneo + `os.replace`) e con backup `config/presets.json.bak`; le
   altre chiavi restano intatte;
3. manda `{"cmd":"reload"}` al motore e aspetta fino a ~8 s che lo stato
   riporti la nuova config con un `boot_id` diverso. Risponde 200 con
   `{"ok":true,"audio":{...},"reloaded":true|false,"status":{...},"warning":"..."}`.
   Se il motore non risponde, la config resta salvata e verra' applicata al
   prossimo avvio.

La UI mostra il **rotellone** durante il reload e lo toglie quando
`/api/state` riporta `audio.config_device` uguale al device scelto (o allo
scadere del timeout), poi mostra esito, fallback o errore.

### Fallback e modalita' offline

All'apertura il motore prova il device configurato, poi `default`, poi
`hw:XTONE` (doppioni saltati), con log esplicito su stderr/journal. Se uno dei
successivi si apre si suona lo stesso e lo stato segnala `fallback: true`.

Se **nessuno** si apre il motore non esce: resta attivo **senza audio** con il
socket di controllo e la web UI funzionanti, `/api/state` riporta
`audio.open: false` e `audio.error`, e da Impostazioni si corregge il device e
si preme **Applica** (che fa re-exec e riprova). Questo evita il crash loop di
systemd (`Restart=on-failure`) con un device sbagliato.

## Eventi SSE (`GET /api/events`)

`web/server.py` espone `GET /api/events` come `text/event-stream`. Per ogni
client apre una connessione persistente al socket di controllo del motore e
inoltra ogni riga di stato come:

```
data: {"slot":0,"tuner":{"on":true,"note":"A4",...},...}

```

Il flusso è pensato per la UI: `EventSource` è il canale principale, il polling
`/api/state` ogni 400 ms resta come fallback.

- **Heartbeat**: un commento `: heartbeat` ogni ~15 s tiene viva la connessione
  attraverso proxy e NAT. Verso il motore parte invece `{"cmd":"ping"}` ogni
  ~4 s: il motore chiude i client che non mandano comandi da ~10 s, e il ping
  evita che una connessione SSE venga scartata.
- **Re-exec del motore**: il bridge SSE resta aperto e si riconnette da solo
  alla socket ctl quando il motore rinasce (cambio preset, ~0,3-1 s): la UI non
  vede una disconnessione e il rotellone copre la pausa. Il browser riconnette
  comunque da solo se cade il web server (`retry: 1500`).
- **Chiusura**: se il client chiude, `read` sul socket HTTP torna 0 e il thread
  termina subito; nessun client appeso.
- **Uso da terminale**: `curl -N http://127.0.0.1:8080/api/events` stampa gli
  eventi in tempo reale (Ctrl-C per uscire).

## Reti WiFi e AP di fallback

La config viva è `config/wifi.json` (fonte di verità, `deploy.sh` non la
sovrascrive):

```json
{
  "networks": [
    {"ssid": "Casa", "password": "…", "priority": 2}
  ],
  "ap": {
    "enabled": true,
    "ssid": "namzero-lite",
    "password": "namzero-lite"
  }
}
```

Il servizio `namzero-wifi` (root, installato da `install.sh`) gestisce `wlan0`
con NetworkManager. Se manca solo la unit:

```bash
sudo cp systemd/namzero-wifi.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now namzero-wifi
```

- **Avvio**: se non c'è già una STA attiva, prova le reti note in ordine di
  priorità per **~10 s** (scan + `nmcli connection up`, creando la connessione
  se manca).
- **AP di fallback**: se nessuna rete nota è raggiungibile e `ap.enabled` è
  `true`, chiude la STA e avvia un hotspot **WPA2** su `wlan0` (profilo NM
  `namzero-lite-ap`, `ipv4.method shared`: DHCP e instradamento li gestisce
  NetworkManager, che per il DHCP usa `dnsmasq-base`, già presente su
  Raspberry Pi OS). L'IP dell'AP è di solito `10.42.0.1`; la web resta su
  `0.0.0.0:8080` e lo script verifica una GET su `127.0.0.1` e sull'IP
  dell'AP.
- **Ritorno in STA**: in AP riscansiona ogni **~50 s**. Appena una rete nota è
  visibile, chiude l'AP e si collega; se il collegamento fallisce riapre l'AP.
- **Perdita della STA**: prova a ricollegarsi; dopo **due controlli** senza
  link e senza reti note (isteresi, ~30-60 s) torna all'AP.
- **Una sola radio**: AP e STA non sono mai attivi insieme. La STA viene
  chiusa prima di avviare l'AP e l'AP prima di tornare in STA.
- **STA già attiva**: se c'è già una connessione (anche una rete non in
  elenco, per esempio una connessione netplan), il servizio la lascia stare.
  L'AP parte solo quando non c'è più link: così non butta giù una connessione
  funzionante (e l'SSH).
- **Credenziali AP**: SSID e password si modificano da Impostazioni. La
  password deve avere **8-63 caratteri** (WPA2) e la UI rifiuta valori più
  corti quando l'AP è abilitato. Default documentato: SSID `namzero-lite`,
  password `namzero-lite`; **cambiarla**. Le modifiche vengono rilevate entro
  ~50 s e, se l'AP è attivo, lo script lo riavvia. Aggiungere/rimuovere reti
  note non tocca il blocco `ap`.
- **Log**: tag `[wifi]` nel journal, senza password (i comandi sono
  redatti).
- **Comandi di diagnostica**:

  ```bash
  ./scripts/wifi-connect.sh --dry-run                  # stato + decisione
  ./scripts/wifi-connect.sh --dry-run --simulate no-link
  ./scripts/wifi-connect.sh --dry-run --simulate known
  ./scripts/wifi-connect.sh --test                     # test delle decisioni
  python3 test/wifi_manager_test.py
  journalctl -u namzero-wifi -f
  nmcli -t -f DEVICE,STATE,CONNECTION device status
  curl -s http://127.0.0.1:8080/api/wifi/status
  ```

  `--dry-run` legge lo stato con `nmcli` ma non esegue mai comandi che
  modificano NetworkManager. `--simulate no-link` e `--simulate known`
  ignorano lo stato corrente per verificare le due decisioni (AP o STA) in
  sicurezza.

### Test manuale dell'hotspot (rischioso)

Attivare l'AP butta giù l'SSH se passa su `wlan0`. **Prima** di avviarlo,
programmare un revert automatico staccato; sostituire `<rete-abituale>` con il
nome della connessione NM della rete abituale (si vede con
`nmcli -t -f NAME,TYPE connection show`):

```bash
# sul Pi, PRIMA di toccare l'AP (il revert gira da root, come il servizio)
sudo sh -c '(sleep 120; nmcli connection down namzero-lite-ap; \
  nmcli connection up "<rete-abituale>"; logger -t wifi "revert AP -> STA")' \
  >/tmp/wifi-revert.log 2>&1 &

# poi, con sudo (il servizio è root)
sudo systemctl restart namzero-wifi

# dal telefono: collegarsi a SSID "namzero-lite" e aprire http://10.42.0.1:8080
# dopo ~120 s, quando il revert è scattato:
journalctl -u namzero-wifi -n 100 --no-pager
nmcli -t -f DEVICE,STATE,CONNECTION device status
```

Se non si riesce a verificare in sicurezza, **non attivare l'AP**: `--dry-run`,
`--simulate` e `--test` coprono le decisioni. Il test reale serve solo a
validare hotspot, DHCP e raggiungibilità della web dal telefono.

## Caricamento

Cambio preset, cambio modello/IR, spostamento del mod e **Applica** nella
sezione Interfaccia audio ricostruiscono il motore (re-exec, ~0,5-2 s): la
pagina mostra un **rotellone** a tutto schermo e lo toglie solo quando lo stato
riporta il nuovo slot **e** il nuovo path preset (o, per i cambi file/posizione,
al primo stato dopo il riavvio). Timeout di sicurezza 6 s (8 s per l'audio).

I file statici (`index.html`, `ui.js`) sono riletti da disco a ogni richiesta,
quindi una modifica alla UI si vede ricaricando la pagina. `GET /api/presets`
invece è codice Python: dopo un aggiornamento bisogna riavviare `namzero-web`.
Senza sudo si può verificare su un'istanza manuale:

```bash
NAMZERO_WEB_PORT=8090 python3 web/server.py &
curl -s http://127.0.0.1:8090/api/presets
```

Con sudo: `sudo systemctl restart namzero-web`.

### Come raggiungere la web dall'AP

- `http://namzero-lite:8080` — funziona se `install.sh` ha creato il drop-in
  `/etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf` (`address=/namzero-lite/10.42.0.1`):
  i client dell'AP usano il DNS dell'AP e risolvono il nome;
- `http://namzero-lite.local:8080` — mDNS (iPhone/macOS; Android dipende);
- `http://10.42.0.1:8080` — IP dell'AP, sempre valido.

## API

| Metodo | Percorso | Note |
|---|---|---|
| GET | `/api/state` | snapshot dello stato (fallback del polling) |
| GET | `/api/events` | SSE: `data:` con lo snapshot a ogni cambiamento, heartbeat 15 s |
| GET | `/api/presets` | elenco preset: `[{"slot":0,"name":"vox_wah"}, ...]` da `config/presets.json` |
| GET | `/api/audio` | config audio + ingressi + stato del motore (`open`, `fallback`, errore) |
| GET | `/api/audio/devices` | device da `aplay -l` con card/device/nome/id e nomi `hw`/`plughw` |
| POST | `/api/audio` | valida e salva il blocco `audio`, poi reload del motore (200/400) |
| POST | `/api/cmd` | comando JSON al motore (`param`, `set`, `slot`, `boost`, `eq`, `tuner`) |
| POST | `/api/save` | salva il preset |
| GET | `/api/files?kind=models|irs` | elenco file |
| GET | `/api/wifi` | reti conosciute + blocco `ap` |
| POST | `/api/wifi` | salva reti e/o blocco `ap` (password AP 8-63 se abilitato) |
| GET | `/api/wifi/status` | `{"mode":"sta|ap|none","ssid":"…","ip":"…","ap_ssid":"…"}` da `nmcli` |
| POST | `/api/upload?kind=models|irs` | corpo = file, header `X-Filename` |

Il socket di controllo è `/tmp/namzero-lite-ctl.sock` (righe JSON).

## Socket di controllo (multi-client)

Il motore ascolta su `/tmp/namzero-lite-ctl.sock` (righe JSON). La web UI apre
una connessione per richiesta: manda il comando, poi `{"cmd":"state"}`, legge
l'ultima riga disponibile e chiude. L'unica eccezione è SSE: `/api/events` tiene
una connessione aperta finché il client resta collegato.

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
  ~0,3-0,5 s. Il client deve riconnettersi: la web UI (EventSource) lo fa da
  sola, il bridge SSE riconnette la socket ctl senza chiudere lo stream.

## Cambio preset e systemd

Il re-exec usa `execv`, quindi **il PID non cambia**: systemd (`Type=simple`,
`Restart=on-failure`) non vede un riavvio e non incrementa `NRestarts`. Se il
motore esce davvero per un errore (device audio perso **durante** il
funzionamento, `execv` fallito) esce con codice 1, cosi' `Restart=on-failure` lo
rilancia; un'uscita richiesta con `q`/Ctrl-C resta codice 0 e il servizio si
ferma. All'avvio, se **nessun** device audio si apre, non esce: resta in
modalita' offline (vedi sopra) per non entrare in crash loop.

Un'uscita 0 (`q`/Ctrl-C, SIGTERM gestito) non fa scattare `Restart=on-failure`;
un errore (codice 1) o un crash con segnale non gestito si'.

Senza sudo `deploy.sh` non puo' fermare il servizio con `systemctl`: usa
`pkill` (SIGTERM, gestito: uscita pulita). Se systemd rilancia il motore (per
esempio se il segnale arriva durante l'avvio), il processo resta col binario
vecchio: il deploy lo rileva e lo segnala. In quel caso serve
`sudo systemctl restart namzero-lite`.
