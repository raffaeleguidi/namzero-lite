#pragma once

#include <alsa/asoundlib.h>

#include <cstdint>
#include <string>

namespace namzero {

// Uscita rawmidi: serve ai LED del G-Board (Note On canale 1, note 0-7,
// velocity 127 = acceso / 0 = spento).
class MidiOutput {
public:
    MidiOutput() = default;
    ~MidiOutput();

    MidiOutput(const MidiOutput&) = delete;
    MidiOutput& operator=(const MidiOutput&) = delete;

    bool open(const std::string& device, std::string& err);
    void close();
    bool is_open() const { return out_ != nullptr; }

    void send_note(uint8_t channel, uint8_t note, uint8_t velocity);

private:
    snd_rawmidi_t* out_ = nullptr;
};

}  // namespace namzero
