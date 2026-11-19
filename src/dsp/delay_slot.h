#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dsp/slot.h"

namespace namzero {

// Delay stereo: tempo, feedback, mix e un filtro passa-basso nel anello di
// retroazione (il "tone"). L'eco ripetuta si scurisce a ogni giro, come nei
// delay analogici, invece di restare brillante all'infinito.
//
// Il tempo e' frazionario: si legge interpolando linearmente, cosi' cambiare il
// tempo non fa scatti.
class DelaySlot final : public Slot {
public:
    static constexpr float kMaxDelayMs = 2000.0f;

    void set_time_ms(float ms) {
        time_ms_.store(std::clamp(ms, 1.0f, kMaxDelayMs), std::memory_order_relaxed);
    }
    void set_feedback(float f) { fb_.store(std::clamp(f, 0.0f, 0.95f), std::memory_order_relaxed); }
    void set_mix(float m) { mix_.store(std::clamp(m, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_tone(float t) { tone_.store(std::clamp(t, 0.0f, 1.0f), std::memory_order_relaxed); }
    // Modalita' stereo:
    //  kStereo          due linee indipendenti, l'eco resta sul suo canale
    //  kPingPong        ingresso sommato a mono, linee incrociate: eco alternato
    //  kPingPongStereo  ingresso stereo preservato, solo la retroazione si
    //                   incrocia: piu' largo, meno definito del ping-pong classico
    enum class Mode { kStereo, kPingPong, kPingPongStereo };

    void set_mode(Mode m) { mode_.store(m, std::memory_order_relaxed); }

    float time_ms() const { return time_ms_.load(std::memory_order_relaxed); }
    float feedback() const { return fb_.load(std::memory_order_relaxed); }
    float mix() const { return mix_.load(std::memory_order_relaxed); }
    float tone() const { return tone_.load(std::memory_order_relaxed); }
    Mode mode() const { return mode_.load(std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        const std::size_t n = static_cast<std::size_t>(fs_ * kMaxDelayMs / 1000.0f) + 4;
        buf_l_.assign(n, 0.0f);
        buf_r_.assign(n, 0.0f);
        reset();
    }

    void reset() override {
        std::fill(buf_l_.begin(), buf_l_.end(), 0.0f);
        std::fill(buf_r_.begin(), buf_r_.end(), 0.0f);
        pos_ = 0;
        lp_l_ = 0.0f;
        lp_r_ = 0.0f;
    }

    const char* name() const override { return "delay"; }

    void process(float* interleaved, std::size_t frames) override {
        if (buf_l_.empty()) return;

        const float delay_samples =
            time_ms_.load(std::memory_order_relaxed) * fs_ / 1000.0f;
        const float fb = fb_.load(std::memory_order_relaxed);
        const float mix = mix_.load(std::memory_order_relaxed);
        const Mode mode = mode_.load(std::memory_order_relaxed);
        const float dry = 1.0f - mix;

        // Tono 0 -> 800 Hz (scuro), 1 -> 12 kHz (aperto).
        const float cutoff = 800.0f * std::pow(15.0f, tone_.load(std::memory_order_relaxed));
        const float a = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff / fs_);

        const std::size_t n = buf_l_.size();
        std::size_t read_pos = pos_;

        for (std::size_t i = 0; i < frames; ++i) {
            // Posizione di lettura frazionaria, indietro di delay_samples.
            float back = delay_samples;
            while (back >= static_cast<float>(n)) back -= static_cast<float>(n);
            float rpos = static_cast<float>(read_pos) - back;
            if (rpos < 0.0f) rpos += static_cast<float>(n);

            const std::size_t i0 = static_cast<std::size_t>(rpos);
            const std::size_t i1 = (i0 + 1 == n) ? 0 : i0 + 1;
            const float frac = rpos - static_cast<float>(i0);

            const float dl = buf_l_[i0] + (buf_l_[i1] - buf_l_[i0]) * frac;
            const float dr = buf_r_[i0] + (buf_r_[i1] - buf_r_[i0]) * frac;

            // Passa-basso nel anello: ogni ripetizione perde un po' di alte.
            lp_l_ += (dl - lp_l_) * a;
            lp_r_ += (dr - lp_r_) * a;

            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            switch (mode) {
                case Mode::kPingPong: {
                    // Entrata mono su una linea sola; poi ogni linea alimenta
                    // l'altra. Il primo eco e' a sinistra, il secondo a destra.
                    const float mono = 0.5f * (in_l + in_r);
                    buf_l_[read_pos] = mono + lp_r_ * fb;
                    buf_r_[read_pos] = lp_l_ * fb;
                    break;
                }
                case Mode::kPingPongStereo: {
                    // Ogni canale conserva il suo ingresso, ma la retroazione si
                    // incrocia: gli echi si alternano senza perdere la larghezza.
                    buf_l_[read_pos] = in_l + lp_r_ * fb;
                    buf_r_[read_pos] = in_r + lp_l_ * fb;
                    break;
                }
                case Mode::kStereo:
                default:
                    buf_l_[read_pos] = in_l + lp_l_ * fb;
                    buf_r_[read_pos] = in_r + lp_r_ * fb;
                    break;
            }

            interleaved[i * 2 + 0] = in_l * dry + dl * mix;
            interleaved[i * 2 + 1] = in_r * dry + dr * mix;

            if (++read_pos >= n) read_pos = 0;
        }
        pos_ = read_pos;
    }

private:
    std::atomic<float> time_ms_{350.0f};
    std::atomic<float> fb_{0.35f};
    std::atomic<float> mix_{0.25f};
    std::atomic<float> tone_{0.5f};
    std::atomic<Mode> mode_{Mode::kStereo};

    std::vector<float> buf_l_;
    std::vector<float> buf_r_;
    std::size_t pos_ = 0;
    float fs_ = 48000.0f;
    float lp_l_ = 0.0f;
    float lp_r_ = 0.0f;
};

}  // namespace namzero
