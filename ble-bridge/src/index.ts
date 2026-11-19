import { runSession, sleep } from "./bluez";
import { describeMessage } from "./ble_midi";
import { MidiSocket } from "./socket";

// Configurazione da ambiente: niente file da parsare per un processo cosi' piccolo.
const device = process.env.NAMZERO_BLE_DEVICE ?? "Chocolate";
const socketPath = process.env.NAMZERO_BLE_SOCKET ?? "/tmp/namzero-lite-ble.sock";
const debug = process.env.NAMZERO_BLE_DEBUG === "1";

function log(message: string): void {
  const now = new Date().toISOString().slice(11, 23);
  console.log(`${now} ${message}`);
}

const socket = new MidiSocket(socketPath, debug ? log : () => {});
log(`bridge BLE MIDI: cerco "${device}", scrivo su ${socketPath}`);

for (;;) {
  try {
    await runSession({
      device,
      log,
      onMidi: (bytes) => {
        if (debug) log(`midi: ${describeMessage(bytes)}  [${bytes.map((b) => b.toString(16).padStart(2, "0")).join(" ")}]`);
        socket.send(bytes);
      },
    });
  } catch (err) {
    log(`errore: ${(err as Error).message}`);
  }
  await sleep(2000);
}
