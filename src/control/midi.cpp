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
    status_ = 0;
    need_ = 0;
    have_ = 0;
    return true;
}

void MidiInput::close() {
    if (in_ != nullptr) {
        snd_rawmidi_close(in_);
        in_ = nullptr;
    }
}

void MidiInput::feed(const uint8_t* data, std::size_t n, std::vector<MidiEvent>& out) {
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
            if ((status_ & 0xF0) == 0xB0) {  // control change
                MidiEvent e;
                e.status = status_;
                e.cc = data_[0];
                e.value = data_[1];
                out.push_back(e);
            }
        }
    }
}

void MidiInput::poll(int timeout_ms, std::vector<MidiEvent>& out) {
    if (in_ == nullptr) return;

    bool got_any = false;
    for (;;) {
        const ssize_t n = snd_rawmidi_read(in_, buf_.data(), buf_.size());
        if (n > 0) {
            got_any = true;
            feed(buf_.data(), static_cast<std::size_t>(n), out);
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
