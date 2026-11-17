#pragma once

#include <string>
#include <vector>

namespace namzero {

// Risultato del parsing di un file WAV. Il contenuto viene sempre ridotto a un
// canale (le IR stereo vengono sommate) e normalizzato sul picco.
struct AudioFile {
    bool ok = false;
    std::string error;
    unsigned sample_rate = 0;
    unsigned channels = 0;
    std::vector<float> samples;
};

// Legge un WAV RIFF: PCM 8/16/24/32 bit oppure IEEE float 32/64 bit.
// Supporta anche il formato WAVE_FORMAT_EXTENSIBLE.
AudioFile load_wav(const std::string& path);

// Versione comoda: carica, riduce a mono, normalizza al picco.
AudioFile load_ir_mono(const std::string& path);

}  // namespace namzero
