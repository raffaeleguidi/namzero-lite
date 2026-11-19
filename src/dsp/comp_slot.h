#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Compressore feed-forward: rilevatore di picco, curva statica in dB,
// attacco/rilascio separati e makeup. Niente lookahead, quindi zero latenza.
class CompSlot final : public Slot {
public:
    void set_threshold_db(float db) { threshold_db_.store(db, std::memory_order_relaxed); }
    void set_ratio(float r) { ratio_.store(r, std::memory_order_relaxed); }
    void set_attack_ms(float ms) { attack_ms_.store(ms, std::memory_order_relaxed); }
    void set_release_ms(float ms) { release_ms_.store(ms, std::memory_order_relaxed); }
    void set_makeup_db(float db) { makeup_db_.store(db, std::memory_order_relaxed); }

    float threshold_db() const { return threshold_db_.load(std::memory_order_relaxed); }
    float ratio() const { return ratio_.load(std::memory_order_relaxed); }
    float attack_ms() const { return attack_ms_.load(std::memory_order_relaxed); }
    float release_ms() const { return release_ms_.load(std::memory_order_relaxed); }
    float makeup_db() const { return makeup_db_.load(std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        env_ = 0.0f;
        gain_ = 1.0f;
        update_coeffs();
    }

    const char* name() const override { return "comp"; }

    void process(float* interleaved, std::size_t frames) override {
        update_coeffs();

        const float thr = threshold_db_.load(std::memory_order_relaxed);
        const float ratio = std::max(ratio_.load(std::memory_order_relaxed), 1.0f);
        const float makeup = db_to_lin(makeup_db_.load(std::memory_order_relaxed));
        const float slope = 1.0f - 1.0f / ratio;

        for (std::size_t i = 0; i < frames; ++i) {
            const float l = interleaved[i * 2 + 0];
            const float r = interleaved[i * 2 + 1];
            const float x = std::max(std::fabs(l), std::fabs(r));

            env_ += (x - env_) * (x > env_ ? attack_coef_ : release_coef_);

            const float env_db = 20.0f * std::log10(env_ + 1e-9f);
            const float over = env_db - thr;

            float target = 1.0f;
            if (over > 0.0f) {
                target = db_to_lin(-over * slope);
            }
            // Il guadagno insegue il target: attacco quando scende, rilascio quando sale.
            gain_ += (target - gain_) * (target < gain_ ? attack_coef_ : release_coef_);

            const float g = gain_ * makeup;
            interleaved[i * 2 + 0] = l * g;
            interleaved[i * 2 + 1] = r * g;
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

    std::atomic<float> threshold_db_{-18.0f};
    std::atomic<float> ratio_{3.0f};
    std::atomic<float> attack_ms_{10.0f};
    std::atomic<float> release_ms_{150.0f};
    std::atomic<float> makeup_db_{3.0f};

    float fs_ = 48000.0f;
    float attack_coef_ = 0.002f;
    float release_coef_ = 0.0001f;
    float env_ = 0.0f;
    float gain_ = 1.0f;
};

}  // namespace namzero
