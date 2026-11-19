import { createBluetooth } from "node-ble";

import { BLE_MIDI_CHARACTERISTIC, BLE_MIDI_SERVICE, decodePacket } from "./ble_midi";

const MAC = /^([0-9a-f]{2}:){5}[0-9a-f]{2}$/i;

export interface SessionOptions {
  /** Nome (parte del nome) oppure indirizzo MAC del dispositivo BLE. */
  device: string;
  onMidi: (bytes: number[]) => void;
  log: (message: string) => void;
}

interface FoundDevice {
  // node-ble non esporta i tipi in modo comodo: qui serve solo l'oggetto.
  device: any;
  address: string;
  name: string;
}

async function findDevice(adapter: any, wanted: string, log: (m: string) => void): Promise<FoundDevice> {
  if (MAC.test(wanted)) {
    log(`cerco per indirizzo ${wanted}`);
    return { device: await adapter.waitDevice(wanted.toUpperCase()), address: wanted, name: wanted };
  }

  const needle = wanted.toLowerCase();
  const deadline = Date.now() + 30000;
  while (Date.now() < deadline) {
    const addresses: string[] = await adapter.devices();
    for (const address of addresses) {
      try {
        const device = await adapter.getDevice(address);
        const name = (await device.getName()) ?? "";
        if (name.toLowerCase().includes(needle)) {
          log(`trovato "${name}" (${address})`);
          return { device, address, name };
        }
      } catch {
        // un device puo' sparire tra devices() e getDevice(): si ignora
      }
    }
    await sleep(1000);
  }
  throw new Error(`nessun dispositivo BLE con "${wanted}" nel nome (30 s di scansione)`);
}

function waitForDisconnect(device: any): Promise<void> {
  return new Promise((resolve) => {
    const timer = setInterval(async () => {
      try {
        const connected = await device.isConnected();
        if (connected === false) {
          clearInterval(timer);
          resolve();
        }
      } catch {
        clearInterval(timer);
        resolve();
      }
    }, 2000);

    // Alcune versioni emettono l'evento, altre no: si tengono entrambe le vie.
    device.on("disconnect", () => {
      clearInterval(timer);
      resolve();
    });
  });
}

/** Una sessione: si risolve quando il dispositivo si disconnette. */
export async function runSession(options: SessionOptions): Promise<void> {
  const { bluetooth, destroy } = createBluetooth();
  try {
    const adapter = await bluetooth.defaultAdapter();
    if (!(await adapter.isDiscovering())) await adapter.startDiscovery();

    const found = await findDevice(adapter, options.device, options.log);

    await found.device.connect();
    options.log(`connesso a ${found.name} (${found.address})`);

    const gatt = await found.device.gatt();
    const service = await gatt.getPrimaryService(BLE_MIDI_SERVICE);
    const characteristic = await service.getCharacteristic(BLE_MIDI_CHARACTERISTIC);

    characteristic.on("valuechanged", (buffer: Buffer) => {
      const bytes = decodePacket(new Uint8Array(buffer));
      if (bytes.length > 0) options.onMidi(bytes);
    });
    await characteristic.startNotifications();
    options.log("notifiche BLE MIDI attive");

    await waitForDisconnect(found.device);
    options.log("disconnesso");
  } finally {
    destroy();
  }
}

export function sleep(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}
