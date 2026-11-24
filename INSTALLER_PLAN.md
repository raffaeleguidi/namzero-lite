# Piano installer automatico (Raspbian 64-bit Lite)

**Stato: STANDBY** (in attesa di una SD nuova per i test). Decisione: **pacchetto
`.deb`** unico, con upgrade che preserva la configurazione.

Obiettivo: installazione automatica su una Raspbian 64-bit Lite pulita con un
solo comando:

```bash
sudo apt install ./namzero-lite_<versione>_arm64.deb
```

e aggiornamento del solo binario tramite `apt install ./nuovo.deb`, senza
password di root per l'utente finale (vedi `namzero-ctl`).

## Fattibilità

Alta, stima ~1–2 giorni. La logica di setup esiste già in `scripts/install.sh`
e va spostata nei maintainer script del pacchetto. Il grosso del lavoro è il
packaging (build arm64 + postinst), non il codice.

## Contenuto del pacchetto `namzero-lite_<ver>_arm64.deb`

| Percorso | Cosa |
|---|---|
| `/usr/lib/namzero-lite/` | binario, `web/`, `scripts/`, `ble-bridge-py/` |
| `/etc/namzero/` | config di default (`midi.json`, `presets.json`, `presets/`) → **conffiles** |
| `/var/lib/namzero/` | dati utente (models/IR upload, preset personali); creato dal postinst |
| `/lib/systemd/system/` | `namzero-lite`, `namzero-web`, `namzero-ble-bridge`, `namzero-wifi`, `cpu-performance` |
| `/usr/local/sbin/namzero-ctl` | wrapper sudo senza root |
| `/etc/sudoers.d/namzero` | regola NOPASSWD (dal postinst) |
| `/etc/NetworkManager/dnsmasq-shared.d/` | drop-in DNS AP (dal postinst) |

Dipendenze `Depends:`: `libasound2`, `python3`, `python3-venv`, `bluez`,
`network-manager`.

## Maintainer scripts

- **postinst** (cuore, ricicla `install.sh`): crea `/var/lib/namzero` (se
  assente copia la config factory), venv del bridge + `pip install -r`
  (o wheels vendute nel pacchetto per l'offline), `setcap`, regola sudoers,
  drop-in dnsmasq, `systemctl daemon-reload` + `enable --now`, governor.
- **preinst** (upgrade): stop preventivo dei servizi.
- **prerm**: `systemctl stop` dei servizi.
- **postrm** (purge): rimuove sudoers/drop-in; `/var/lib/namzero` solo con
  `purge` esplicito.

## Build

- `debian/` con debhelper (`dh_installsystemd`, `dh_install`), build **arm64**
  in Docker (stesso pattern di `scripts/cross-build.sh`:
  `dpkg-buildpackage -a arm64`), oppure `fpm`/`dpkg-deb -b` per l'MVP.
- Pubblicazione: GitHub Releases; install con `apt install ./file.deb`.

## Upgrade

`apt install ./nuovo.deb` → binario sostituito, **conffiles**
(`/etc/namzero`) preservati (prompt se modificati), servizio riavviato dal
postinst. È il caso "rilascio il solo binario senza password di root": il
pacchetto fa tutto.

## Rischi / decisioni

1. **Modelli NAM e preset**: il pacchetto mette solo i default (licenze
   TONE3000: non ridistribuire i modelli dell'utente). I preset personali in
   `/var/lib/namzero`.
2. **venv/pip offline**: con rete funziona; per installazioni senza rete
   conviene vendere le wheel nel `.deb`.
3. **Utente `pi`**: il pacchetto assume l'utente `pi` di Raspberry Pi OS (o lo
   parametrizza con una variabile in `/etc/default/namzero`).
4. **Audio/XTONE**: nessun setup (il motore va in offline se manca, come ora).

## Piano in fasi

1. **MVP**: `debian/` (control + postinst che riscrive l'attuale `install.sh`),
   build `.deb` arm64, test su Raspbian Lite **pulita**: install → audio/UI/
   preset → reboot.
2. **Upgrade**: install v0.1, modifica `/etc/namzero`, install v0.2 → verifica
   conffiles + riavvio.
3. **Publish**: GitHub Releases + (opzionale) repo apt `dists/` per
   `apt upgrade`.
4. **Purge/rollback**: test `apt remove`/`purge` e rollback del binario
   precedente.

## Riferimenti utili (già nel repo)

- `scripts/install.sh`: setup idempotente (capability, governor, unit, wifi,
  `namzero-ctl` + sudoers) da cui partire per il postinst.
- `scripts/namzero-ctl` + `/etc/sudoers.d/namzero`: controllo servizi senza
  root, pensato per card distribuite a terzi.
- `scripts/cross-build.sh` + `scripts/deploy.sh`: build arm64 e deploy attuale.
- `systemd/*.service`: unit da installare.
