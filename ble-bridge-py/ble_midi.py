"""Decodifica dei pacchetti BLE-MIDI.

Formato (Apple BLE-MIDI): ogni pacchetto inizia con un header (bit 7 a 1), poi
un timestamp, poi le MIDI. La regola che conta e' quella del parser ESPHome: un
byte con bit 7 a 1 e' uno status *solo* se arriva subito dopo un timestamp;
altrimenti e' un timestamp. Per questo non basta "togliere i byte con bit 7
alto": si perderebbe il primo status della nota.

I SysEx non servono al bridge (il Chocolate manda CC) e vengono scartati: un
SysEx incompleto in mezzo non deve rompere il parsing.
"""

from __future__ import annotations

BLE_MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
BLE_MIDI_CHARACTERISTIC = "7772e5db-3868-4112-a1a9-f2669d106bf3"

SYSTEM_REALTIME = 0xF8
SYSEX_START = 0xF0
SYSEX_END = 0xF7


def expected_data_bytes(status: int) -> int:
    """Quanti byte di dati seguono questo status (0 per i messaggi senza dati)."""
    if status >= 0xF0:
        if status in (0xF1, 0xF3):  # MIDI Time Code Quarter Frame, Song Select
            return 1
        if status == 0xF2:  # Song Position Pointer
            return 2
        return 0
    kind = status & 0xF0
    if kind in (0xC0, 0xD0):  # Program Change, Channel Aftertouch
        return 1
    return 2


def decode_packet(data: bytes) -> list[bytes]:
    """Trasforma una notifica BLE-MIDI in messaggi MIDI completi.

    Ogni elemento della lista e' un messaggio (status + dati) gia' pronto per
    essere scritto sulla socket del motore.
    """
    out: list[bytes] = []
    # Un pacchetto valido e' almeno header + timestamp, e l'header ha bit 7 a 1.
    if len(data) < 2 or (data[0] & 0x80) == 0:
        return out

    expect_timestamp = True
    running_status = 0
    message = bytearray()

    for byte in data[1:]:
        if (byte & 0x80) == 0:
            # Dato: continua il messaggio in corso, o ne apre uno col running status.
            if running_status == 0:
                continue
            if not message:
                message = bytearray([running_status])
            message.append(byte)
            if len(message) == expected_data_bytes(running_status) + 1:
                out.append(bytes(message))
                message = bytearray()
            expect_timestamp = True
            continue

        if expect_timestamp:
            expect_timestamp = False
            continue
        expect_timestamp = True

        if byte >= SYSTEM_REALTIME:
            continue  # clock/active sensing: ignorati
        if byte in (SYSEX_START, SYSEX_END):
            message = bytearray()
            running_status = 0
            continue

        message = bytearray()  # messaggio incompleto: scartato
        running_status = byte
        expected = expected_data_bytes(byte)
        if expected == 0:
            out.append(bytes([byte]))
        else:
            message = bytearray([byte])

    return out


def describe_message(message: bytes) -> str:
    """Comodo per i log: "cc ch1 32 0" invece di "b0 20 00"."""
    if len(message) < 2:
        return " ".join(f"{b:02x}" for b in message)
    status = message[0]
    kind = status & 0xF0
    channel = (status & 0x0F) + 1
    if kind == 0xB0:
        return f"cc ch{channel} {message[1]} {message[2] if len(message) > 2 else 0}"
    if kind == 0x90:
        return f"noteon ch{channel} {message[1]} {message[2] if len(message) > 2 else 0}"
    if kind == 0x80:
        return f"noteoff ch{channel} {message[1]} {message[2] if len(message) > 2 else 0}"
    if kind == 0xC0:
        return f"programchange ch{channel} {message[1]}"
    return " ".join(f"{b:02x}" for b in message)
