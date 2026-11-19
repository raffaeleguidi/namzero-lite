# Bridge BLE MIDI

Pont tra un controller BLE MIDI (M-Vave Chocolate) e il motore, senza toccare il
processo audio: un processo separato in Bun parla con BlueZ via D-Bus, decodifica
i pacchetti BLE-MIDI e scrive i byte puliti su una socket Unix dove il motore e'
in ascolto.

```
Chocolate ──BLE──▶ ble-midi-bridge (Bun) ──Unix socket──▶ namzero-lite
                   src/ (questo repo)        /tmp/namzero-lite-ble.sock
```

Perche' cosi': BlueZ e D-Bus non sono realtime e possono bloccarsi; il motore
non deve accorgersene. Se il bridge muore, il pedale USB (XTONE) continua a
funzionare e il motore resta in piedi. Quando il bridge torna, si riconnette.

## Cosa serve sul Pi

1. **Bluetooth acceso** (di default e' off):

```bash
sudo hciconfig hci0 up          # oppure: bluetoothctl power on
rfkill list                     # se e' bloccato: sudo rfkill unblock bluetooth
```

2. **Permessi D-Bus** per l'utente che esegue il bridge (una volta sola):

```bash
echo '<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-BUS Bus Configuration 1.0//EN"
  "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <policy user="pi">
    <allow own="org.bluez"/>
    <allow send_destination="org.bluez"/>
    <allow send_interface="org.bluez.GattCharacteristic1"/>
    <allow send_interface="org.bluez.GattDescriptor1"/>
    <allow send_interface="org.freedesktop.DBus.ObjectManager"/>
    <allow send_interface="org.freedesktop.DBus.Properties"/>
  </policy>
</busconfig>' | sudo tee /etc/dbus-1/system.d/node-ble.conf >/dev/null
sudo systemctl restart dbus
```

Senza questo file il bridge riceve `AccessDenied` da D-Bus.

3. **Il Chocolate in modalita' MIDI**: va impostata dall'app M-Vave. Nella
modalita' giusta il dispositivo si annuncia via BLE con un nome che contiene
"Chocolate"; il bridge lo cerca per nome (o per MAC, vedi sotto).

## Compilare (dal Mac)

```bash
cd ble-bridge
TMPDIR=$PWD/.tmp BUN_INSTALL_CACHE_DIR=$PWD/.bun-cache bun install
bun run build          # bun build --compile --target=bun-linux-arm64
```

Produce `dist/ble-midi-bridge`, un binario aarch64 con il runtime Bun dentro
(~95 MB). Si copia sul Pi:

```bash
scp dist/ble-midi-bridge pi@namzero-lite:/usr/local/bin/
ssh pi@namzero-lite 'sudo chmod +x /usr/local/bin/ble-midi-bridge'
```

Non serve Bun sul Pi: il binario e' autosufficiente.

## Avviare

```bash
NAMZERO_BLE_DEBUG=1 /usr/local/bin/ble-midi-bridge
```

Variabili:

| Variabile | Default | Note |
|-----------|---------|------|
| `NAMZERO_BLE_DEVICE` | `Chocolate` | parte del nome BLE **oppure** MAC (`AA:BB:...`) |
| `NAMZERO_BLE_SOCKET` | `/tmp/namzero-lite-ble.sock` | deve combaciare con `midi.ble.socket` in `config/midi.json` |
| `NAMZERO_BLE_DEBUG` | `0` | `1` stampa ogni messaggio decodificato |

Il motore fa da **server** della socket: va avviato per primo (o comunque la
socket viene creata quando parte; il bridge riprova ogni secondo).

Output atteso in debug:

```
19:41:02.113 connesso a Chocolate (C4:3A:BE:12:34:56)
19:41:02.340 notifiche BLE MIDI attive
19:41:05.881 midi: cc ch1 40 127  [b0 28 7f]
```

## systemd

```ini
# /etc/systemd/system/namzero-ble-bridge.service
[Unit]
Description=namzero-lite BLE MIDI bridge
After=bluetooth.target
Wants=bluetooth.target

[Service]
User=pi
Environment=NAMZERO_BLE_DEVICE=Chocolate
Environment=NAMZERO_BLE_SOCKET=/tmp/namzero-lite-ble.sock
ExecStart=/usr/local/bin/ble-midi-bridge
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

Il bridge usa `Restart=always`: se il Chocolate va in sleep o si disconnette,
la sessione BLE finisce e il ciclo esterno riprova dopo 2 s.

## Mappare i tasti (un tasto = un effetto)

I CC che il Chocolate manda si configurano dall'app M-Vave (consiglio:
`CC 40/41/42/43`, uno per tasto). Poi in `config/midi.json`:

```json
"mod_select": { "tremolo": 40, "univibe": 41, "flanger": 42, "phaser": 43 }
```

Comportamento: il primo tasto accende quell'effetto (e spegne il precedente,
perche' lo slot e' uno solo); ripremere lo stesso tasto lo spegne. Ogni selezione
imposta anche rate e feedback suggeriti per il tipo.

Il **wah** non e' mappabile qui: lo comanda solo il pedale di espressione
(auto-engage). Se un CC di `mod_select` collide con un CC di uno slot o con
l'espressione, `midi.json` viene rifiutato all'avvio con un messaggio esplicito.

## Se non funziona

| Sintomo | Causa |
|---------|-------|
| `Resource Not Ready` | adattatore BT spento: `sudo hciconfig hci0 up` |
| `AccessDenied` / `Not Authorized` | manca il file D-Bus di cui sopra |
| `nessun dispositivo BLE con "Chocolate" nel nome` | dispositivo spento, non in modalita' MIDI, o gia' connesso a un altro host |
| Connesso ma nessun messaggio | il Chocolate e' in modalita' HID/tastiera: va messo in MIDI dall'app |
| Messaggi nel log ma gli effetti non cambiano | CC diversi da quelli in `mod_select`, o motore non in ascolto sulla stessa socket |

Verifica rapida del device:

```bash
bluetoothctl
[bluetooth]# power on
[bluetooth]# scan on          # cerca "Chocolate"
```

## Perche' Bun e non Python

Provato per scelta: il binario si cross-compila dal Mac e sul Pi non serve un
runtime. Se in futuro BlueZ o `node-ble` dessero problemi, l'interfaccia e' solo
la socket: si riscrive il bridge in Python (`bleak`) senza toccare il motore.
Il decoder BLE-MIDI ha i suoi test (`bun test`) e non dipende da BlueZ.
