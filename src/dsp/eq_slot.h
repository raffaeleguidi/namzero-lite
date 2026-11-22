#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>

#include "dsp/slot.h"

namespace namzero {

// EQ grafico globale a 5 bande, sempre in catena in fondo (dopo riverbero e
// looper, prima del gain master/boost). Biquad Direct Form I:
//   band 0: low shelf  @ 80 Hz
//   band 1: peaking    @ 250 Hz, Q 0.7
//   band 2: peaking    @ 800 Hz, Q 0.7
//   band 3: peaking    @ 2500 Hz, Q 0.7
//   band 4: high shelf @ 8000 Hz
//
// I guadagni sono atomici e aggiornabili dal thread di controllo. Il thread
// audio ricontrolla una revisione all'inizio del blocco e ricalcola i
// coefficienti una volta sola: niente trig per frame. A 0 dB la banda e'
// spenta (coefficienti identita'), quindi il segnale passa inalterato.
class EqSlot final : public Slot {
public:
    static constexpr int kBandCount = 5;

    static constexpr float kMinDb = -12.0f;
    static constexpr float kMaxDb = 12.0f;

    EqSlot() {
        for (auto& g : gains_) g.store(0.0f, std::memory_order_relaxed);
        for (auto& c : coeff_) c = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        for (auto& a : active_) a = false;
    }

    // Chiamabile dal thread ctl. Il clamp e' qui: anche un comando sbagliato
    // non puo' far esplodere il filtro.
    void set_gain(int band, float db) {
        if (band < 0 || band >= kBandCount) return;
        if (db < kMinDb) db = kMinDb;
        if (db > kMaxDb) db = kMaxDb;
        gains_[band].store(db, std::memory_order_relaxed);
        // release: i guadagni scritti prima della revisione sono visibili al
        // thread audio che carica la revisione con acquire in process().
        revision_.fetch_add(1, std::memory_order_release);
    }

    float gain_db(int band) const {
        if (band < 0 || band >= kBandCount) return 0.0f;
        return gains_[band].load(std::memory_order_relaxed);
    }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = sample_rate > 0.0 ? static_cast<float>(sample_rate) : 48000.0f;
        reset();
    }

    void reset() override {
        for (auto& band : state_) {
            for (auto& st : band) st = BiquadState{};
        }
        // Revisione letta prima del ricalcolo: se il ctl cambia un guadagno nel
        // frattempo, il blocco successivo vede la revisione nuova e ricalcola.
        const int rev = revision_.load(std::memory_order_acquire);
        update_coefficients();
        applied_revision_ = rev;
    }

    const char* name() const override { return "eq"; }

    void process(float* interleaved, std::size_t frames) override {
        // Una sola lettura della revisione per blocco. Se il ctl cambia un
        // guadagno a meta' blocco, il ricalcolo avverra' al blocco successivo.
        const int rev = revision_.load(std::memory_order_acquire);
        if (rev != applied_revision_) {
            update_coefficients();
            applied_revision_ = rev;
        }

        for (std::size_t i = 0; i < frames; ++i) {
            for (int ch = 0; ch < kChannels; ++ch) {
                float x = interleaved[i * kChannels + ch];
                for (int b = 0; b < kBandCount; ++b) {
                    if (!active_[b]) continue;
                    const Coeff& c = coeff_[b];
                    BiquadState& st = state_[b][ch];
                    const float y = c.b0 * x + c.b1 * st.x1 + c.b2 * st.x2 -
                                    c.a1 * st.y1 - c.a2 * st.y2;
                    st.x2 = st.x1;
                    st.x1 = x;
                    st.y2 = st.y1;
                    st.y1 = y;
                    x = y;
                }
                interleaved[i * kChannels + ch] = x;
            }
        }
    }

private:
    static constexpr int kChannels = 2;

    struct Coeff {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
    };

    struct BiquadState {
        float x1 = 0.0f;
        float x2 = 0.0f;
        float y1 = 0.0f;
        float y2 = 0.0f;
    };

    static float frequency(int band) {
        switch (band) {
            case 0: return 80.0f;
            case 1: return 250.0f;
            case 2: return 800.0f;
            case 3: return 2500.0f;
            default: return 8000.0f;
        }
    }

    // Ricalcola tutti i coefficienti. Gira una volta per blocco, solo quando
    // la revisione e' cambiata (o in prepare/reset).
    void update_coefficients() {
        for (int b = 0; b < kBandCount; ++b) {
            const float db = gains_[b].load(std::memory_order_relaxed);
            if (std::fabs(db) < 1e-6f) {
                coeff_[b] = Coeff{};
                if (active_[b]) {
                    for (auto& st : state_[b]) st = BiquadState{};
                }
                active_[b] = false;
                continue;
            }

            const float f0 = frequency(b);
            // Il sample rate e' fisso per tutta la vita dello slot, ma un
            // clamp evita w0 fuori banda se un giorno cambiasse.
            const float nyquist = fs_ * 0.5f;
            const float f = std::min(f0, nyquist * 0.98f);
            const float a = std::pow(10.0f, db / 40.0f);  // ampiezza RBJ
            constexpr float kPi = 3.14159265358979323846f;
            const float w0 = 2.0f * kPi * f / fs_;
            const float cosw = std::cos(w0);
            const float sinw = std::sin(w0);

            float b0, b1, b2, a0, a1, a2;
            if (b == 0 || b == 4) {
                // Shelf con pendenza S = 1 (nessuna risonanza).
                const float alpha = sinw * 0.5f * std::sqrt(2.0f);
                const float sqrt_a = std::sqrt(a);
                if (b == 0) {
                    b0 = a * ((a + 1.0f) - (a - 1.0f) * cosw +
                              2.0f * sqrt_a * alpha);
                    b1 = 2.0f * a * ((a - 1.0f) - (a + 1.0f) * cosw);
                    b2 = a * ((a + 1.0f) - (a - 1.0f) * cosw -
                              2.0f * sqrt_a * alpha);
                    a0 = (a + 1.0f) + (a - 1.0f) * cosw +
                         2.0f * sqrt_a * alpha;
                    a1 = -2.0f * ((a - 1.0f) + (a + 1.0f) * cosw);
                    a2 = (a + 1.0f) + (a - 1.0f) * cosw -
                         2.0f * sqrt_a * alpha;
                } else {
                    b0 = a * ((a + 1.0f) + (a - 1.0f) * cosw +
                              2.0f * sqrt_a * alpha);
                    b1 = -2.0f * a * ((a - 1.0f) + (a + 1.0f) * cosw);
                    b2 = a * ((a + 1.0f) + (a - 1.0f) * cosw -
                              2.0f * sqrt_a * alpha);
                    a0 = (a + 1.0f) - (a - 1.0f) * cosw +
                         2.0f * sqrt_a * alpha;
                    a1 = 2.0f * ((a - 1.0f) - (a + 1.0f) * cosw);
                    a2 = (a + 1.0f) - (a - 1.0f) * cosw -
                         2.0f * sqrt_a * alpha;
                }
            } else {
                // Peaking: Q 0.7, larghezza musicale senza risonanze.
                constexpr float kQ = 0.7f;
                const float alpha = sinw / (2.0f * kQ);
                b0 = 1.0f + alpha * a;
                b1 = -2.0f * cosw;
                b2 = 1.0f - alpha * a;
                a0 = 1.0f + alpha / a;
                a1 = -2.0f * cosw;
                a2 = 1.0f - alpha / a;
            }

            const float inv = 1.0f / a0;
            coeff_[b] = Coeff{b0 * inv, b1 * inv, b2 * inv, a1 * inv, a2 * inv};
            active_[b] = true;
        }
    }

    std::atomic<float> gains_[kBandCount];
    std::atomic<int> revision_{0};

    float fs_ = 48000.0f;
    Coeff coeff_[kBandCount];
    bool active_[kBandCount];
    BiquadState state_[kBandCount][kChannels];
    int applied_revision_ = 0;
};

}  // namespace namzero
