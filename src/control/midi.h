#pragma once

#include <alsa/asoundlib.h>

#include <cstdint>
#include <string>
#include <vector>

namespace namzero {

struct MidiEvent {
    uint8_t status = 0;  // 0xB0 = control change sul canale 1
    uint8_t cc = 0;
    uint8_t value = 0;
};

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

    // Attende fino a timeout_ms, poi restituisce gli eventi CC disponibili.
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
