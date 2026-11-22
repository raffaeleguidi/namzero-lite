# Tuning realtime del Raspberry Pi Zero 2W

Obiettivo: nessun xrun a 64 frame (1,33 ms) con la catena completa.

## 1. Sistema di base

- Raspberry Pi OS **Lite 64-bit** (aarch64). Il Zero 2W lo supporta ed è più
  veloce del 32-bit sui calcoli in virgola mobile.
- Scrivi l'immagine con Raspberry Pi Imager, imposta hostname, utente e SSH
  già in fase di scrittura.
- Aggiorna e installa il necessario con `scripts/setup_pi.sh`.

## 2. Kernel realtime

Due strade, in ordine di preferenza.

### A. Kernel PREEMPT_RT dai repo

```bash
uname -a                     # deve comparire "PREEMPT_RT" o "rt"
```


Se il kernel RT è disponibile nei repo della tua release:

```bash
sudo apt-get install -y linux-image-rt-arm64
sudo reboot
```

### B. Fallback senza kernel RT

Con il kernel standard si ottiene molto togliendo il Pi dalla gestione
"desktop" e isolando il core audio.

In `/boot/firmware/cmdline.txt` (una sola riga, aggiungi in coda):

```
isolcpus=3 nohz_full=3 rcu_nocbs=3 threadirqs
```

Note:
- `isolcpus=3` riserva il core 3 ai thread che ci si affinità esplicitamente.
  Deve combaciare con `CPUAffinity=3` nella unit systemd.
- `nohz_full=3` e `rcu_nocbs=3` riducono le interruzioni sul core isolato.
- `threadirqs` sposta la gestione degli interrupt su thread kernel, che possono
  essere spostati via da core 3.

Riattiva USB autosuspend a livello modprobe anche con `scripts/setup_pi.sh`
(crea `/etc/modprobe.d/namzero-usb.conf`).

## 3. Limiti di processo

`/etc/security/limits.d/99-audio.conf`:

```
@audio - rtprio 95
@audio - memlock unlimited
```

Richiede logout/login (o riavvio) per essere applicato al gruppo.

## 4. Ridurre il rumore di sistema

In `/boot/firmware/config.txt`:

```
# niente audio onboard, niente HDMI
dtparam=audio=off
hdmi_blanking=2

# opzionale: disabilita Bluetooth e WiFi in v1 (radio condivisa)
dtoverlay=disable-bt
dtoverlay=disable-wifi
```

Attenzione: disabilitando il WiFi perdi l'accesso SSH. Fai prima il debug in
rete, poi disabilita.

Altri accorgimenti:

```bash
# niente servizi inutili
sudo systemctl disable --now avahi-daemon cups bluetooth
sudo systemctl disable --now triggerhappy

# journal volatile, niente scritture continue sulla SD
sudo mkdir -p /etc/systemd/journald.conf.d
printf '[Journal]\nStorage=volatile\nRuntimeMaxUse=32M\n' | \
  sudo tee /etc/systemd/journald.conf.d/namzero.conf
```

## 5. Verifica

```bash
# priorità del thread audio (mentre gira)
pid=$(pgrep -f namzero-lite); chrt -p "$pid"

# temperatura e throttling
vcgencmd measure_temp
vcgencmd get_throttled      # 0x0 = nessun evento

# latenza del kernel (utile per scegliere buffer)
sudo apt-get install -y rt-tests
sudo cyclictest -m -Sp90 -i200 -h400 -l10000
```

`cyclictest` con `-Sp90` deve mostrare un massimo ben sotto i 1000 µs, meglio
sotto i 300 µs. Se il massimo è alto, il problema è quasi sempre il kernel o
le interruzioni, non il codice.

## 6. Rootfs read-only (fase M8)

Da fare solo quando il sistema è stabile, perché rende scomodo modificare i
file. Serve a proteggere la SD da spegnimenti bruschi.

```bash
sudo apt-get install -y overlayroot
# /etc/overlayroot.conf:  overlayroot="tmpfs"
sudo reboot
```

I file scritti dopo il boot finiscono in RAM e si perdono al riavvio: preset e
config vanno tenuti su una partizione separata o sincronizzati a mano.

## 7. Diagnostica rapida

| Sintomo | Prima cosa da controllare |
|---|---|
| xrun sporadici | `vcgencmd get_throttled`, alimentazione, hub USB |
| xrun continui | buffer troppo piccolo, priorità FIFO non attiva |
| max blocco > periodo | la CPU non sta nel tempo: buffer 128, catena più corta, capability realtime |
| xrun dopo un po' | swap/zram attivo e nessun `mlockall`: servono le capability (`cap_ipc_lock`) |
| click allo switch | rampa di bypass, ma anche zeri nei buffer non inizializzati |
| audio distorto | clipping sul master (`--gain`), fondo scala XTONE |
| device non trovato | `aplay -l`, numero di scheda cambiato |
| MIDI assente | `amidi -l`, `aconnect -l`, cavo OTG |

### Dopo ogni modifica: due script

Le operazioni sono divise in due, così la build non richiede mai sudo:

```bash
./scripts/build.sh        # compila e testa, nessun sudo
./scripts/install.sh      # capability, governor, servizi (chiede la password)
```

Oppure, senza compilare sul Pi (più veloce e senza OOM da LTO):

```bash
./scripts/cross-build.sh  # sul Mac, container aarch64 -> build-arm64/namzero-lite
./scripts/deploy.sh       # copia il binario sul Pi e applica setcap
```

`build.sh`:

1. si rifiuta di compilare se il motore è in esecuzione (`ETXTBSY` e contesa CPU);
2. compila con **un job** (`-j2` fa uccidere `cc1plus` dall'OOM);
3. lancia i test;
4. ricorda di eseguire `install.sh` (dopo la build le capability del file sono perse).

`install.sh` (idempotente):

1. riapplica `cap_sys_nice,cap_ipc_lock` per l'uso **manuale**;
2. mette il governor su `performance` e lo rende persistente (unit `cpu-performance`);
3. prepara il venv e le dipendenze del bridge (`dbus-fast`);
4. installa e abilita le unit **`namzero-lite`** e **`namzero-ble-bridge`**, e le avvia;
5. verifica capability, governor, stato dei due servizi e log del motore.

Le capability del file **sopravvivono ai riavvii** ma non alle ricompilazioni; il
servizio non ne ha bisogno perché la unit usa `AmbientCapabilities`. Dopo una
build rilancia `install.sh` solo se vuoi usare il motore a mano (senza servizio).

Se gli xrun restano, in ordine di efficacia:

1. `sudo setcap cap_sys_nice,cap_ipc_lock+ep build/namzero-lite` (RT + memoria
   bloccata: toglie sia la prelazione sia i page fault dello swap);
2. `"period_frames": 128` nel preset (budget 2.67 ms invece di 1.33, +1.33 ms di
   latenza);
3. governor `performance`:
   `sudo sh -c 'echo performance > /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'`;
4. meno roba accesa: riverbero/delay fuori, IR con meno tap;
5. processi pesanti accanto al motore: il bridge BLE (Bun) costa ~9% di CPU e
   ~60 MB, che su 415 MB con zram si sentono.

## Whammy: costo e latenza

Il pitch shifter del whammy (blocco FILTER, tipo `whammy`) costa poco più del
wah: su Pi Zero 2W, con blocco da 64 frame, la sola elaborazione DSP passa da
~3,4 µs (wah) a ~13,8 µs per blocco, cioè **~1% del periodo da 1,33 ms**. La
latenza aggiunta è la finestra di crossfade dichiarata (~30 ms) e viene sommata
alla catena solo quando il filter è acceso e il tipo è `whammy`.

Misura sul Pi con NAM A2 (`vox_ac30_a2`) + IR (`deluxe_lean`, 2048 tap) + delay
e riverbero, `--stats`, 5 s per intervallo:

| Configurazione | max blocco tipico | picco | xrun |
|---|---|---|---|
| filter spento (stessa catena) | 0,71–0,96 ms (53–72%) | 0,96 ms | 0 |
| filter whammy acceso | 0,72–1,04 ms (54–78%) | 1,18 ms al primo intervallo (88%) | 0 |

Il picco di 1,18 ms cade sull'intervallo in cui il filter si accende e il buffer
granulare si riempie; a regime il massimo resta intorno a 0,9–1,0 ms, con
qualche spike di scheduling oltre il 70% del periodo ma **nessuno xrun** dopo il
priming iniziale (2–3 xrun di avvio, come per qualsiasi preset). Se servisse più
margine, le leve sono le stesse della sezione precedente (buffer 128 frame, meno
tap IR, riverbero/delay fuori): ridurre la finestra del whammy sotto i 30 ms
toglierebbe poco (il DSP è ~1%) e peggiorerebbe la qualità del crossfade.
