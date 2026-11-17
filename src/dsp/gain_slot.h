#pragma once

#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Slot di comodo: gain in dB con rampa lenta sul cambio di valore.
// Serve da smoke test della catena e del bypass.
class GainSlot final : public Slot {
public:
    explicit GainSlot(float db = 0.0f) : target_db_(db) {}

    void set_db(float db) { target_db_.store(db, std::memory_order_relaxed); }

    void prepare(double /*sample_rate*/, std::size_t /*max_frames*/) override {
        gain_ = db_to_lin(target_db_.load(std::memory_order_relaxed));
    }

    void process(float* interleaved, std::size_t frames) override {
        const float target = db_to_lin(target_db_.load(std::memory_order_relaxed));
        for (std::size_t i = 0; i < frames; ++i) {
            gain_ += (target - gain_) * 0.001f;
            interleaved[i * 2 + 0] *= gain_;
            interleaved[i * 2 + 1] *= gain_;
        }
    }

    void reset() override {
        gain_ = db_to_lin(target_db_.load(std::memory_order_relaxed));
    }

    const char* name() const override { return "gain"; }

private:
    static float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

    std::atomic<float> target_db_{0.0f};
    float gain_ = 1.0f;
};

}  // namespace namzero
