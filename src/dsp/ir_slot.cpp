#include "dsp/ir_slot.h"

#include "dsp/wav.h"

namespace namzero {

bool IrSlot::load(const std::string& path, std::size_t max_taps, std::string& err) {
    AudioFile f = load_ir_mono(path);
    if (!f.ok) {
        err = f.error;
        return false;
    }
    if (f.sample_rate != 0 && f.sample_rate != 48000) {
        err = "IR a " + std::to_string(f.sample_rate) +
              " Hz: servono 48 kHz (ricampionare a parte)";
        return false;
    }
    if (max_taps > 0 && f.samples.size() > max_taps) {
        f.samples.resize(max_taps);
    }
    if (f.samples.empty()) {
        err = "IR vuota";
        return false;
    }

    if (!left_.init(f.samples, kBlock) || !right_.init(f.samples, kBlock)) {
        err = "inizializzazione della convoluzione fallita";
        return false;
    }

    taps_ = f.samples.size();
    block_ = left_.block_size();
    reset();
    return true;
}

void IrSlot::prepare(double /*sample_rate*/, std::size_t max_frames) {
    if (max_frames > capacity_) {
        capacity_ = max_frames;
        in_l_.resize(capacity_);
        in_r_.resize(capacity_);
        out_l_.resize(capacity_);
        out_r_.resize(capacity_);
    }
    reset();
}

void IrSlot::reset() {
    left_.reset();
    right_.reset();
}

void IrSlot::process(float* interleaved, std::size_t frames) {
    if (!left_.ready() || frames > capacity_) return;

    for (std::size_t i = 0; i < frames; ++i) {
        in_l_[i] = interleaved[i * 2 + 0];
        in_r_[i] = interleaved[i * 2 + 1];
    }

    left_.process(in_l_.data(), out_l_.data(), frames);
    right_.process(in_r_.data(), out_r_.data(), frames);

    for (std::size_t i = 0; i < frames; ++i) {
        interleaved[i * 2 + 0] = out_l_[i];
        interleaved[i * 2 + 1] = out_r_[i];
    }
}

}  // namespace namzero
