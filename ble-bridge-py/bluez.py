"""Connessione al pedale via BlueZ, direttamente su D-Bus (dbus-fast).

Perche' non bleak: bleak cerca il device nella lista della scansione. Il
Chocolate, quando e' connesso (anche solo per una connessione rimasta appesa) o
assopito, non si annuncia, quindi la scansione non lo trova mai. BlueZ invece lo
conosce per MAC e `Device1.Connect()` funziona anche senza advertising: e' la
strada che usiamo qui. In piu' non si fa scansione continua, che sul Pi disturba
WiFi e audio.

Il flusso di una sessione:
  1. trova l'adapter e il path del device dal MAC;
  2. se e' connesso lo stacca, poi connette (sessione pulita);
  3. aspetta che i servizi siano risolti;
  4. trova la caratteristica BLE MIDI e fa StartNotify;
  5. consegna ogni `Value` a on_packet finche' il device resta connesso.
"""

from __future__ import annotations

import asyncio
import re

from dbus_fast import BusType
from dbus_fast.aio import MessageBus

from ble_midi import BLE_MIDI_CHARACTERISTIC, BLE_MIDI_SERVICE

MAC = re.compile(r"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$")

BLUEZ = "org.bluez"


def _path_for(adapter: str, mac: str) -> str:
    return f"{adapter}/dev_" + mac.replace(":", "_").upper()


async def _managed_objects(bus: MessageBus) -> dict:
    intro = await bus.introspect(BLUEZ, "/")
    obj = bus.get_proxy_object(BLUEZ, "/", intro)
    manager = obj.get_interface("org.freedesktop.DBus.ObjectManager")
    return await manager.call_get_managed_objects()


async def _interface(bus: MessageBus, path: str, name: str):
    intro = await bus.introspect(BLUEZ, path)
    obj = bus.get_proxy_object(BLUEZ, path, intro)
    return obj.get_interface(name)


async def _find_adapter(bus: MessageBus) -> str:
    for path, interfaces in (await _managed_objects(bus)).items():
        if "org.bluez.Adapter1" in interfaces:
            return path
    raise RuntimeError("nessun adapter Bluetooth: il servizio bluetooth e' attivo?")


async def _resolve_address(bus: MessageBus, device: str, log) -> str:
    """MAC diretto, oppure nome risolto dalla cache di BlueZ (niente scansione).

    Il pedale accoppiato resta nella lista dei device di BlueZ anche quando non
    si annuncia: il nome basta a trovare l'indirizzo.
    """
    if MAC.match(device):
        return device.upper()

    needle = device.lower()
    for _path, interfaces in (await _managed_objects(bus)).items():
        dev = interfaces.get("org.bluez.Device1")
        if dev is None:
            continue
        names = []
        for key in ("Name", "Alias"):
            value = dev.get(key)
            if value is not None:
                names.append(str(value.value).lower())
        if any(needle in name for name in names):
            address = str(dev["Address"].value).upper()
            log(f'"{device}" -> {address}')
            return address

    raise RuntimeError(
        f'BlueZ non conosce "{device}": accoppia il pedale una volta con bluetoothctl'
    )


async def _connect_device(bus: MessageBus, path: str, log) -> None:
    device = await _interface(bus, path, "org.bluez.Device1")
    props = await _interface(bus, path, "org.freedesktop.DBus.Properties")

    if (await props.call_get("org.bluez.Device1", "Connected")).value:
        # Sessione rimasta appesa da un'esecuzione precedente: si riparte puliti.
        log("device gia' connesso: lo stacco per una sessione pulita")
        try:
            await device.call_disconnect()
        except Exception as exc:
            log(f"disconnect: {exc}")
        await asyncio.sleep(0.5)

    try:
        await device.call_connect()
    except Exception as exc:
        # Una connessione gia' in corso puo' dare AlreadyConnected: si prosegue.
        if "AlreadyConnected" not in str(exc) and "InProgress" not in str(exc):
            raise

    for _ in range(150):  # 15 s
        if (await props.call_get("org.bluez.Device1", "ServicesResolved")).value:
            return
        if not (await props.call_get("org.bluez.Device1", "Connected")).value:
            raise RuntimeError("il pedale si e' disconnesso durante la connessione")
        await asyncio.sleep(0.1)
    raise RuntimeError("servizi GATT non risolti entro 15 s")


async def _midi_characteristic(bus: MessageBus) -> str:
    objects = await _managed_objects(bus)

    service_path = None
    for path, interfaces in objects.items():
        service = interfaces.get("org.bluez.GattService1")
        if service is not None and str(service["UUID"].value).lower() == BLE_MIDI_SERVICE:
            service_path = path
            break
    if service_path is None:
        raise RuntimeError(
            "il dispositivo non espone il servizio BLE MIDI (03B80E5A): "
            "e' in modalita' HID/tastiera, va messo in modalita' MIDI "
            "dall'app M-Vave"
        )

    for path, interfaces in objects.items():
        characteristic = interfaces.get("org.bluez.GattCharacteristic1")
        if characteristic is None:
            continue
        if str(characteristic["Service"].value) != service_path:
            continue
        if str(characteristic["UUID"].value).lower() == BLE_MIDI_CHARACTERISTIC:
            return path

    raise RuntimeError("servizio BLE MIDI trovato ma senza la caratteristica 7772E5DB")


async def run_session(device: str, log, on_packet) -> None:
    """Una sessione completa. Ritorna quando il pedale si disconnette."""
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    path = None
    try:
        mac = await _resolve_address(bus, device, log)
        adapter = await _find_adapter(bus)
        path = _path_for(adapter, mac)
        if path not in await _managed_objects(bus):
            raise RuntimeError(
                f"BlueZ non conosce {mac}: accoppia il pedale una volta con bluetoothctl"
            )

        log(f"connessione a {mac}")
        await _connect_device(bus, path, log)
        log(f"connesso a {mac}")

        char_path = await _midi_characteristic(bus)
        characteristic = await _interface(bus, char_path, "org.bluez.GattCharacteristic1")
        char_props = await _interface(bus, char_path, "org.freedesktop.DBus.Properties")

        def on_properties_changed(_interface_name: str, changed: dict, _invalidated: list) -> None:
            value = changed.get("Value")
            if value is not None:
                on_packet(bytes(value.value))

        char_props.on_properties_changed(on_properties_changed)
        await characteristic.call_start_notify()
        log("notifiche BLE MIDI attive")

        device_props = await _interface(bus, path, "org.freedesktop.DBus.Properties")
        while (await device_props.call_get("org.bluez.Device1", "Connected")).value:
            await asyncio.sleep(1.0)
        log("disconnesso")
    finally:
        # Su uscita ordinata si stacca: una connessione senza client lascerebbe il
        # pedale non annunciabile al prossimo avvio.
        if path is not None:
            try:
                props = await _interface(bus, path, "org.freedesktop.DBus.Properties")
                if (await props.call_get("org.bluez.Device1", "Connected")).value:
                    device = await _interface(bus, path, "org.bluez.Device1")
                    await device.call_disconnect()
            except Exception:
                pass
        bus.disconnect()
