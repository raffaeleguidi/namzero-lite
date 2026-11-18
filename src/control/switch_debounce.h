#pragma once

#include <array>
#include <cstdint>

namespace namzero {

// Rilevatore di "pressione" per footswitch che mandano un control change.
//
// Due famiglie di controller si comportano in modo opposto e non c'e' modo di
// distinguerle dal singolo messaggio:
//
//   a stato (latching)  ogni pressione manda un valore che alterna 127/0, una
//                       sola volta per pressione;
//   momentaneo          manda 127 alla pressione e 0 al rilascio, due messaggi
//                       per pressione.
//
// Con "a stato" ogni messaggio e' una pressione (anche val 0). Con "momentaneo"
// il rilascio non deve contare, e conta il fronte di salita. Il modo si sceglie:
// sbagliarlo significa o ignorare meta' delle pressioni, o accendere e
// rispegnere subito il blocco tenendo il pedale premuto.
class SwitchDebounce {
public:
    enum class Mode {
        kAny,   // ogni messaggio = una pressione (controller a stato)
        kEdge,  // solo il fronte di salita = pressione (controller momentaneo)
    };

    void set_mode(Mode m) { mode_ = m; }

    // true se questo evento va trattato come una nuova pressione.
    bool pressed(uint8_t cc, uint8_t value, double now_ms) {
        Entry& e = entries_[cc % entries_.size()];
        const bool down = value >= kThreshold;
        const bool was_down = e.down;
        e.down = down;

        if (mode_ == Mode::kEdge) {
            if (!down) {
                e.saw_release = true;  // il rilascio non produce mai un toggle
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

        // kAny: il valore non conta, conta che sia passato abbastanza tempo
        // dall'ultima pressione (rimbalzo o ripetizione del controller).
        if (e.used && now_ms - e.last_ms < kBounceMs) return false;
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

    Mode mode_ = Mode::kAny;
    std::array<Entry, 128> entries_{};
};

}  // namespace namzero
