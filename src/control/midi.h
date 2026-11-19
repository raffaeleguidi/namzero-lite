#pragma once

#include <alsa/asoundlib.h>

#include <cstdint>
#include <string>
#include <vector>

#include "control/midi_parser.h"

namespace namzero {

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
    snd_rawmidi_t* in_ = nullptr;
    std::vector<uint8_t> buf_;
    MidiParser parser_;
};

}  // namespace namzero
