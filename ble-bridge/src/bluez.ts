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
  // node-ble crea un proxy D-Bus per ogni getDevice(): senza cache i listener
  // si accumulano a ogni giro e il nome viene riletto ogni secondo.
  const seen = new Map<string, { device: any; name: string }>();
  const deadline = Date.now() + 30000;

  while (Date.now() < deadline) {
    const addresses: string[] = await adapter.devices();
    for (const address of addresses) {
      if (!seen.has(address)) {
        try {
          const device = await adapter.getDevice(address);
          const name = ((await device.getName()) ?? "").trim();
          seen.set(address, { device, name });
        } catch {
          // un device puo' sparire tra devices() e getDevice(): si ignora
        }
      }
      const entry = seen.get(address);
      if (entry && entry.name.toLowerCase().includes(needle)) {
        log(`trovato "${entry.name}" (${address})`);
        return { device: entry.device, address, name: entry.name };
      }
    }
    await sleep(1000);
  }

  // Non trovato: elenca cosa si e' visto, serve a capire con che nome si
  // annuncia davvero il controller (spesso il nome non e' nell'advertisement).
  log(`visti ${seen.size} dispositivi, nessuno contiene "${wanted}":`);
  for (const [address, entry] of seen) {
    log(`  ${address}  ${entry.name || "(senza nome)"}`);
  }
  throw new Error(`nessun dispositivo BLE con "${wanted}" nel nome (30 s di scansione)`);
}

// Le operazioni BLE possono restare appese se il device si disconnette a meta':
// un timeout le trasforma in un errore e il ciclo esterno riprova.
function withTimeout<T>(promise: Promise<T>, ms: number, what: string): Promise<T> {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`${what}: nessuna risposta in ${ms} ms`)), ms);
    promise.then(
      (value) => {
        clearTimeout(timer);
        resolve(value);
      },
      (err) => {
        clearTimeout(timer);
        reject(err);
      },
    );
  });
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

    await withTimeout(found.device.connect(), 10000, "connect");
    options.log(`connesso a ${found.name} (${found.address})`);

    const gatt = await withTimeout(found.device.gatt(), 10000, "gatt");
    let service;
    try {
      service = await withTimeout(gatt.getPrimaryService(BLE_MIDI_SERVICE), 10000, "getPrimaryService");
    } catch {
      throw new Error(
        "il dispositivo non espone il servizio BLE MIDI (03B80E5A): e' in modalita' HID/tastiera, " +
          "va messo in modalita' MIDI dall'app M-Vave",
      );
    }
    let characteristic;
    try {
      characteristic = await withTimeout(
        service.getCharacteristic(BLE_MIDI_CHARACTERISTIC),
        10000,
        "getCharacteristic",
      );
    } catch {
      throw new Error("servizio BLE MIDI trovato ma senza la caratteristica 7772E5DB");
    }

    characteristic.on("valuechanged", (buffer: Buffer) => {
      const bytes = decodePacket(new Uint8Array(buffer));
      if (bytes.length > 0) options.onMidi(bytes);
    });
    await withTimeout(characteristic.startNotifications(), 10000, "startNotifications");
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
