# Bridge BLE MIDI

Collega un controller BLE MIDI (M-Vave Chocolate) al motore senza toccare il
processo audio: un processo separato parla con BlueZ via D-Bus, decodifica i
pacchetti BLE-MIDI e scrive i byte puliti su una socket Unix dove il motore e'
in ascolto.

```
Chocolate ──BLE──▶ bridge (Python/dbus-fast) ──Unix socket──▶ namzero-lite
                    ble-bridge-py/            /tmp/namzero-lite-ble.sock
```

Perche' cosi': BlueZ e D-Bus non sono realtime e possono bloccarsi; il motore
non deve accorgersene. Se il bridge muore, il pedale USB (XTONE) continua a
funzionare e il motore resta in piedi. Quando il bridge torna, si riconnette.

**Versione di default: Python** (`ble-bridge-py/`, `dbus-fast`). Misurata sul Pi:
**~26 MB di RAM e ~0% di CPU a riposo**, contro 67 MB e 5,2% della versione Bun
(`ble-bridge/`, tenuta come esperimento). Ogni MB conta su 415 MB con zram.

## Cosa serve sul Pi

1. **Bluetooth acceso** (di default e' off):

```bash
sudo hciconfig hci0 up          # oppure: bluetoothctl power on
rfkill list                     # se e' bloccato: sudo rfkill unblock bluetooth
```

2. **Il Chocolate in modalita' MIDI**: si imposta dall'app M-Vave. Serve il **MAC**
   del pedale: da connesso (o assopito) non si annuncia, quindi non si cerca per
   nome ma ci si connette direttamente all'indirizzo.

3. **Accoppiare una volta** (senza bonding il pedale non espone il servizio MIDI:
   si connette, non risolve il GATT e il link cade):

```bash
bluetoothctl
[bluetooth]# agent NoInputNoOutput
[bluetooth]# default-agent
[bluetooth]# scan on                 # attendere che compaia FootCtrl
[bluetooth]# pair 77:CD:E5:29:2A:4F
[bluetooth]# trust 77:CD:E5:29:2A:4F
[bluetooth]# quit
```

Dopo il pairing, `bluetoothctl info` mostra `Paired: yes` e tra gli UUID compare
`03b80e5a-ede8-4b33-a751-6ce34ec4c700` (BLE MIDI). Il MAC va sostituito con
quello del proprio pedale.

## Installazione (Python)

```bash
sudo apt install -y python3-venv          # solo se manca
python3 -m venv ~/ble-venv
~/ble-venv/bin/pip install -r ~/namzero-lite/ble-bridge-py/requirements.txt
```

Avvio manuale:

```bash
cd ~/namzero-lite/ble-bridge-py
NAMZERO_BLE_DEBUG=1 ./run.sh
```

Variabili:

| Variabile | Default | Note |
|-----------|---------|------|
| `NAMZERO_BLE_DEVICE` | `77:CD:E5:29:2A:4F` | **MAC** del pedale (obbligatorio: la ricerca per nome non funziona quando e' connesso) |
| `NAMZERO_BLE_SOCKET` | `/tmp/namzero-lite-ble.sock` | deve combaciare con `midi.ble.socket` in `config/midi.json` |
| `NAMZERO_BLE_DEBUG` | `0` | `1` stampa ogni messaggio decodificato |
| `BLE_PYTHON` | `~/ble-venv/bin/python` | interprete usato da `run.sh` |

Output atteso in debug:

```
20:43:00.337 bridge BLE MIDI: cerco "77:CD:E5:29:2A:4F", scrivo su /tmp/namzero-lite-ble.sock
20:43:00.900 connessione a 77:CD:E5:29:2A:4F
20:43:01.226 connesso a 77:CD:E5:29:2A:4F
20:43:01.227 notifiche BLE MIDI attive
20:43:05.881 midi: cc ch1 32 0  [b0 20 00]
```

## Il primo CC non si perde

Se il pedale manda un tasto mentre il motore sta ancora partendo, la socket non
esiste ancora. Il bridge **accoda** i messaggi e li svuota appena la socket
compare (coda limitata a 512, cosi' un motore spento a lungo non consuma RAM).
Lo stesso vale quando il motore si riavvia. C'e' un test apposta
(`test_midi_socket.py`: messaggi inviati prima che il server esista).

## systemd

```ini
# /etc/systemd/system/namzero-ble-bridge.service
[Unit]
Description=namzero-lite BLE MIDI bridge
After=bluetooth.target
Wants=bluetooth.target

[Service]
User=pi
Environment=NAMZERO_BLE_DEVICE=77:CD:E5:29:2A:4F
Environment=NAMZERO_BLE_SOCKET=/tmp/namzero-lite-ble.sock
ExecStart=/home/pi/ble-venv/bin/python /home/pi/namzero-lite/ble-bridge-py/main.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now namzero-ble-bridge
journalctl -u namzero-ble-bridge -f
```

Il bridge usa `Restart=always`: se il Chocolate va in sleep o si disconnette, la
sessione BLE finisce e il ciclo esterno riprova dopo 2 s.

## Mappare i tasti

Il Chocolate configurato con l'app M-Vave manda, di default, **CC 32/33/34/35**
(uno per tasto, un solo messaggio per pressione con valore 0). In `config/midi.json`:

```json
"mod_select":   { "tremolo": 32, "univibe": 33, "flanger": 34, "phaser": 35 },
"drive_select": { "fuzz": 36, "klon": 37, "tubescreamer": 38 },
"delay_select": { "stereo": 45, "ping_pong": 46, "ping_pong_stereo": 47 },
"boost":        { "cc": 44, "percent": 20 }
```

Tutti i gruppi `*_select` sono radio button: spento + tasto → accende con quella
scelta; altro tasto → cambia; stesso tasto → spegne. Il **wah** non e' mappabile:
lo comanda solo il pedale di espressione (auto-engage). Dettagli in
[midi.md](midi.md).

## Se non funziona

| Sintomo | Causa |
|---------|-------|
| `Resource Not Ready` | adattatore BT spento: `sudo hciconfig hci0 up` |
| `AccessDenied` / `Not Authorized` | mancano i permessi D-Bus per l'utente |
| `BlueZ non conosce il MAC` | pedale mai accoppiato, o MAC sbagliato |
| `il dispositivo non espone il servizio BLE MIDI` | e' in modalita' HID/tastiera, o non e' accoppiato |
| Connesso ma nessun messaggio | il Chocolate e' in modalita' HID: va messo in MIDI dall'app |
| Messaggi nel log ma gli effetti non cambiano | CC diversi da quelli in `midi.json`, o motore non in ascolto |

Verifica rapida del device:

```bash
bluetoothctl
[bluetooth]# power on
[bluetooth]# scan on          # cerca "FootCtrl"
```

## La versione Bun

`ble-bridge/` resta come esperimento documentato: stessa architettura, decoder
con 7 test, si cross-compila dal Mac con
`bun build --compile --target=bun-linux-arm64 --external x11`. Funziona, ma sul
Pi costa 67 MB e ~5% di CPU a riposo; per questo il default e' Python. Se un
giorno servisse, il motore non cambia: parla sempre la stessa socket.
