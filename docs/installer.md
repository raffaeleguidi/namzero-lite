# Installer Debian (fase 1 MVP)

Pacchetto unico `namzero-lite_<versione>_arm64.deb` per Raspberry Pi OS Lite
64-bit. Sostituisce, per le installazioni "da distribuire", il flusso
`scripts/install.sh` + `scripts/deploy.sh` (che restano validi per lo sviluppo
sul checkout git). Il piano generale è in [INSTALLER_PLAN.md](../INSTALLER_PLAN.md).

## Installazione

```bash
sudo apt install ./namzero-lite_0.1.0_arm64.deb
```

L'installazione parte da sola: crea l'utente di servizio `namzero`, il venv del
bridge, applica le capability, abilita e avvia le unit. La web UI risponde su
`http://<ip-del-pi>:8080`. A differenza di `scripts/install.sh` (che usa
`/home/pi/namzero-lite` e l'utente `pi`), il pacchetto usa l'utente di sistema
`namzero`: `pi` resta solo l'utente autorizzato a `namzero-ctl`.

```bash
systemctl status namzero-lite namzero-web namzero-ble-bridge namzero-wifi
curl -s http://127.0.0.1:8080/api/state | jq '.audio'
sudo namzero-ctl is-active lite
```

Aggiornamento del solo pacchetto (con configurazione preservata):

```bash
sudo apt install ./namzero-lite_0.2.0_arm64.deb
```

Rimozione / purge:

```bash
sudo apt remove namzero-lite    # conserva /etc/namzero e /var/lib/namzero
sudo apt purge namzero-lite     # rimuove anche config, dati e venv
```

## Layout installato

| Percorso | Contenuto |
|---|---|
| `/opt/namzero-lite/` | binario `namzero-lite`, `web/`, `scripts/`, `ble-bridge-py/`, `models/`, `docs/`, `venv/` (creato dal postinst) |
| `/opt/namzero-lite/config` | **symlink** a `/etc/namzero` (il motore usa path relativi `config/...`) |
| `/opt/namzero-lite/irs` | **symlink** a `/var/lib/namzero/irs` (le IR non sono nel pacchetto) |
| `/etc/namzero/` | config viva: `presets.json`, `midi.json`, `presets/*.json`, `wifi.json`, `tone3000.json` — tutti **conffiles** |
| `/etc/default/namzero` | conffile con `NAMZERO_CTL_USER` (default `pi`) |
| `/var/lib/namzero/` | dati utente: `irs/`, `models/`, `backups/`, `factory-defaults/` (creata dal postinst) |
| `/lib/systemd/system/` | `namzero-lite`, `namzero-web`, `namzero-ble-bridge`, `namzero-wifi`, `cpu-performance` |
| `/usr/local/sbin/namzero-ctl` | wrapper `start/stop/restart/is-active` per `lite|web|ble-bridge|wifi` |
| `/etc/sudoers.d/namzero` | regola NOPASSWD per `namzero-ctl` (creata dal postinst) |
| `/etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf` | drop-in DNS per il nome dell'AP (creato dal postinst) |

`/etc/namzero` è di proprietà `root:namzero` con directory `0770` e file
`0660`: il motore e la web UI girano come utente di servizio `namzero` e devono
poter salvare opzioni, preset e WiFi, ma la config non è leggibile dagli altri
utenti. Lo stesso utente è nei gruppi `audio` e `bluetooth`. La directory
`/opt/namzero-lite/models` è scrivibile dal gruppo per gli upload dalla UI.

### Modelli e IR

Il pacchetto include i modelli NAM presenti nel checkout al momento della
build (`models/`, ~1.1 MB) e i preset di default (11 file). Le IR **non** sono
ridistribuite: `/opt/namzero-lite/irs` punta a `/var/lib/namzero/irs`, quindi
gli upload finiscono nei dati utente. I preset che citano un file IR assente
avvisano a video/journal e il motore continua senza di esso.

Nessun segreto reale è nel pacchetto: `wifi.json` e `tone3000.json` contengono
gli esempi del repo (`Casa-Esempio`, client id `...ESEMPIO...`).

## Build

Prerequisiti: Docker e binario arm64 in `build-arm64/namzero-lite` (se manca,
lo script chiama `scripts/cross-build.sh`), che a sua volta richiede i sorgenti
NAM in `.nam/` o `NAM_DIR`.

```bash
./scripts/build-deb.sh              # versione da VERSION
./scripts/build-deb.sh 0.2.0        # versione esplicita
VERSION=0.2.0+rc1 ./scripts/build-deb.sh
```

Lo script:

1. crea la staging in `build-deb/root` (layout della tabella sopra);
2. genera `DEBIAN/control`, `conffiles` e i maintainer script;
3. chiama `dpkg-deb --root-owner-group --build` dentro
   `debian:bookworm` **arm64** (`--platform linux/arm64`);
4. scrive `dist/namzero-lite_<ver>_arm64.deb` e ne stampa control, conffiles e
   layout.

Su Mac x86_64 il container arm64 gira via qemu (Docker Desktop); su Apple
Silicon o su un host arm64 è nativo. La build non richiede `dpkg` sull'host.

## Test in container arm64

```bash
./scripts/test-container.sh
SKIP_BUILD=1 ./scripts/test-container.sh   # riusa i .deb in dist/
KEEP_CONTAINER=1 ./scripts/test-container.sh
```

Lo script costruisce due pacchetti (versione base e `<base>+test1`), l'immagine
`docker/Dockerfile.test` (Debian bookworm arm64 + systemd) e avvia il container
con i flag richiesti da systemd:

```bash
--privileged --cgroupns=host \
-v /sys/fs/cgroup:/sys/fs/cgroup:rw --tmpfs /run --tmpfs /run/lock
```

I check (in `scripts/test-deb-inside.sh`) verificano:

- `apt install ./base.deb` e le dipendenze dichiarate;
- layout, symlink `config`/`irs`, venv, modelli, preset, sudoers, drop-in DNS;
- `systemctl is-active` di `namzero-lite`, `namzero-web`,
  `namzero-ble-bridge`; `namzero-wifi` può essere `inactive` senza hardware
  WiFi (in container non c'è `wlan0`);
- `curl /` → 200 e `curl /api/state` → JSON valido con
  `audio.open == false` e `audio.error` valorizzato (niente `/dev/snd`);
- `sudo -n namzero-ctl is-active lite` come utente `pi`;
- modifica di `presets.json` e di un preset, upgrade a `+test1` con
  `--force-confold` e verifica che i marker siano preservati;
- `apt remove`: config e `/var/lib/namzero` restano;
- `apt purge`: spariscono config, dati, sudoers, drop-in, unit, `/opt` e
  `namzero-ctl`.

### Esito atteso e limiti

In container l'audio ALSA non esiste: il motore resta attivo in modalità
offline, esattamente come su un Pi senza XTONE collegato. Non sono testabili:

- apertura reale di `/dev/snd` e latenza/xrun;
- USB audio (XTONE) e MIDI hardware (XTONE/G-Board);
- Bluetooth/BLE (serve un adapter e il pedale);
- WiFi/AP (serve `wlan0` e NetworkManager con hardware);
- governor `performance` e `setcap` (in container mancano `cpufreq` e i
  privilegi reali; il postinst non fallisce se non sono disponibili).

Il test verifica quindi packaging, permessi, config, servizi, API e ciclo
install/upgrade/remove/purge, non la qualità audio.

### Esito dell'ultima esecuzione

Ambiente: Docker su host x86_64, container `debian:bookworm` **arm64** (qemu),
systemd come PID 1. `./scripts/build-deb.sh` produce
`dist/namzero-lite_0.1.0_arm64.deb` (~712 kB) e la variante `0.1.0+test1` per
l'upgrade. `scripts/test-container.sh` (con `SKIP_BUILD=1`, stessi .deb) ha
riportato **tutti i check superati**:

```
OK   apt install ./base.deb
OK   symlink config -> /etc/namzero
OK   venv del bridge
OK   preset di default (>= 8)
OK   modello NAM di fabbrica
OK   conffiles registrati
OK   namzero-lite attivo
OK   namzero-web attivo
OK   namzero-ble-bridge attivo
OK   cpu-performance attivo
OK   namzero-wifi: active (hardware assente, accettato)
OK   GET / -> 200 (200)
OK   GET /api/state JSON valido
OK   audio.open == false senza /dev/snd
OK   sudo -n namzero-ctl is-active lite -> active
OK   versione aggiornata: 0.1.0 -> 0.1.0+test1
OK   presets.json conservato dopo upgrade
OK   preset slot conservato dopo upgrade
OK   /var/lib/namzero resta su remove
OK   /etc/namzero resta su remove
OK   /var/lib/namzero rimosso   (purge)
OK   sudoers rimosso           (purge)
OK   /opt/namzero-lite rimosso (purge)
RISULTATO: tutti i check superati
```

Avvisi attesi, non fallimenti: `systemd` risulta `degraded` nel container
(unit di sistema senza hardware, es. `NetworkManager-wait-online`), e `dpkg`
segnala che `/usr/local` non è vuota durante il purge. `namzero-wifi` risulta
attivo ma senza `wlan0`: non fa danni e non entra in restart loop.

## Maintainer script

- **preinst**: su upgrade ferma motore, web e bridge; **non** ferma il WiFi
  (non si perde la sessione SSH).
- **postinst**: crea utente/directory, sistema i permessi di `/etc/namzero`,
  salva la copia factory in `/var/lib/namzero/factory-defaults`, crea il venv
  e installa `dbus-fast` (prima le wheel in `/opt/namzero-lite/wheels`, poi
  PyPI), applica `setcap`, scrive sudoers e drop-in DNS, `daemon-reload`,
  abilita e riavvia le unit. È idempotente e non fallisce se systemd o la rete
  non sono disponibili.
- **prerm**: stop dei servizi (il WiFi solo alla rimozione).
- **postrm**: su `purge` rimuove sudoers, drop-in, `/var/lib/namzero`,
  `/etc/namzero`, venv e upload; su `remove` conserva config e dati.

## Limiti noti / fase 2

- Non c'è ancora un repository APT (`dists/`): l'upgrade è `apt install
  ./nuovo.deb`, non `apt upgrade`.
- Le wheel di `dbus-fast` non sono vendute nel `.deb`: senza rete il bridge
  resta non configurato (il resto funziona). Aggiungere `packaging/wheels/*.whl`
  abilita il ramo offline già implementato.
- I preset personali e i modelli caricati dalla UI vivono ancora sotto
  `/etc/namzero` e `/opt/namzero-lite/models`; la separazione completa in
  `/var/lib/namzero` richiede di rendere configurabili i path del motore.
- `NAMZERO_CTL_USER` è fisso a `pi` come default; se il Pi usa un altro utente
  va cambiato in `/etc/default/namzero` (conffile) e il pacchetto va
  reinstallato/reconfigured per aggiornare sudoers.
- Firma del pacchetto, `lintian`, `debhelper`/`dpkg-buildpackage`, rollback e
  publishing su GitHub Releases sono fuori dall'MVP.
- Test su Raspberry Pi reale (audio, USB, BT, WiFi, reboot) ancora da fare.
