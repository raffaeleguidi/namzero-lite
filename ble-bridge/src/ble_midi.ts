// Decodifica dei pacchetti BLE-MIDI.
//
// Formato (Apple BLE-MIDI): ogni pacchetto inizia con un header (bit 7 a 1),
// poi un timestamp, poi le MIDI. La regola che conta e' quella del parser
// ESPHome: un byte con bit 7 a 1 e' uno status *solo* se arriva subito dopo un
// timestamp; altrimenti e' un timestamp. Per questo non basta "togliere i byte
// con bit 7 alto": si perderebbe il primo status della nota.
//
// I messaggi SysEx non servono al bridge (il Chocolate manda CC e note) e
// vengono scartati: un SysEx incompleto in mezzo non deve rompere il parsing.

export const BLE_MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700";
export const BLE_MIDI_CHARACTERISTIC = "7772e5db-3868-4112-a1a9-f2669d106bf3";

const SYSTEM_REALTIME = 0xf8;
const SYSEX_START = 0xf0;
const SYSEX_END = 0xf7;

export function expectedDataBytes(status: number): number {
  if (status >= 0xf0) {
    switch (status) {
      case 0xf1: // MIDI Time Code Quarter Frame
      case 0xf3: // Song Select
        return 1;
      case 0xf2: // Song Position Pointer
        return 2;
      default:
        return 0;
    }
  }
  switch (status & 0xf0) {
    case 0xc0: // Program Change
    case 0xd0: // Channel Aftertouch
      return 1;
    default:
      return 2;
  }
}

/** Trasforma una notifica BLE-MIDI in messaggi MIDI completi (status + dati). */
export function decodePacket(data: Uint8Array): number[] {
  const out: number[] = [];
  // Un pacchetto valido e' almeno header + timestamp, e l'header ha bit 7 a 1.
  if (data.length < 2 || (data[0] & 0x80) === 0) return out;

  let expectTimestamp = true;
  let runningStatus = 0;
  let message: number[] = [];
  let expected = 0;

  const emit = (): void => {
    if (message.length > 0) out.push(...message);
    message = [];
  };

  for (let i = 1; i < data.length; i++) {
    const byte = data[i];

    if ((byte & 0x80) === 0) {
      // Dato: continua il messaggio in corso, o ne apre uno col running status.
      if (runningStatus === 0) continue;
      if (message.length === 0) {
        message = [runningStatus];
        expected = expectedDataBytes(runningStatus);
      }
      message.push(byte);
      if (message.length === expected + 1) emit();
      expectTimestamp = true;
      continue;
    }

    if (expectTimestamp) {
      expectTimestamp = false;
      continue;
    }
    expectTimestamp = true;

    if (byte >= SYSTEM_REALTIME) continue; // clock/active sensing: ignorati
    if (byte === SYSEX_START || byte === SYSEX_END) {
      // SysEx: fuori scope, e comunque non deve lasciare stato appeso.
      message = [];
      runningStatus = 0;
      continue;
    }

    if (message.length > 0) message = []; // messaggio incompleto: scartato
    runningStatus = byte;
    expected = expectedDataBytes(byte);
    message = [byte];
    if (expected === 0) emit();
  }

  return out;
}

/** Comodo per i log: "cc 20 127" invece di "b0 14 7f". */
export function describeMessage(bytes: number[]): string {
  if (bytes.length < 2) return bytes.map((b) => b.toString(16)).join(" ");
  const status = bytes[0];
  const kind = status & 0xf0;
  const channel = (status & 0x0f) + 1;
  if (kind === 0xb0) return `cc ch${channel} ${bytes[1]} ${bytes[2] ?? 0}`;
  if (kind === 0x90) return `noteon ch${channel} ${bytes[1]} ${bytes[2] ?? 0}`;
  if (kind === 0x80) return `noteoff ch${channel} ${bytes[1]} ${bytes[2] ?? 0}`;
  if (kind === 0xc0) return `programchange ch${channel} ${bytes[1]}`;
  return bytes.map((b) => b.toString(16).padStart(2, "0")).join(" ");
}
