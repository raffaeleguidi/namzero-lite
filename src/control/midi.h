#pragma once

#include <alsa/asoundlib.h>

#include <cstdint>
#include <string>
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

// Ingresso MIDI rawmidi (il device dell'XTONE: "hw:XTONE", per nome: l'indice
// della scheda cambia al boot, e' gia' successo).
// Non interpreta: consegna gli eventi. La politica sta in chi lo usa.
class MidiInput {
public:
    MidiInput() = default;
    ~MidiInput();

    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    bool open(const std::string& device, std::string& err);
    void close();
    bool is_open() const { return in_ != nullptr; }

    // Attende fino a timeout_ms, poi restituisce gli eventi disponibili.
    void poll(int timeout_ms, std::vector<MidiEvent>& out);

private:
    void feed(const uint8_t* data, std::size_t n, std::vector<MidiEvent>& out);

    snd_rawmidi_t* in_ = nullptr;
    std::vector<uint8_t> buf_;

    // Stato per il running status.
    uint8_t status_ = 0;
    uint8_t data_[2] = {0, 0};
    uint8_t need_ = 0;
    uint8_t have_ = 0;
};

}  // namespace namzero
