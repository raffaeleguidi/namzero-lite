#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "control/midi_parser.h"

namespace namzero {

// Ingresso MIDI via Unix socket, in ascolto.
//
// Il bridge BLE (processo separato, ble-bridge/) si connette qui e scrive byte
// MIDI grezzi. Il motore fa da server: se il bridge non c'e' il resto funziona
// lo stesso, e quando riparte si riconnette da solo.
//
// Perche' una socket e non un rawmidi virtuale: scrivere su /dev/snd/midi*
// richiede l'API ALSA, che in JS non c'e'. La socket e' ~80 righe da entrambe
// le parti e non aggiunge dipendenze al motore.
class MidiSocket {
public:
    MidiSocket() = default;
    ~MidiSocket();

    MidiSocket(const MidiSocket&) = delete;
    MidiSocket& operator=(const MidiSocket&) = delete;

    // Crea il socket e si mette in ascolto. false + err se il percorso non e'
    // utilizzabile (il chiamante decide se e' fatale: di solito no).
    bool listen(const std::string& path, std::string& err);
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool connected() const { return client_ >= 0; }

    // Accetta nuove connessioni e legge i byte disponibili. Non blocca.
    void poll(std::vector<MidiEvent>& out);

private:
    void drop_client();

    int fd_ = -1;
    int client_ = -1;
    std::string path_;
    MidiParser parser_;
    uint8_t buf_[512] = {0};
};

}  // namespace namzero
