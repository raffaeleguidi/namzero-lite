#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Slot modulazione, con due tipi:
//   tremolo — LFO sinusoidale sul gain, fase continua (cambiare rate non scatta)
//   wah     — filtro a stato variabile (TPT) in bandpass, frequenza data dalla
//             posizione del pedale su scala logaritmica
//
// Il wah sta prima dell'amplificatore, come un pedale vero: filtrare dopo il
// modello darebbe un suono diverso e meno credibile.
class ModSlot final : public Slot {
public:
    enum class Type { kTremolo, kWah };

    void set_type(Type t) { type_.store(t, std::memory_order_relaxed); }

    // tremolo
    void set_rate_hz(float hz) { rate_.store(clamp(hz, 0.1f, 20.0f), std::memory_order_relaxed); }
    void set_depth(float d) { depth_.store(clamp(d, 0.0f, 1.0f), std::memory_order_relaxed); }

    // wah
    void set_position(float p) { position_.store(clamp(p, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_wah_q(float q) { q_.store(clamp(q, 0.5f, 12.0f), std::memory_order_relaxed); }
    void set_wah_mix(float m) { mix_.store(clamp(m, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_wah_low_hz(float hz) { low_hz_.store(clamp(hz, 100.0f, 1000.0f), std::memory_order_relaxed); }
    void set_wah_high_hz(float hz) { high_hz_.store(clamp(hz, 600.0f, 5000.0f), std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        phase_ = 0.0f;
        ic1_l_ = ic2_l_ = ic1_r_ = ic2_r_ = 0.0f;
        coef_valid_ = false;
    }

    const char* name() const override { return "mod"; }

    void process(float* interleaved, std::size_t frames) override {
        if (type_.load(std::memory_order_relaxed) == Type::kWah) {
            process_wah(interleaved, frames);
        } else {
            process_tremolo(interleaved, frames);
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    void process_tremolo(float* interleaved, std::size_t frames) {
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

    // Aggiorna i coefficienti una volta per blocco: la posizione del pedale
    // cambia lentamente, e un tan() per campione sarebbe sprecato.
    void update_wah_coeffs() {
        const float pos = position_.load(std::memory_order_relaxed);
        const float lo = low_hz_.load(std::memory_order_relaxed);
        const float hi = high_hz_.load(std::memory_order_relaxed);
        // Scala logaritmica: la corsa del pedale deve corrispondere alla
        // percezione dell'orecchio, non alla frequenza lineare.
        const float fc = lo * std::pow(hi / lo, pos);

        // Il Q scende verso la punta. A Q pieno la risonanza sulle alte, dove
        // il filtro si apre, diventa "acida": il picco stretto a 2 kHz suona
        // come un fischio, non come un wah. Al tacco il filtro resta selettivo.
        const float q_raw = q_.load(std::memory_order_relaxed);
        q_eff_ = q_raw * (1.0f - 0.45f * pos);
        if (q_eff_ < 0.6f) q_eff_ = 0.6f;

        const float g = std::tan(3.14159265f * fc / fs_);
        const float k = 1.0f / q_eff_;
        a1_ = 1.0f / (1.0f + g * (g + k));
        a2_ = g * a1_;
        a3_ = g * a2_;
        coef_valid_ = true;
    }

    void process_wah(float* interleaved, std::size_t frames) {
        update_wah_coeffs();
        const float mix = mix_.load(std::memory_order_relaxed);
        const float dry = 1.0f - mix;
        // Compensa il guadagno del bandpass, che altrimenti suona fiacco. Usa
        // il Q effettivo: a Q pieno la compensazione amplificherebbe proprio il
        // picco che vogliamo addolcire.
        const float boost = 1.0f + 0.5f * mix * q_eff_ / 3.0f;

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            // Cytomic/TPT state variable filter, uscita bandpass.
            const float v3l = in_l - ic2_l_;
            const float v1l = a1_ * ic1_l_ + a2_ * v3l;
            const float v2l = ic2_l_ + a2_ * ic1_l_ + a3_ * v3l;
            ic1_l_ = 2.0f * v1l - ic1_l_;
            ic2_l_ = 2.0f * v2l - ic2_l_;

            const float v3r = in_r - ic2_r_;
            const float v1r = a1_ * ic1_r_ + a2_ * v3r;
            const float v2r = ic2_r_ + a2_ * ic1_r_ + a3_ * v3r;
            ic1_r_ = 2.0f * v1r - ic1_r_;
            ic2_r_ = 2.0f * v2r - ic2_r_;

            interleaved[i * 2 + 0] = in_l * dry + v1l * mix * boost;
            interleaved[i * 2 + 1] = in_r * dry + v1r * mix * boost;
        }
    }

    std::atomic<Type> type_{Type::kTremolo};

    std::atomic<float> rate_{4.5f};
    std::atomic<float> depth_{0.6f};

    std::atomic<float> position_{0.5f};
    std::atomic<float> q_{3.0f};
    std::atomic<float> mix_{1.0f};
    std::atomic<float> low_hz_{350.0f};
    std::atomic<float> high_hz_{2200.0f};

    float fs_ = 48000.0f;
    float phase_ = 0.0f;

    float q_eff_ = 3.0f;
    float a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_l_ = 0.0f, ic2_l_ = 0.0f;
    float ic1_r_ = 0.0f, ic2_r_ = 0.0f;
    bool coef_valid_ = false;
};

}  // namespace namzero
