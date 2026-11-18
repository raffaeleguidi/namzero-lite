#pragma once

#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Tremolo: LFO sinusoidale su un gain, con profondita' e frequenza regolabili.
// L'LFO e' a fase continua, quindi cambiare rate non produce salti udibili.
class TremoloSlot final : public Slot {
public:
    void set_rate_hz(float hz) { rate_.store(clamp(hz, 0.1f, 20.0f), std::memory_order_relaxed); }
    void set_depth(float d) { depth_.store(clamp(d, 0.0f, 1.0f), std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        phase_ = 0.0f;
    }

    void reset() override { phase_ = 0.0f; }

    const char* name() const override { return "mod"; }

    void process(float* interleaved, std::size_t frames) override {
        const float depth = depth_.load(std::memory_order_relaxed);
        const float inc = rate_.load(std::memory_order_relaxed) / fs_;

        for (std::size_t i = 0; i < frames; ++i) {
            const float lfo = 0.5f + 0.5f * std::sin(2.0f * 3.14159265f * phase_);
            const float gain = 1.0f - depth * (1.0f - lfo);

            interleaved[i * 2 + 0] *= gain;
            interleaved[i * 2 + 1] *= gain;

            phase_ += inc;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    std::atomic<float> rate_{4.5f};
    std::atomic<float> depth_{0.6f};
    float fs_ = 48000.0f;
    float phase_ = 0.0f;
};

}  // namespace namzero
