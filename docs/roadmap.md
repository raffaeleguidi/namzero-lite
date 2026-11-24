# Roadmap

## In corso

- **Accordatore** (modale grande, SSE) + **pitch di riferimento** nelle Impostazioni:
  analisi del dry, nota/cent/frequenza a 10–20 Hz via `/api/events`, transpose/ref
  personalizzabili, mute.

## Fatto

- **Interfaccia audio configurabile** (device/buffer/periodi/ingresso, offline,
  fallback, UI Impostazioni) — `219e891`.

- **Preset dai PC della G-Board**: Program mode = slot 0–7, Control/CC mode = effetti
  (CC 50–57), LED invariati, `preset_pc`/`pc_effects`, test 99+30 — `32504b5`.
- **AP di fallback WiFi**: il servizio `namzero-wifi` collega le reti note e,
  se nessuna è raggiungibile entro ~10 s, avvia un hotspot WPA2
  (`namzero-lite`, DHCP `shared` di NetworkManager). In AP riscansiona ogni
  ~50 s e torna in STA appena compare una rete nota; isteresi sulla perdita
  della STA, una sola radio (AP *o* STA), dry-run/test sicuri, stato e
  credenziali AP nelle Impostazioni.

## Da fare

Nessun item pianificato: tutte le feature previste sono implementate. Restano le
**verifiche manuali** e i **rimandati**.

## Verifiche manuali (hardware/rete)

- `sudo ./scripts/install.sh` → installa anche `namzero-wifi` e il drop-in DNS
  per `http://namzero-lite:8080`; cambia la password AP (default `namzero-lite`);
  test AP fuori casa con `scripts/measure_ap.py` (vedi docs/web.md).
- **G-Board** (editor iCON): Control/CC mode = CC 50–57 (mod/drive/boost), i
  Program Change sono ignorati (`preset_pc: false`, `pc_effects: false`); Note On
  dei LED 0-based (note 0-7).
- **Accordatore**: 415.3/0 oppure 440/−1; cantini B3/E4; ronzio; nota verde.
- **EQ/Volume +6 dB**: livelli (soft_clip); EQ off lascia comunque il volume.
- **Interfaccia audio**: `plughw:XTONE` + Applica; distacco/recupero XTONE; stato
  offline; sezione da telefono.
- **Looper**: REC/PLAY/DUB, stop/riprendi, clear, con il gruppo rosso.

## Rimandati / da valutare

- **MIDI learn** nella UI (assegnazione CC con apprendimento).
- Ricerca **TONE3000** dentro la GUI (piano commerciale; oggi c'è il flusso Select).
- **Controlli analogici** (ESP32/ADC) per potenziometri fisici.
- Looper: undo dell'ultimo layer, export WAV su SD.
- Cambio preset in-process (oggi re-exec ~0,5 s).
- Audio: hotplug/riconnessione a caldo; ingressi >2 canali; kHz nativo (oggi 48k
  interno / `plughw:` per 44.1k).

## Fatto

- **EQ globale 5 bande** + **volume globale +6 dB**, **combo preset**, **UI
  progressiva iPhone** (hamburger, font 16, touch 44, safe-area) — `dbccb70`,
  `85ad06d`, `7f60ed5`.
- **Ctl multi-client** + SSE-ready broadcast, exit code corretti, fix build/deploy
  — `953d3e5`.
- **Filter/mod**: wah nel filter fisso prima del drive, mod spostabile pre/post
  IR — `f3d4068`, `d9351a2`.
- **Looper** (fine catena, quantizzato, gruppo rosso), **TONE3000** (Select +
  import), **WiFi reti note**, **defaults/undo/save**.
