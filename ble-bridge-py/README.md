# Bridge BLE MIDI (Python)

Versione Python del bridge: `bleak` invece di `node-ble`, ~26 MB di RAM contro
67 MB e nessun CPU a riposo. Il motore non cambia: parla sempre la socket
`/tmp/namzero-lite-ble.sock`.

```bash
python3 -m venv ~/ble-venv
~/ble-venv/bin/pip install -r requirements.txt
NAMZERO_BLE_DEBUG=1 ./run.sh
```

Test (non serve il pedale):

```bash
~/ble-venv/bin/python -m unittest -v
```

Documentazione completa in [docs/ble.md](../docs/ble.md).
