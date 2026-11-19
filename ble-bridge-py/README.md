# Bridge BLE MIDI (Python)

Versione Python del bridge: `dbus-fast` direttamente su BlueZ, ~26 MB di RAM
contro 67 MB della versione Bun. Si connette al **MAC** del pedale: connesso o
assopito il Chocolate non si annuncia, quindi la scansione per nome non serve. Il motore non cambia: parla sempre la socket
`/tmp/namzero-lite-ble.sock`.

```bash
python3 -m venv ~/ble-venv
~/ble-venv/bin/pip install -r requirements.txt
NAMZERO_BLE_DEVICE=77:CD:E5:29:2A:4F NAMZERO_BLE_DEBUG=1 ./run.sh
```

Test (non serve il pedale):

```bash
~/ble-venv/bin/python -m unittest -v
```

Documentazione completa in [docs/ble.md](../docs/ble.md).
