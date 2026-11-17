#pragma once

#include <alsa/asoundlib.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace namzero {

struct AudioConfig {
    std::string device = "hw:XTONE";
    unsigned rate = 48000;
    unsigned period_frames = 64;
    unsigned periods = 3;
};

// Full-duplex I/O su un'unica scheda ALSA.
// Formato fisso: S32_LE interleaved, 2 canali. La conversione float<->int32
// avviene nel chiamante, cosi' il resto del motore lavora solo in float.
class AlsaDuplex {
public:
    AlsaDuplex() = default;
    ~AlsaDuplex();

    AlsaDuplex(const AlsaDuplex&) = delete;
    AlsaDuplex& operator=(const AlsaDuplex&) = delete;

    bool open(const AudioConfig& cfg, std::string& err);
    void close();

    // Restituisce il numero di frame letti/scritti, oppure un errno negativo
    // (es. -EPIPE su xrun). Bloccante: attende il periodo completo.
    long read_i32(int32_t* dst, std::size_t frames);
    long write_i32(const int32_t* src, std::size_t frames);

    // Recupera dopo un errore di stream (xrun, sospensione USB).
    // Ritorna true se lo stream e' stato ripreso.
    bool recover(int err);

    unsigned rate() const { return rate_; }
    unsigned period_frames() const { return period_frames_; }
    unsigned periods() const { return periods_; }
    const std::string& device() const { return device_; }

private:
    bool open_one(snd_pcm_t** pcm, const char* what, snd_pcm_stream_t stream,
                  std::string& err);
    bool apply_params(snd_pcm_t* pcm, const char* what, unsigned& rate,
                      unsigned& period_frames, unsigned& periods,
                      std::string& err);
    void close_one(snd_pcm_t** pcm);

    snd_pcm_t* capture_ = nullptr;
    snd_pcm_t* playback_ = nullptr;
    std::string device_;
    unsigned rate_ = 0;
    unsigned period_frames_ = 0;
    unsigned periods_ = 0;
};

}  // namespace namzero
