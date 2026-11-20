#include "control/midi_output.h"

namespace namzero {

MidiOutput::~MidiOutput() { close(); }

bool MidiOutput::open(const std::string& device, std::string& err) {
    close();
    int rc = snd_rawmidi_open(nullptr, &out_, device.c_str(), 0);
    if (rc < 0) {
        err = std::string("rawmidi out ") + device + ": " + snd_strerror(rc);
        out_ = nullptr;
        return false;
    }
    return true;
}

void MidiOutput::close() {
    if (out_ != nullptr) {
        snd_rawmidi_close(out_);
        out_ = nullptr;
    }
}

void MidiOutput::send_note(uint8_t channel, uint8_t note, uint8_t velocity) {
    if (out_ == nullptr) return;
    uint8_t msg[3] = {static_cast<uint8_t>(0x90 | (channel & 0x0F)),
                      static_cast<uint8_t>(note & 0x7F),
                      static_cast<uint8_t>(velocity & 0x7F)};
    snd_rawmidi_write(out_, msg, sizeof(msg));
    snd_rawmidi_drain(out_);
}

}  // namespace namzero
