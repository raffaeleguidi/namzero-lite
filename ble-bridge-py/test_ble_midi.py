"""Test del decoder BLE-MIDI. Non serve BlueZ: si decodificano solo i byte."""

from __future__ import annotations

import unittest

from ble_midi import decode_packet, describe_message


class DecodeTest(unittest.TestCase):
    def test_cc(self) -> None:
        # header, timestamp, poi B0 14 7F
        packet = bytes([0x80, 0x80, 0xB0, 0x14, 0x7F])
        self.assertEqual(decode_packet(packet), [bytes([0xB0, 0x14, 0x7F])])

    def test_due_messaggi_nello_stesso_pacchetto(self) -> None:
        packet = bytes([0x80, 0x80, 0xB0, 0x14, 0x7F, 0x90, 0xB0, 0x15, 0x00])
        self.assertEqual(
            decode_packet(packet),
            [bytes([0xB0, 0x14, 0x7F]), bytes([0xB0, 0x15, 0x00])],
        )

    def test_nota_on_e_off(self) -> None:
        self.assertEqual(
            decode_packet(bytes([0x80, 0x80, 0x90, 60, 100])), [bytes([0x90, 60, 100])]
        )
        self.assertEqual(
            decode_packet(bytes([0x80, 0x80, 0x80, 60, 0])), [bytes([0x80, 60, 0])]
        )

    def test_clock_realtime_ignorato(self) -> None:
        packet = bytes([0x80, 0x80, 0xF8, 0x90, 0xB0, 0x14, 0x7F])
        self.assertEqual(decode_packet(packet), [bytes([0xB0, 0x14, 0x7F])])

    def test_pacchetto_malformato(self) -> None:
        self.assertEqual(decode_packet(b""), [])
        self.assertEqual(decode_packet(bytes([0x90])), [])
        self.assertEqual(decode_packet(bytes([0x01, 0x02, 0x03])), [])

    def test_sysex_non_lascia_stato(self) -> None:
        packet = bytes([0x80, 0x80, 0xF0, 0x7E, 0x00, 0x90, 0xB0, 0x14, 0x7F])
        self.assertEqual(decode_packet(packet), [bytes([0xB0, 0x14, 0x7F])])

    def test_running_status(self) -> None:
        # Un solo status, due valori: il secondo e' running status.
        packet = bytes([0x80, 0x80, 0xB0, 0x14, 0x7F, 0x15, 0x00])
        self.assertEqual(
            decode_packet(packet),
            [bytes([0xB0, 0x14, 0x7F]), bytes([0xB0, 0x15, 0x00])],
        )

    def test_descrizione(self) -> None:
        self.assertEqual(describe_message(bytes([0xB0, 0x14, 0x7F])), "cc ch1 20 127")
        self.assertEqual(describe_message(bytes([0x90, 60, 100])), "noteon ch1 60 100")


if __name__ == "__main__":
    unittest.main()
