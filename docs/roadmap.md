# Roadmap

## In corso

- **Accordatore** (modale grande, SSE) + **pitch di riferimento** nelle Impostazioni:
  analisi del dry, nota/cent/frequenza a 10–20 Hz via `/api/events`, transpose/ref
  personalizzabili, mute.

## Fatto

- **Preset dai PC della G-Board**: Program mode = slot 0–7, Control/CC mode = effetti
  (CC 50–57), LED invariati, `preset_pc`/`pc_effects`, test 99+30 — `32504b5`.
- **Fuzz octaver + flag Ottava**: `octave` (mix) + `octave_on` separato, toggle in UI,
  fade senza click — `2ece6ad`, `3f7586e`. di recente

- **AP di fallback WiFi**: il servizio `namzero-wifi` collega le reti note e,
  se nessuna è raggiungibile entro ~10 s, avvia un hotspot WPA2
  (`namzero-lite`, DHCP `shared` di NetworkManager). In AP riscansiona ogni
  ~50 s e torna in STA appena compare una rete nota; isteresi sulla perdita
  della STA, una sola radio (AP *o* STA), dry-run/test sicuri, stato e
  credenziali AP nelle Impostazioni.

- **Fuzz Octavia**: parametro `octave` (0..1) nel blocco **drive**, significativo
  solo con tipo `fuzz`: raddrizzamento a onda intera + passa-alto a 200 Hz
  miscelato al fuzz, carattere Octavia (non un pitch shifter). Manopola nella
  web UI solo con `fuzz`, salvato nel preset e nel comando `param`.

## Da fare (ordine proposto)

1. **Configurazione interfaccia audio** (Impostazioni): elenco delle interfacce
   collegate (da `aplay -l`), scelta del device, **kHz**, **ingressi** (canale
   d'ingresso per chitarra/dry del tuner), buffer/periodi; “Applica” = salva +
   reload. Il motore resta a 48 kHz internamente: per device che accettano solo
   44.1 kHz si usa `plughw:` (conversione ALSA) o si valida il device. Fallback
   al default se l'apertura fallisce. Stima: **1,5–2 giorni** (MVP), +1–2 per
   hotplug/riconnessione.

## Rimandati / da valutare

- **MIDI learn** nella UI (assegnazione CC con apprendimento).
- Ricerca **TONE3000** dentro la GUI (piano commerciale; oggi c'è il flusso Select).
- **Controlli analogici** (ESP32/ADC) per potenziometri fisici.
- Looper: undo dell'ultimo layer, export WAV su SD.
- Cambio preset in-process (oggi re-exec ~0,5 s).

## Fatto

- **Whammy** nel blocco filter (tipo wah/whammy, pitch shifter granulare,
  auto-engage, misura 1% del periodo) — `deee421`, `0545ad9`, `59edcef`.
- **EQ globale 5 bande** + **volume globale +6 dB**, **combo preset**, **UI
  progressiva iPhone** (hamburger, font 16, touch 44, safe-area) — `dbccb70`,
  `85ad06d`, `7f60ed5`.
- **Ctl multi-client** + SSE-ready broadcast, exit code corretti, fix build/deploy
  — `953d3e5`.
- **Filter/mod**: wah nel filter fisso prima del drive, mod spostabile pre/post
  IR — `f3d4068`, `d9351a2`.
- **Looper** (fine catena, quantizzato, gruppo rosso), **TONE3000** (Select +
  import), **WiFi reti note**, **defaults/undo/save**.
