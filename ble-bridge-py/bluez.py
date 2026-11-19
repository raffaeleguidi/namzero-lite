"""Connessione al pedale via BlueZ (bleak) e sottoscrizione delle notifiche."""

from __future__ import annotations

import asyncio
import re

from bleak import BleakClient, BleakScanner

from ble_midi import BLE_MIDI_CHARACTERISTIC, BLE_MIDI_SERVICE

MAC = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")


async def find_address(device: str, log) -> str:
    if MAC.match(device):
        log(f"cerco per indirizzo {device}")
        return device.upper()

    log(f'cerco "{device}" (30 s)')
    found = await BleakScanner.find_device_by_name(device, timeout=30.0)
    if found is None:
        raise RuntimeError(f'nessun dispositivo BLE con "{device}" nel nome')
    log(f'trovato "{found.name}" ({found.address})')
    return found.address


async def run_session(device: str, log, on_packet) -> None:
    """Una sessione: trova il pedale, si connette, resta in ascolto.

    Ritorna quando il pedale si disconnette (o solleva un errore): il ciclo
    esterno in main.py riprova.
    """
    address = await find_address(device, log)
    async with BleakClient(address, timeout=20.0) as client:
        log(f"connesso a {address}")

        uuids = {service.uuid.lower() for service in client.services}
        if BLE_MIDI_SERVICE not in uuids:
            raise RuntimeError(
                "il dispositivo non espone il servizio BLE MIDI (03B80E5A): "
                "e' in modalita' HID/tastiera, va messo in modalita' MIDI "
                "dall'app M-Vave (e accoppiato una volta con bluetoothctl)"
            )

        await client.start_notify(BLE_MIDI_CHARACTERISTIC, lambda _handle, data: on_packet(bytes(data)))
        log("notifiche BLE MIDI attive")

        while client.is_connected:
            await asyncio.sleep(1.0)

    log("disconnesso")
