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
| click allo switch | rampa di bypass, ma anche zeri nei buffer non inizializzati |
| audio distorto | clipping sul master (`--gain`), fondo scala XTONE |
| device non trovato | `aplay -l`, numero di scheda cambiato |
| MIDI assente | `amidi -l`, `aconnect -l`, cavo OTG |
