#include "control/midi.h"

#include <ctime>

#include <cerrno>
#include <cstring>

namespace namzero {

MidiInput::~MidiInput() { close(); }

bool MidiInput::open(const std::string& device, std::string& err) {
    close();
    int rc = snd_rawmidi_open(&in_, nullptr, device.c_str(), SND_RAWMIDI_NONBLOCK);
    if (rc < 0) {
        err = std::string("midi ") + device + ": " + snd_strerror(rc);
        in_ = nullptr;
        return false;
    }
    buf_.resize(256);
    parser_.reset();
    return true;
}

void MidiInput::close() {
    if (in_ != nullptr) {
        snd_rawmidi_close(in_);
        in_ = nullptr;
    }
}

void MidiInput::poll(int timeout_ms, std::vector<MidiEvent>& out) {
    if (in_ == nullptr) return;

    bool got_any = false;
    for (;;) {
        const ssize_t n = snd_rawmidi_read(in_, buf_.data(), buf_.size());
        if (n > 0) {
            got_any = true;
            parser_.feed(buf_.data(), static_cast<std::size_t>(n), out);
            continue;
        }
        break;  // -EAGAIN o errore: si torna al chiamante
    }

    if (!got_any && timeout_ms > 0) {
        // Il rawmidi in non-blocking non offre un poll() portabile qui: si
        // dorme 2 ms, irrilevante rispetto alla granularita' dei comandi.
        timespec ts{};
        ts.tv_sec = 0;
        ts.tv_nsec = 2000000L;
        nanosleep(&ts, nullptr);
    }
}

}  // namespace namzero
