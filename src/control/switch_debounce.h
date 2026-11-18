#pragma once

#include <array>
#include <cstdint>

namespace namzero {

// Rilevatore di "pressione" per footswitch che mandano un control change.
//
// Il problema che risolve: un pedale momentaneo manda 127 alla pressione e 0 al
// rilascio. Un debounce a tempo sul singolo evento (ignora tutto per 250 ms)
// sembrava funzionare, ma se tenevi premuto oltre la finestra il rilascio
// arrivava oltre i 250 ms e contava come una seconda pressione: il blocco si
// accendeva e si rispegneva subito. Da qui "i footswitch non fanno niente".
//
// Qui conta il fronte di salita: il rilascio non produce mai un evento, quindi
// la durata della pressione non conta piu'.
//
// I controller che mandano solo 127 (senza rilascio) non hanno fronti: per loro
// si torna al tempo, ma solo dopo che si e' capito che il rilascio non arriva.
class SwitchDebounce {
public:
    // true se questo evento va trattato come una nuova pressione.
    bool pressed(uint8_t cc, uint8_t value, double now_ms) {
        Entry& e = entries_[cc % entries_.size()];
        const bool down = value >= kThreshold;
        const bool was_down = e.down;
        e.down = down;

        if (!down) {
            e.saw_release = true;  // da qui in poi serve un fronte
            return false;
        }

        // Rimbalzo meccanico o raffica di messaggi ravvicinati.
        if (e.used && now_ms - e.last_ms < kBounceMs) return false;

        // Con il rilascio: si scatta solo sul fronte di salita.
        if (e.saw_release && was_down) return false;

        // Senza rilascio: due pressioni si distinguono solo dal tempo.
        if (!e.saw_release && was_down && now_ms - e.last_ms < kRepeatMs) return false;

        e.used = true;
        e.last_ms = now_ms;
        return true;
    }

private:
    static constexpr uint8_t kThreshold = 64;
    static constexpr double kBounceMs = 150.0;
    static constexpr double kRepeatMs = 600.0;

    struct Entry {
        bool down = false;
        bool saw_release = false;
        bool used = false;
        double last_ms = 0.0;
    };

    std::array<Entry, 128> entries_{};
};

}  // namespace namzero
