# Installer Debian (fase 1 MVP)

Un pacchetto, due modalita': `namzero-lite_<versione>_arm64.deb` per Raspberry
Pi OS Lite 64-bit e `namzero-lite_<versione>_amd64.deb` per un desktop Linux
(Ubuntu 24.04). Il binario e' lo stesso codice: cambia solo il modo in cui
viene avviato e dove vivono i dati. Sostituisce, per le installazioni "da
distribuire", il flusso `scripts/install.sh` + `scripts/deploy.sh` (che
restano validi per lo sviluppo sul checkout git). Il piano generale e' in
[INSTALLER_PLAN.md](../INSTALLER_PLAN.md).

## Installazione

```bash
sudo apt install ./namzero-lite_0.1.0_arm64.deb   # Raspberry Pi
sudo apt install ./namzero-lite_0.1.0_amd64.deb   # laptop Ubuntu
```

In modalita' servizio l'installazione parte da sola: crea l'utente `namzero`,
il venv del bridge, applica le capability, abilita e avvia le unit. La web UI
risponde su `http://<ip-del-pi>:8080`. A differenza di `scripts/install.sh`
(che usa `/home/pi/namzero-lite` e l'utente `pi`), il pacchetto usa l'utente di
sistema `namzero`: `pi` resta solo l'utente autorizzato a `namzero-ctl`.

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

## Modalita': desktop o service

La scelta sta in debconf (`namzero-lite/install-mode`) e governa maintainer
script e unit. Il default dipende dall'architettura **calcolata nel config
script**, non nel template: su arm64 e' `service` (il Pi resta un apparecchio e
nessuno deve rispondere a domande via SSH), altrove `desktop`.

| Modalita' | Avvio | Config | Dati |
|---|---|---|---|
| `desktop` | menu applicazioni, GUI GTK+WebKit | `~/.config/namzero` | `~/.local/share/namzero` |
| `service` | systemd al boot | `/etc/namzero` (conffiles) | `/var/lib/namzero` |

Le due modalita' sono alternative: non c'e' un "entrambi". Il launcher viene
installato comunque, anche in modalita' servizio: su una macchina che gira come
servizio si puo' aprire `namzero-lite-gui --no-engine` e la finestra si collega
all'istanza gia' attiva, senza avviare un secondo motore.

Una versione precedente del pacchetto offriva anche `both`. Chi l'aveva scelta
viene migrato a `service` (i dati erano gia' in `/etc/namzero`), con un
messaggio nel log del postinst.

Cambio successivo:

```bash
sudo dpkg-reconfigure namzero-lite
```

Installazione non presidiata (CI, immagini, script):

```bash
sudo NAMZERO_INSTALL_MODE=service apt install ./namzero-lite_0.1.0_arm64.deb
```

`NAMZERO_INSTALL_MODE` ha la precedenza su debconf e viene letta dal `config`
script. Va impostata **dentro** una shell di root: `sudo VAR=x comando` non
propaga `VAR`, perche' sudo fa `env_reset`. Per questo `scripts/install-deb.sh`
usa `sudo sh -c "NAMZERO_INSTALL_MODE=... apt-get install ..."`.

### Perche' non basta `install-mode` nel template

Il doppio click su un `.deb` in Files lo passa a gnome-software/App Center con
`DEBIAN_FRONTEND=noninteractive`: la domanda non viene posta e vale il default.
Il `config` script calcola quindi un default sensato prima di chiedere, cosi'
un'installazione non presidiata sul Pi resta `service` e sul laptop diventa
`desktop`. Da terminale (`apt install ./file.deb`) la domanda viene posta.

## Modalita' desktop

Il launcher `/usr/bin/namzero-lite-gui` (Python 3 + GTK3 + WebKit2GTK,
nessuna dipendenza pip) prepara al primo avvio un layout per-utente e avvia
motore e web server come processi figli:

```
~/.config/namzero/           config, copiata una volta da /etc/namzero
~/.local/share/namzero/      root dati = cwd del motore
    config -> ~/.config/namzero
    models/  irs/            (i .nam di fabbrica sono copiati al primo avvio)
~/.local/state/namzero/      namzero-lite-gui.log
```

Il motore apre `config/...`, `models/...` e `irs/...` con percorsi **relativi
alla propria working directory**, quindi la cwd e' la root dati; alla web UI si
passa `NAMZERO_ROOT=<root dati>` (in `web/server.py`, `APP` resta la directory
del pacchetto per gli asset statici). Nessun file di sistema viene scritto:
non servono il gruppo `namzero`, sudo o capability.

Dettagli utili:

- se la porta 8080 e' gia' in ascolto (servizio attivo), la GUI si collega a
  quella e non avvia un secondo web server;
- se il lock `/tmp/namzero-lite.lock` e' gia' preso, la GUI avvisa e non avvia
  un secondo motore;
- alla chiusura i figli ricevono SIGTERM e, dopo 3 s, SIGKILL: nessun orfano e
  il lock torna libero;
- `--no-engine` collega solo la UI (motore gia' attivo come servizio),
  `--debug` scrive su stderr.

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
| `/usr/bin/namzero-lite-gui` | launcher desktop (GTK3 + WebKit2GTK) |
| `/usr/share/applications/namzero-lite.desktop` | voce di menu applicazioni |
| `/usr/share/icons/hicolor/scalable/apps/namzero-lite.svg` | icona |
| `/etc/sudoers.d/namzero` | regola NOPASSWD per `namzero-ctl` (creata dal postinst) |
| `/etc/NetworkManager/dnsmasq-shared.d/namzero-lite.conf` | drop-in DNS per il nome dell'AP (creato dal postinst) |

`/etc/namzero` è di proprietà `root:namzero` con directory `0770` e file
`0660` **in modalità servizio**: il motore e la web UI girano come utente di
servizio `namzero` e devono poter salvare opzioni, preset e WiFi, ma la config
non è leggibile dagli altri utenti. Lo stesso utente è nei gruppi `audio` e
`bluetooth`. La directory `/opt/namzero-lite/models` è scrivibile dal gruppo
per gli upload dalla UI.

In modalità **desktop** `/etc/namzero` è invece `root:root` `0755/0644`:
esprime solo la configurazione di fabbrica, che il launcher copia nella home
dell'utente al primo avvio. Nessun utente di servizio viene creato, nessuna
regola sudoers, nessun drop-in DNS.

### Modelli e IR

Il pacchetto include i modelli NAM presenti nel checkout al momento della
build (`models/`, ~1.1 MB) e i preset di default (11 file). Le IR **non** sono
ridistribuite: `/opt/namzero-lite/irs` punta a `/var/lib/namzero/irs`, quindi
gli upload finiscono nei dati utente. I preset che citano un file IR assente
avvisano a video/journal e il motore continua senza di esso.

Nessun segreto reale è nel pacchetto: `wifi.json` e `tone3000.json` contengono
gli esempi del repo (`Casa-Esempio`, client id `...ESEMPIO...`).

## Build

Tutto in container: non serve `dpkg` né ALSA sull'host, e l'architettura del
pacchetto sceglie l'immagine (Ubuntu 24.04 per amd64, Debian bookworm per
arm64). Il binario viene ricompilato con `cmake` e la suite `ctest` gira prima
di impacchettare: un test rosso ferma la build.

```bash
./scripts/build-deb.sh                    # amd64 (default, host x86_64)
ARCH=arm64 ./scripts/build-deb.sh         # arm64 per il Pi
./scripts/build-deb.sh 0.2.0              # versione esplicita
FORCE_BUILD=1 ./scripts/build-deb.sh      # ricompila anche se il binario c'e'
SKIP_BUILD=1 ./scripts/build-deb.sh       # ripacchetta senza ricompilare
NAM_DIR=~/nam ./scripts/build-deb.sh      # abilita lo slot amp (core NAM)
```

Su arm64, se `build-arm64/namzero-lite` manca, lo script chiama
`scripts/cross-build.sh` come prima. Su Mac x86_64 il container arm64 gira via
qemu (lento); amd64 è nativo.

Lo script:

1. compila (o riusa) `build-<arch>/namzero-lite` ed esegue `ctest`;
2. crea la staging in `build-deb/root` (layout della tabella sopra);
3. genera `DEBIAN/control`, `conffiles`, `templates` e i maintainer script;
4. chiama `dpkg-deb --root-owner-group --build` nell'immagine della
   distribuzione di destinazione;
5. scrive `dist/namzero-lite_<ver>_<arch>.deb` e ne stampa control, conffiles,
   templates e layout.

### Dipendenze dichiarate

`Depends` contiene solo ciò che serve in entrambe le modalità. `libasound2`
è dichiarato come `libasound2 (>= 1.2.0) | libasound2t64` perché Ubuntu 24.04
ha rinominato il pacchetto (transizione t64) e senza l'alternativa il `.deb`
non sarebbe installabile sul laptop. GTK/WebKit (desktop) e
bluez/NetworkManager/dnsmasq (servizio) stanno nei `Recommends`: apt li
installa di default, ma nessuna delle due modalità trascina le dipendenze
dell'altra.

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
