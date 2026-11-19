"""Client verso il motore: il motore fa da server sulla socket Unix.

Il punto delicato e' l'avvio: il bridge e' gia' connesso al pedale quando il
motore non e' ancora in ascolto. Se il primo CC arrivasse in quel momento e lo
scartassimo, si perderebbe sempre il primo tasto dopo ogni avvio. Per questo i
messaggi finiscono in una coda e vengono svuotati appena la socket esiste; la
coda e' limitata, cosi' un motore spento a lungo non fa crescere la memoria.
"""

from __future__ import annotations

import asyncio
import socket
from collections import deque


class MidiSocket:
    def __init__(self, path: str, log=lambda _message: None, retry_seconds: float = 0.5) -> None:
        self._path = path
        self._log = log
        self._retry = retry_seconds
        self._sock: socket.socket | None = None
        self._queue: deque[bytes] = deque(maxlen=512)
        self._task: asyncio.Task | None = None
        self._was_connected = False

    async def start(self) -> None:
        self._task = asyncio.create_task(self._connect_loop())

    async def stop(self) -> None:
        if self._task is not None:
            self._task.cancel()
            try:
                await self._task
            except asyncio.CancelledError:
                pass
            self._task = None
        self._close()

    def send(self, data: bytes) -> None:
        """Accoda e prova a scrivere subito; se non c'e' la socket resta in coda."""
        self._queue.append(data)
        self._flush()

    def _flush(self) -> None:
        sock = self._sock
        if sock is None:
            return
        try:
            while self._queue:
                sock.sendall(self._queue[0])
                self._queue.popleft()
        except OSError as exc:
            self._log(f"socket: {exc}")
            self._close()

    def _close(self) -> None:
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None
        if self._was_connected:
            self._was_connected = False
            self._log("socket: disconnesso")

    async def _connect_loop(self) -> None:
        while True:
            if self._sock is None:
                try:
                    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                    sock.setblocking(True)
                    sock.connect(self._path)
                except OSError:
                    pass  # il motore non c'e' ancora: si riprova
                else:
                    self._sock = sock
                    self._was_connected = True
                    self._log(f"socket: connesso a {self._path}")
            self._flush()
            await asyncio.sleep(self._retry)
