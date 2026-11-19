#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace namzero {

// Un messaggio MIDI "channel voice" (CC, nota, program change, ...).
// Il parser non interpreta: consegna anche quello che non ci serve, cosi' il
// log puo' mostrare cosa manda davvero un controller sconosciuto.
struct MidiEvent {
    uint8_t status = 0;  // 0xB0 = control change sul canale 1
    uint8_t cc = 0;      // primo dato (per i CC: numero del controller)
    uint8_t value = 0;   // secondo dato (per i CC: valore)
};

inline const char* midi_kind_name(uint8_t status) {
    switch (status & 0xF0) {
        case 0x80: return "noteoff";
        case 0x90: return "noteon";
        case 0xA0: return "polyaftertouch";
        case 0xB0: return "cc";
        case 0xC0: return "programchange";
        case 0xD0: return "aftertouch";
        case 0xE0: return "pitchbend";
        default: return "other";
    }
}

// Parser di byte MIDI con running status. Separato dal trasporto: lo usano sia
// il rawmidi dell'XTONE sia la socket del bridge BLE, cosi' il comportamento e'
// identico qualunque sia la sorgente.
class MidiParser {
public:
    void reset() {
        status_ = 0;
        need_ = 0;
        have_ = 0;
    }

    void feed(const uint8_t* data, std::size_t n, std::vector<MidiEvent>& out) {
        for (std::size_t i = 0; i < n; ++i) {
            const uint8_t b = data[i];

            if (b >= 0xF8) continue;  // clock realtime: ignorato
            if (b >= 0xF0) {          // system common: azzera lo stato
                status_ = 0;
                have_ = 0;
                continue;
            }

            if (b & 0x80) {  // status byte
                status_ = b;
                have_ = 0;
                const uint8_t type = b & 0xF0;
                need_ = (type == 0xC0 || type == 0xD0) ? 1 : 2;
                continue;
            }

            if (status_ == 0) continue;  // dato senza status: scartato

            data_[have_++] = b;
            if (have_ >= need_) {
                have_ = 0;
                // Si consegnano tutti i "channel voice", non solo i CC: se un
                // controller manda note o program change, il log lo deve mostrare
                // invece di far sembrare che non stia succedendo niente.
                const uint8_t type = status_ & 0xF0;
                if (type >= 0x80 && type <= 0xE0) {
                    MidiEvent e;
                    e.status = status_;
                    e.cc = data_[0];
                    e.value = (need_ == 2) ? data_[1] : 0;
                    out.push_back(e);
                }
            }
        }
    }

private:
    uint8_t status_ = 0;
    uint8_t data_[2] = {0, 0};
    uint8_t need_ = 0;
    uint8_t have_ = 0;
};

}  // namespace namzero
