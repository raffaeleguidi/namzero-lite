# Roadmap

## In corso

- **Accordatore** (modale grande, SSE) + **pitch di riferimento** nelle Impostazioni:
  analisi del dry, nota/cent/frequenza a 10–20 Hz via `/api/events`, transpose/ref
  personalizzabili, mute.

## Da fare (ordine proposto)

1. **Fuzz octaver** — nuovo tipo nel blocco **drive** (fuzz + raddrizzatore per
   l'ottava, parametro "octave"), carattere Octavia.
2. **Configurazione interfaccia audio** (Impostazioni): elenco delle interfacce
   collegate (da `aplay -l`), scelta del device, **kHz**, **ingressi** (canale
   d'ingresso per chitarra/dry del tuner), buffer/periodi; “Applica” = salva +
   reload. Il motore resta a 48 kHz internamente: per device che accettano solo
   44.1 kHz si usa `plughw:` (conversione ALSA) o si valida il device. Fallback
   al default se l'apertura fallisce. Stima: **1,5–2 giorni** (MVP), +1–2 per
   hotplug/riconnessione.
3. **AP di fallback WiFi** (rimandato): se nessuna rete nota è raggiungibile
   entro ~10 s dall'avvio, parte l'access point; la web UI gestisce reti e AP.

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
