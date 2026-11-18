#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Wah: filtro a stato variabile (TPT/Cytomic) in bandpass. La frequenza segue
// la posizione del pedale su scala logaritmica, perche' la percezione e' log.
// Sta prima dell'amplificatore, come un pedale davanti all'input.
class WahSlot final : public Slot {
public:
    void set_position(float p) { position_.store(clamp(p, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_q(float q) { q_.store(clamp(q, 0.5f, 12.0f), std::memory_order_relaxed); }
    void set_mix(float m) { mix_.store(clamp(m, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_low_hz(float hz) { low_hz_.store(clamp(hz, 100.0f, 1000.0f), std::memory_order_relaxed); }
    void set_high_hz(float hz) { high_hz_.store(clamp(hz, 600.0f, 5000.0f), std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        ic1_l_ = ic2_l_ = ic1_r_ = ic2_r_ = 0.0f;
    }

    const char* name() const override { return "wah"; }

    void process(float* interleaved, std::size_t frames) override {
        // Coefficienti una volta per blocco: la posizione cambia lentamente e
        // un tan() per campione sarebbe sprecato.
        const float pos = position_.load(std::memory_order_relaxed);
        const float lo = low_hz_.load(std::memory_order_relaxed);
        const float hi = high_hz_.load(std::memory_order_relaxed);
        const float q = q_.load(std::memory_order_relaxed);
        const float fc = lo * std::pow(hi / lo, pos);

        const float g = std::tan(3.14159265f * fc / fs_);
        const float k = 1.0f / q;
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float a2 = g * a1;
        const float a3 = g * a2;

        const float mix = mix_.load(std::memory_order_relaxed);
        const float dry = 1.0f - mix;
        // Compensa il guadagno del bandpass, che altrimenti suona fiacco.
        const float boost = 1.0f + 0.5f * mix * q / 3.0f;

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            const float v3l = in_l - ic2_l_;
            const float v1l = a1 * ic1_l_ + a2 * v3l;
            const float v2l = ic2_l_ + a2 * ic1_l_ + a3 * v3l;
            ic1_l_ = 2.0f * v1l - ic1_l_;
            ic2_l_ = 2.0f * v2l - ic2_l_;

            const float v3r = in_r - ic2_r_;
            const float v1r = a1 * ic1_r_ + a2 * v3r;
            const float v2r = ic2_r_ + a2 * ic1_r_ + a3 * v3r;
            ic1_r_ = 2.0f * v1r - ic1_r_;
            ic2_r_ = 2.0f * v2r - ic2_r_;

            interleaved[i * 2 + 0] = in_l * dry + v1l * mix * boost;
            interleaved[i * 2 + 1] = in_r * dry + v1r * mix * boost;
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    std::atomic<float> position_{0.5f};
    std::atomic<float> q_{3.0f};
    std::atomic<float> mix_{1.0f};
    std::atomic<float> low_hz_{350.0f};
    std::atomic<float> high_hz_{2200.0f};

    float fs_ = 48000.0f;
    float ic1_l_ = 0.0f, ic2_l_ = 0.0f;
    float ic1_r_ = 0.0f, ic2_r_ = 0.0f;
};

}  // namespace namzero
