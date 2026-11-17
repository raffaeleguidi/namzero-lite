#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dsp/partitioned_convolver.h"
#include "dsp/slot.h"

namespace namzero {

// Slot IR: convoluzione partizionata FFT.
// Costa circa un ventesimo della forma diretta a parita' di lunghezza e non
// aggiunge latenza: quando il periodo e' gia' bufferizzato, la convoluzione
// produce l'uscita allo stesso confine di blocco.
class IrSlot final : public Slot {
public:
    // Carica un WAV e ne tronca la lunghezza a max_taps.
    bool load(const std::string& path, std::size_t max_taps, std::string& err);

    void prepare(double sample_rate, std::size_t max_frames) override;
    void process(float* interleaved, std::size_t frames) override;
    void reset() override;
    const char* name() const override { return "ir"; }

    std::size_t taps() const { return taps_; }

private:
    static constexpr std::size_t kBlock = 64;

    std::size_t taps_ = 0;
    std::size_t block_ = 0;
    std::size_t capacity_ = 0;

    PartitionedConvolver left_;
    PartitionedConvolver right_;

    std::vector<float> in_l_;
    std::vector<float> in_r_;
    std::vector<float> out_l_;
    std::vector<float> out_r_;
};

}  // namespace namzero
