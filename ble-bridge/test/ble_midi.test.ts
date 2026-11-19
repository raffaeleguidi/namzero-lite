import { describe, expect, test } from "bun:test";

import { decodePacket, describeMessage } from "../src/ble_midi";

// I pacchetti sono costruiti secondo la regola del BLE-MIDI: header, timestamp,
// poi le MIDI. Prima di ogni status c'e' un timestamp.

test("un CC: header, timestamp, B0 14 7F", () => {
  const packet = new Uint8Array([0x80, 0x80, 0xb0, 0x14, 0x7f]);
  expect(decodePacket(packet)).toEqual([0xb0, 0x14, 0x7f]);
});

test("due messaggi nello stesso pacchetto", () => {
  const packet = new Uint8Array([
    0x80, 0x80, 0xb0, 0x14, 0x7f, // CC 20 = 127
    0x90, 0xb0, 0x15, 0x00, // timestamp 0x90, poi CC 21 = 0
  ]);
  expect(decodePacket(packet)).toEqual([0xb0, 0x14, 0x7f, 0xb0, 0x15, 0x00]);
});

test("nota on e off", () => {
  expect(decodePacket(new Uint8Array([0x80, 0x80, 0x90, 60, 100]))).toEqual([0x90, 60, 100]);
  expect(decodePacket(new Uint8Array([0x80, 0x80, 0x80, 60, 0]))).toEqual([0x80, 60, 0]);
});

test("il clock realtime viene ignorato", () => {
  const packet = new Uint8Array([0x80, 0x80, 0xf8, 0x90, 0xb0, 0x14, 0x7f]);
  expect(decodePacket(packet)).toEqual([0xb0, 0x14, 0x7f]);
});

test("un pacchetto malformato non produce nulla", () => {
  expect(decodePacket(new Uint8Array([]))).toEqual([]);
  expect(decodePacket(new Uint8Array([0x90]))).toEqual([]);
  expect(decodePacket(new Uint8Array([0x01, 0x02, 0x03]))).toEqual([]);
});

test("il SysEx non lascia stato appeso", () => {
  const packet = new Uint8Array([0x80, 0x80, 0xf0, 0x7e, 0x00, 0x90, 0xb0, 0x14, 0x7f]);
  expect(decodePacket(packet)).toEqual([0xb0, 0x14, 0x7f]);
});

test("descrizione leggibile", () => {
  expect(describeMessage([0xb0, 0x14, 0x7f])).toBe("cc ch1 20 127");
  expect(describeMessage([0x90, 60, 100])).toBe("noteon ch1 60 100");
});
