#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dsp/slot.h"

namespace namzero {

// Slot IR: convoluzione diretta a lunghezza finita.
// Storicamente i cabinet IR sono lunghi (spesso > 2048 tap), quindi questa e'
// la versione semplice: costo lineare nei tap. Se il budget CPU con il NAM
// diventa stretto, si sostituisce con una partizionata FFT senza toccare
// l'interfaccia Slot.
class IrSlot final : public Slot {
public:
    // Carica un WAV e ne tronca la lunghezza a max_taps.
    bool load(const std::string& path, std::size_t max_taps, std::string& err);

    void prepare(double sample_rate, std::size_t max_frames) override;
    void process(float* interleaved, std::size_t frames) override;
    void reset() override;
    const char* name() const override { return "ir"; }

    std::size_t taps() const { return taps_.size(); }

private:
    float convolve(const float* hist, std::size_t pos) const;

    std::vector<float> taps_;
    std::vector<float> hist_l_;
    std::vector<float> hist_r_;
    std::size_t pos_ = 0;
};

}  // namespace namzero
