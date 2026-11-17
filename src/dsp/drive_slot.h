#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Boost / overdrive: pre-guadagno, saturazione morbida e filtro di tono.
// La saturazione e' un tanh: costa qualche ciclo in piu' di una razionale, ma
// non introduce aliasing strano e a 64 campioni per blocco e' irrilevante.
class DriveSlot final : public Slot {
public:
    void set_drive(float d) { drive_.store(std::clamp(d, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_tone(float t) { tone_.store(std::clamp(t, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_level_db(float db) { level_db_.store(db, std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        lp_l_ = 0.0f;
        lp_r_ = 0.0f;
        update_tone_coeff();
    }

    const char* name() const override { return "drive"; }

    void process(float* interleaved, std::size_t frames) override {
        update_tone_coeff();

        const float drive = drive_.load(std::memory_order_relaxed);
        // 1x .. 31x: il boost pulito sta in basso, l'overdrive in alto.
        const float pre = 1.0f + drive * 30.0f;
        const float level = db_to_lin(level_db_.load(std::memory_order_relaxed));
        // Compensa il guadagno: senza, alzare il drive fa solo piu' volume.
        const float norm = level / std::tanh(pre);

        for (std::size_t i = 0; i < frames; ++i) {
            const float l = std::tanh(pre * interleaved[i * 2 + 0]) * norm;
            const float r = std::tanh(pre * interleaved[i * 2 + 1]) * norm;

            lp_l_ += (l - lp_l_) * tone_coef_;
            lp_r_ += (r - lp_r_) * tone_coef_;

            interleaved[i * 2 + 0] = lp_l_;
            interleaved[i * 2 + 1] = lp_r_;
        }
    }

private:
    static float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

    void update_tone_coeff() {
        // Tono 0 -> 800 Hz (scuro), tono 1 -> 8 kHz (aperto).
        const float t = tone_.load(std::memory_order_relaxed);
        const float cutoff = 800.0f * std::pow(10.0f, t);
        tone_coef_ = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff / fs_);
    }

    std::atomic<float> drive_{0.35f};
    std::atomic<float> tone_{0.5f};
    std::atomic<float> level_db_{0.0f};

    float fs_ = 48000.0f;
    float tone_coef_ = 0.2f;
    float lp_l_ = 0.0f;
    float lp_r_ = 0.0f;
};

}  // namespace namzero
