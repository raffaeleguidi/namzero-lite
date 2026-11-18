#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dsp/slot.h"

namespace namzero {

// Riverbero Schroeder/Freeverb: 8 comb filtrati in parallelo piu' 4 allpass in
// serie, per canale, con offset stereo. Suono "da sala" credibile con costo
// bassissimo (~24 linee di ritardo per campione).
//
// I tempi di ritardo sono quelli classici a 44,1 kHz, riscalati sul sample rate
// effettivo: usare le costanti originali a 48 kHz darebbe un riverbero piu'
// corto del previsto e con risonanze diverse.
class ReverbSlot final : public Slot {
public:
    void set_room_size(float v) { room_.store(clamp01(v), std::memory_order_relaxed); }
    void set_damp(float v) { damp_.store(clamp01(v), std::memory_order_relaxed); }
    void set_wet(float v) { wet_.store(clamp01(v), std::memory_order_relaxed); }
    void set_dry(float v) { dry_.store(clamp01(v), std::memory_order_relaxed); }
    void set_width(float v) { width_.store(clamp01(v), std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        const double scale = sample_rate / 44100.0;
        static const int kComb[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int kAllpass[4] = {556, 441, 341, 225};
        static const int kStereoSpread = 23;

        for (int ch = 0; ch < 2; ++ch) {
            const int offset = (ch == 1) ? static_cast<int>(kStereoSpread * scale) : 0;
            for (int i = 0; i < 8; ++i) {
                comb_[ch][i].init(static_cast<std::size_t>(kComb[i] * scale) + offset);
            }
            for (int i = 0; i < 4; ++i) {
                allpass_[ch][i].init(static_cast<std::size_t>(kAllpass[i] * scale) + offset);
            }
        }
        reset();
    }

    void reset() override {
        for (int ch = 0; ch < 2; ++ch) {
            for (auto& c : comb_[ch]) c.clear();
            for (auto& a : allpass_[ch]) a.clear();
        }
    }

    const char* name() const override { return "reverb"; }

    void process(float* interleaved, std::size_t frames) override {
        const float room = room_.load(std::memory_order_relaxed);
        const float damp = damp_.load(std::memory_order_relaxed);
        const float wet = wet_.load(std::memory_order_relaxed);
        const float dry = dry_.load(std::memory_order_relaxed);
        const float width = width_.load(std::memory_order_relaxed);

        // Freeverb: feedback 0.7..0.98, damping mappato su 0..1.
        const float feedback = 0.7f + room * 0.28f;
        const float damp1 = damp * 0.4f;
        const float damp2 = 1.0f - damp1;
        const float wet1 = wet * (width * 0.5f + 0.5f);
        const float wet2 = wet * ((1.0f - width) * 0.5f);

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];
            const float input = (in_l + in_r) * 0.015f;  // guadagno d'ingresso Freeverb

            float out_l = 0.0f;
            float out_r = 0.0f;

            for (int c = 0; c < 8; ++c) {
                out_l += comb_[0][c].process(input, feedback, damp1, damp2);
                out_r += comb_[1][c].process(input, feedback, damp1, damp2);
            }
            for (int a = 0; a < 4; ++a) {
                out_l = allpass_[0][a].process(out_l);
                out_r = allpass_[1][a].process(out_r);
            }

            interleaved[i * 2 + 0] = in_l * dry + out_l * wet1 + out_r * wet2;
            interleaved[i * 2 + 1] = in_r * dry + out_r * wet1 + out_l * wet2;
        }
    }

private:
    static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

    struct Comb {
        std::vector<float> buf;
        std::size_t pos = 0;
        float store = 0.0f;

        void init(std::size_t n) {
            if (n < 1) n = 1;
            buf.assign(n, 0.0f);
            pos = 0;
            store = 0.0f;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.0f);
            pos = 0;
            store = 0.0f;
        }
        float process(float in, float feedback, float damp1, float damp2) {
            const float out = buf[pos];
            store = out * damp2 + store * damp1;
            buf[pos] = in + store * feedback;
            if (++pos >= buf.size()) pos = 0;
            return out;
        }
    };

    struct Allpass {
        std::vector<float> buf;
        std::size_t pos = 0;

        void init(std::size_t n) {
            if (n < 1) n = 1;
            buf.assign(n, 0.0f);
            pos = 0;
        }
        void clear() {
            std::fill(buf.begin(), buf.end(), 0.0f);
            pos = 0;
        }
        float process(float in) {
            const float bufout = buf[pos];
            const float out = -in + bufout;
            buf[pos] = in + bufout * 0.5f;
            if (++pos >= buf.size()) pos = 0;
            return out;
        }
    };

    Comb comb_[2][8];
    Allpass allpass_[2][4];

    std::atomic<float> room_{0.55f};
    std::atomic<float> damp_{0.45f};
    std::atomic<float> wet_{0.28f};
    std::atomic<float> dry_{0.75f};
    std::atomic<float> width_{1.0f};
};

}  // namespace namzero
