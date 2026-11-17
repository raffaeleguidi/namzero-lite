#pragma once

#include <complex>
#include <cstddef>
#include <vector>

#include "dsp/fft.h"

namespace namzero {

// Convoluzione partizionata uniforme (overlap-save, blocchi di dimensione fissa).
//
// Costo per blocco: 2 FFT di dimensione 2*block + P moltiplicazioni spettrali,
// contro i (tap * block) prodotti della forma diretta. Con block 64 e 1024 tap
// si passa da ~131.000 moltiplicazioni a ~6.000.
//
// Nessuna latenza aggiuntiva: elabora il blocco corrente usando come storia il
// blocco precedente, quindi l'uscita e' pronta al confine del periodo.
class PartitionedConvolver {
public:
    bool init(const std::vector<float>& ir, std::size_t block);
    void reset();

    // Streaming: accetta qualsiasi lunghezza, bufferizza internamente.
    void process(const float* in, float* out, std::size_t frames);

    bool ready() const { return block_ != 0 && parts_ != 0; }
    std::size_t block_size() const { return block_; }
    std::size_t parts() const { return parts_; }

private:
    void step();

    std::size_t block_ = 0;
    std::size_t nfft_ = 0;
    std::size_t parts_ = 0;
    std::size_t cur_ = 0;

    std::vector<std::complex<float>> h_;      // spettri delle partizioni
    std::vector<std::complex<float>> x_;      // spettri di ingresso recenti
    std::vector<std::complex<float>> frame_;
    std::vector<std::complex<float>> acc_;

    std::vector<float> prev_;
    std::vector<float> in_buf_;
    std::vector<float> out_buf_;
    std::size_t in_count_ = 0;
    std::size_t out_pos_ = 0;
};

}  // namespace namzero
