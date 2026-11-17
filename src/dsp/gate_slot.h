#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Noise gate con isteresi: si apre sopra la soglia, si chiude con un margine
// piu' basso, cosi' non "sfarfalla" sulle note che sfumano.
class GateSlot final : public Slot {
public:
    void set_threshold_db(float db) { threshold_db_.store(db, std::memory_order_relaxed); }
    void set_attack_ms(float ms) { attack_ms_.store(ms, std::memory_order_relaxed); }
    void set_release_ms(float ms) { release_ms_.store(ms, std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        env_ = 0.0f;
        gain_ = 0.0f;
        update_coeffs();
    }

    const char* name() const override { return "gate"; }

    void process(float* interleaved, std::size_t frames) override {
        update_coeffs();

        const float thr = db_to_lin(threshold_db_.load(std::memory_order_relaxed));
        // Isteresi: si chiude solo sotto il 60% della soglia.
        const float close_thr = thr * 0.6f;

        for (std::size_t i = 0; i < frames; ++i) {
            const float l = interleaved[i * 2 + 0];
            const float r = interleaved[i * 2 + 1];
            const float x = std::max(std::fabs(l), std::fabs(r));

            env_ += (x - env_) * (x > env_ ? attack_coef_ : release_coef_);

            float target;
            if (gate_open_) {
                gate_open_ = env_ > close_thr;
                target = 1.0f;
            } else {
                gate_open_ = env_ > thr;
                target = gate_open_ ? 1.0f : 0.0f;
            }

            gain_ += (target - gain_) *
                     (target > gain_ ? attack_coef_ : release_coef_);

            interleaved[i * 2 + 0] = l * gain_;
            interleaved[i * 2 + 1] = r * gain_;
        }
    }

private:
    static float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

    void update_coeffs() {
        const float a = std::max(attack_ms_.load(std::memory_order_relaxed), 0.1f);
        const float r = std::max(release_ms_.load(std::memory_order_relaxed), 1.0f);
        attack_coef_ = 1.0f - std::exp(-1000.0f / (fs_ * a));
        release_coef_ = 1.0f - std::exp(-1000.0f / (fs_ * r));
    }

    std::atomic<float> threshold_db_{-55.0f};
    std::atomic<float> attack_ms_{2.0f};
    std::atomic<float> release_ms_{120.0f};

    float fs_ = 48000.0f;
    float attack_coef_ = 0.01f;
    float release_coef_ = 0.0002f;
    float env_ = 0.0f;
    float gain_ = 0.0f;
    bool gate_open_ = false;
};

}  // namespace namzero
