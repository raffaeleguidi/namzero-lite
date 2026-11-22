#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>

namespace namzero {

// Pitch shifter granulare a due testine, pensato per il WHAMMY del blocco
// FILTER (prima del drive, comandato dal pedale di espressione).
//
// Come funziona:
//   - il segnale entra in un buffer circolare (120 ms utilizzabili a 48 kHz);
//   - due testine di lettura scorrono sul buffer a velocita' = rapporto di
//     trasposizione. Una testina che legge piu' veloce del write va piu' acuta,
//     una piu' lenta va piu' grave;
//   - le due testine sono sfasate di mezza corsa e si alternano con un crossfade
//     lineare: quando una arriva al bordo e "salta" indietro, il suo guadagno e'
//     gia' a zero. La corsa e' 60 ms, quindi la finestra di crossfade e' ~30 ms;
//   - la lettura e' interpolata linearmente (niente gradini anche a rapporto
//     non intero).
//
// Il rapporto non si applica a scatti: si insegue il target con una costante di
// tempo breve, e anche il mix dry/wet e' smussato. Cambiare range o posizione
// mentre si suona non produce click. Il buffer e' un std::array: prepare() non
// alloca e process() non fa nessuna allocazione.
//
// Latenza: la lettura sta sempre fra 1 ms e 61 ms dietro al write; il valor
// medio (e quello dichiarato) e' la finestra di crossfade, ~30 ms. Non c'e'
// altro ritardo.
class PitchShifter {
public:
    // 8192 campioni: 170 ms a 48 kHz, di cui 120 ms usabili (il resto e' margine
    // per l'interpolazione). A 96 kHz restano ~85 ms, sufficienti per la corsa.
    static constexpr std::size_t kCapacity = 8192;
    static constexpr std::size_t kMask = kCapacity - 1;

    // A 48 kHz: corsa 60 ms, crossfade 30 ms.
    static constexpr float kSpanMs = 60.0f;
    static constexpr float kWindowMs = 30.0f;
    static constexpr float kMinDelayMs = 1.0f;
    static constexpr float kBufferMs = 120.0f;

    void prepare(double sample_rate) {
        fs_ = static_cast<float>(sample_rate);
        if (fs_ < 1000.0f) fs_ = 1000.0f;

        min_delay_ = std::max(1.0f, kMinDelayMs * 0.001f * fs_);
        const float max_delay =
            std::min(kBufferMs * 0.001f * fs_, static_cast<float>(kCapacity - 2));
        span_ = std::min(kSpanMs * 0.001f * fs_, std::max(1.0f, max_delay - min_delay_));
        window_frames_ = span_ * 0.5f;

        // Coefficienti di smussamento: il rapporto in ~20 ms, il mix in ~10 ms.
        // Cambiare range/posizione/mix non produce scalini udibili.
        ratio_coef_ = 1.0f - std::exp(-1.0f / (0.020f * fs_));
        mix_coef_ = 1.0f - std::exp(-1.0f / (0.010f * fs_));

        reset();
    }

    void reset() {
        buf_l_.fill(0.0f);
        buf_r_.fill(0.0f);
        write_ = 0;
        // Fase 0.5: la testina A e' a meta' corsa con guadagno pieno, la B e'
        // all'inizio (guadagno zero). All'accensione l'uscita e' quindi la
        // testina piu' vecchia che si riempie per prima, senza salti.
        phase_ = 0.5f;
        ratio_cur_ = 1.0f;
        mix_cur_ = mix_.load(std::memory_order_relaxed);
        ratio_pub_.store(1.0f, std::memory_order_relaxed);
    }

    // Semitoni desiderati (-24..+24). Positivo = piu' acuto.
    void set_semitones(float semitones) {
        semitones_.store(clamp(semitones, -24.0f, 24.0f), std::memory_order_relaxed);
    }
    float semitones() const { return semitones_.load(std::memory_order_relaxed); }

    // Quota di wet (1 = solo pitch, 0 = solo dry).
    void set_mix(float mix) {
        mix_.store(clamp(mix, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    float mix() const { return mix_.load(std::memory_order_relaxed); }

    // Rapporto effettivamente in uso (per diagnostica/test). Lo aggiorna il
    // thread audio; la lettura da altri thread e' atomica.
    float current_ratio() const { return ratio_pub_.load(std::memory_order_relaxed); }

    // Latenza dichiarata: la finestra di crossfade (~30 ms a 48 kHz).
    std::size_t latency_frames() const {
        return static_cast<std::size_t>(window_frames_ + 0.5f);
    }

    void process(float* interleaved, std::size_t frames) {
        if (interleaved == nullptr || frames == 0) return;

        // Il target si legge una volta per blocco: pow() una volta sola.
        const float target_ratio =
            std::pow(2.0f, semitones_.load(std::memory_order_relaxed) / 12.0f);
        const float target_mix = clamp(mix_.load(std::memory_order_relaxed), 0.0f, 1.0f);

        for (std::size_t i = 0; i < frames; ++i) {
            // Smussamento: nessun salto di rapporto o di mix fra un blocco e
            // l'altro (e quindi nessun click quando il pedale si muove).
            ratio_cur_ += (target_ratio - ratio_cur_) * ratio_coef_;
            mix_cur_ += (target_mix - mix_cur_) * mix_coef_;
            const float ratio = ratio_cur_;
            const float mix = mix_cur_;

            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];
            buf_l_[write_] = in_l;
            buf_r_[write_] = in_r;

            // Avanza la fase: la testina di lettura si muove di "ratio" campioni
            // per campione scritto, quindi il ritardo varia di (1 - ratio). Il
            // segno e' invertito rispetto al ritardo: ratio > 1 = piu' acuto.
            phase_ += (1.0f - ratio) / span_;
            while (phase_ >= 1.0f) phase_ -= 1.0f;
            while (phase_ < 0.0f) phase_ += 1.0f;

            float phase_b = phase_ + 0.5f;
            if (phase_b >= 1.0f) phase_b -= 1.0f;

            // Pesi lineari complementari: 0 ai bordi (dove la testina salta),
            // 1 a meta' corsa. Le due testine sommano sempre a 1.
            const float w_a = phase_ < 0.5f ? (2.0f * phase_) : (2.0f - 2.0f * phase_);
            const float w_b = 1.0f - w_a;

            const float delay_a = min_delay_ + phase_ * span_;
            const float delay_b = min_delay_ + phase_b * span_;

            const float wet_l = read(buf_l_, delay_a) * w_a + read(buf_l_, delay_b) * w_b;
            const float wet_r = read(buf_r_, delay_a) * w_a + read(buf_r_, delay_b) * w_b;

            interleaved[i * 2 + 0] = in_l * (1.0f - mix) + wet_l * mix;
            interleaved[i * 2 + 1] = in_r * (1.0f - mix) + wet_r * mix;

            write_ = (write_ + 1) & kMask;
        }

        ratio_pub_.store(ratio_cur_, std::memory_order_relaxed);
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Legge "delay" campioni dietro al write con interpolazione lineare.
    float read(const std::array<float, kCapacity>& buf, float delay) const {
        float pos = static_cast<float>(write_) - delay;
        while (pos < 0.0f) pos += static_cast<float>(kCapacity);
        while (pos >= static_cast<float>(kCapacity)) pos -= static_cast<float>(kCapacity);

        const std::size_t i0 = static_cast<std::size_t>(pos);
        const float frac = pos - static_cast<float>(i0);
        const float a = buf[i0 & kMask];
        const float b = buf[(i0 + 1) & kMask];
        return a + (b - a) * frac;
    }

    std::atomic<float> semitones_{0.0f};
    std::atomic<float> mix_{1.0f};
    std::atomic<float> ratio_pub_{1.0f};

    std::array<float, kCapacity> buf_l_{};
    std::array<float, kCapacity> buf_r_{};
    std::size_t write_ = 0;

    float fs_ = 48000.0f;
    float min_delay_ = 48.0f;
    float span_ = 2880.0f;
    float window_frames_ = 1440.0f;
    float ratio_coef_ = 0.001f;
    float mix_coef_ = 0.002f;

    float phase_ = 0.5f;
    float ratio_cur_ = 1.0f;
    float mix_cur_ = 1.0f;
};

}  // namespace namzero
