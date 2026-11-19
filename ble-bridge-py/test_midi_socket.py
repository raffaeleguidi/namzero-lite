"""Test della coda verso il motore: il primo CC non deve andare perso.

Il caso reale: il bridge e' connesso al pedale, il motore sta ancora partendo e
la sua socket non esiste. Il messaggio deve restare in coda e uscire appena la
socket compare.
"""

from __future__ import annotations

import asyncio
import os
import tempfile
import unittest

from midi_socket import MidiSocket


class QueueTest(unittest.IsolatedAsyncioTestCase):
    async def test_messaggi_in_coda_prima_che_il_motore_esista(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "motore.sock")
            received = bytearray()

            async def handler(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
                while True:
                    chunk = await reader.read(64)
                    if not chunk:
                        break
                    received.extend(chunk)
                writer.close()

            client = MidiSocket(path, retry_seconds=0.1)
            await client.start()
            try:
                # Il motore non c'e' ancora: questi due messaggi devono restare in coda.
                client.send(bytes([0xB0, 0x20, 0x00]))
                client.send(bytes([0xB0, 0x21, 0x00]))
                await asyncio.sleep(0.15)
                self.assertEqual(len(received), 0)

                server = await asyncio.start_unix_server(handler, path=path)
                try:
                    for _ in range(50):
                        if len(received) >= 6:
                            break
                        await asyncio.sleep(0.05)
                    self.assertEqual(
                        bytes(received), bytes([0xB0, 0x20, 0x00, 0xB0, 0x21, 0x00])
                    )
                finally:
                    server.close()
            finally:
                # Prima il client, poi il server: con una connessione aperta
                # Server.wait_closed() aspetterebbe all'infinito.
                await client.stop()


if __name__ == "__main__":
    unittest.main()
