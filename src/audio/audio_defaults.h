#pragma once

#include <algorithm>
#include <string>
#include <vector>

namespace namzero {

// Interfaccia audio globale: vive in config/presets.json -> blocco "audio".
// E' l'ultima opzione di sistema che la UI puo' cambiare senza ricompilare.
//
// Il rate NON e' configurabile: tutta la catena (NAM, IR, delay, accordatore)
// assume 48 kHz. Un device che accetta solo 44.1 kHz si usa con il nome
// "plughw:CARD,DEV": ALSA converte, il motore resta a 48 kHz.
constexpr unsigned kAudioRate = 48000;
constexpr unsigned kAudioBufferMin = 16;
constexpr unsigned kAudioBufferMax = 2048;
constexpr unsigned kAudioPeriodsMin = 2;
constexpr unsigned kAudioPeriodsMax = 8;
// AlsaDuplex apre sempre 2 canali: l'ingresso sceglie quale dei due porta il
// segnale alla catena (0 = L, 1 = R). Interfacce con piu' canali si vedono da
// ALSA solo tramite un plugin che espone 2 canali.
constexpr unsigned kAudioInputMin = 0;
constexpr unsigned kAudioInputMax = 1;

struct AudioSettings {
    std::string device = "hw:XTONE";
    unsigned buffer = 64;
    unsigned periods = 3;
    unsigned input = 0;
};

inline unsigned clamp_audio_buffer(unsigned v) {
    return std::clamp(v, kAudioBufferMin, kAudioBufferMax);
}

inline unsigned clamp_audio_periods(unsigned v) {
    return std::clamp(v, kAudioPeriodsMin, kAudioPeriodsMax);
}

inline unsigned clamp_audio_input(unsigned v) {
    return std::clamp(v, kAudioInputMin, kAudioInputMax);
}

// Ordine di apertura: il device configurato, poi "default", poi il device
// storico "hw:XTONE". I doppioni si eliminano: se il configurato e' gia'
// "default" non lo si riprova due volte.
inline std::vector<std::string> audio_device_candidates(const std::string& configured) {
    std::vector<std::string> out;
    const auto add = [&](const std::string& dev) {
        if (dev.empty()) return;
        for (const auto& d : out) {
            if (d == dev) return;
        }
        out.push_back(dev);
    };
    add(configured);
    add("default");
    add("hw:XTONE");
    return out;
}

}  // namespace namzero
