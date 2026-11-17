#include "dsp/ir_slot.h"

#include <algorithm>

#include "dsp/wav.h"

namespace namzero {

bool IrSlot::load(const std::string& path, std::size_t max_taps, std::string& err) {
    AudioFile f = load_ir_mono(path);
    if (!f.ok) {
        err = f.error;
        return false;
    }
    if (f.sample_rate != 0 && f.sample_rate != 48000) {
        // Non ricampioniamo: non vale la complessita' per questa fase.
        // Meglio un errore esplicito che una IR suonata alla velocita' sbagliata.
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

    taps_.swap(f.samples);
    reset();
    return true;
}

void IrSlot::prepare(double /*sample_rate*/, std::size_t /*max_frames*/) {
    reset();
}

void IrSlot::reset() {
    hist_l_.assign(taps_.size(), 0.0f);
    hist_r_.assign(taps_.size(), 0.0f);
    pos_ = 0;
}

float IrSlot::convolve(const float* hist, std::size_t pos) const {
    const std::size_t n = taps_.size();
    float acc = 0.0f;

    std::size_t k = 0;
    std::size_t i = pos;
    // Dall'indice corrente fino a 0.
    for (; k < n && i < n; ++k, --i) {
        acc += taps_[k] * hist[i];
    }
    // Poi dal fondo dell'anello.
    i = n - 1;
    for (; k < n; ++k, --i) {
        acc += taps_[k] * hist[i];
    }
    return acc;
}

void IrSlot::process(float* interleaved, std::size_t frames) {
    const std::size_t n = taps_.size();
    if (n == 0) return;

    float* hl = hist_l_.data();
    float* hr = hist_r_.data();
    std::size_t pos = pos_;

    for (std::size_t f = 0; f < frames; ++f) {
        hl[pos] = interleaved[f * 2 + 0];
        hr[pos] = interleaved[f * 2 + 1];

        const float out_l = convolve(hl, pos);
        const float out_r = convolve(hr, pos);

        interleaved[f * 2 + 0] = out_l;
        interleaved[f * 2 + 1] = out_r;

        pos = (pos + 1 == n) ? 0 : pos + 1;
    }
    pos_ = pos;
}

}  // namespace namzero
