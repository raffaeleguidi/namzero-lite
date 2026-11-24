"""Bridge BLE MIDI: pedale -> socket Unix del motore.

Variabili d'ambiente:
  NAMZERO_BLE_DEVICE  nome o MAC del pedale; default FootCtrl
  NAMZERO_BLE_SOCKET  socket del motore; default /tmp/namzero-lite-ble.sock
  NAMZERO_BLE_DEBUG   1 = stampa ogni messaggio decodificato
"""

from __future__ import annotations

import asyncio
import os
import sys
import time

from ble_midi import decode_packet, describe_message
from bluez import run_session
from midi_socket import MidiSocket


def log(message: str) -> None:
    now = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(now))
    print(f"{stamp}.{int(now % 1 * 1000):03d} {message}", flush=True)


async def main() -> int:
    device = os.environ.get("NAMZERO_BLE_DEVICE", "FootCtrl")
    socket_path = os.environ.get("NAMZERO_BLE_SOCKET", "/tmp/namzero-lite-ble.sock")
    debug = os.environ.get("NAMZERO_BLE_DEBUG") == "1"

    socket = MidiSocket(socket_path, log=log)
    await socket.start()
    log(f'bridge BLE MIDI: cerco "{device}", scrivo su {socket_path}')

    def on_packet(data: bytes) -> None:
        for message in decode_packet(data):
            if debug:
                hexdump = " ".join(f"{b:02x}" for b in message)
                log(f"midi: {describe_message(message)}  [{hexdump}]")
            socket.send(message)

    try:
        delay = 2.0
        while True:
            try:
                await run_session(device, log, on_packet)
                delay = 2.0
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # il bridge non deve morire per un errore BLE
                # "In Progress" = BlueZ ha gia' un tentativo appeso: insistere
                # ogni 2s riempirebbe il journal (e la SD) senza utilita'.
                if "In Progress" in str(exc) or "InProgress" in str(exc):
                    delay = min(delay * 2.0, 30.0)
                else:
                    delay = 2.0
                log(f"errore: {exc} (ritento tra {delay:.0f}s)")
            await asyncio.sleep(delay)
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        await socket.stop()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        sys.exit(130)
