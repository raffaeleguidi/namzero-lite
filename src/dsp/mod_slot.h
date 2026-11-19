#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Slot modulazione, con cinque tipi:
//   tremolo  — LFO sinusoidale sul gain
//   wah      — filtro a stato variabile (TPT) comandato dal pedale
//   univibe  — 4 celle di sfasamento con lampada: fase e ampiezza modulate
//   flanger  — delay corto modulato con retroazione (comb filter)
//   phaser   — 4 celle di sfasamento con retroazione
//
// La modulazione sta prima dell'amplificatore, come un pedale vero: filtrare
// dopo il modello darebbe un suono diverso e meno credibile.
class ModSlot final : public Slot {
public:
    enum class Type { kTremolo, kWah, kUnivibe, kFlanger, kPhaser };

    void set_type(Type t) { type_.store(t, std::memory_order_relaxed); }
    Type type() const { return type_.load(std::memory_order_relaxed); }

    // tremolo e effetti modulati
    void set_rate_hz(float hz) { rate_.store(clamp(hz, 0.05f, 20.0f), std::memory_order_relaxed); }
    void set_depth(float d) { depth_.store(clamp(d, 0.0f, 1.0f), std::memory_order_relaxed); }

    // wah
    void set_position(float p) { position_.store(clamp(p, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_wah_q(float q) { q_.store(clamp(q, 0.5f, 12.0f), std::memory_order_relaxed); }
    void set_wah_mix(float m) { mix_.store(clamp(m, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_wah_low_hz(float hz) { low_hz_.store(clamp(hz, 100.0f, 1000.0f), std::memory_order_relaxed); }
    void set_wah_high_hz(float hz) { high_hz_.store(clamp(hz, 600.0f, 5000.0f), std::memory_order_relaxed); }

    // univibe / flanger / phaser
    void set_fx_mix(float m) { fx_mix_.store(clamp(m, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_feedback(float f) { feedback_.store(clamp(f, 0.0f, 0.85f), std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        phase_ = 0.0f;
        ic1_l_ = ic2_l_ = ic1_r_ = ic2_r_ = 0.0f;
        coef_valid_ = false;
        for (auto& ap : ap_l_) ap = AllPass{};
        for (auto& ap : ap_r_) ap = AllPass{};
        fb_l_ = fb_r_ = 0.0f;
        hpf_lp_l_ = hpf_lp_r_ = 0.0f;
        dl_l_.fill(0.0f);
        dl_r_.fill(0.0f);
        write_ = 0;
    }

    const char* name() const override { return "mod"; }

    void process(float* interleaved, std::size_t frames) override {
        switch (type_.load(std::memory_order_relaxed)) {
            case Type::kWah: process_wah(interleaved, frames); break;
            case Type::kUnivibe: process_phaser(interleaved, frames, true); break;
            case Type::kPhaser: process_phaser(interleaved, frames, false); break;
            case Type::kFlanger: process_flanger(interleaved, frames); break;
            default: process_tremolo(interleaved, frames); break;
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Cella di sfasamento del primo ordine: H(z) = (a + z^-1) / (1 + a z^-1).
    struct AllPass {
        float a = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;

        float process(float x) {
            const float y = a * x + x1 - a * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    float allpass_coef(float fc) const {
        const float t = std::tan(3.14159265f * fc / fs_);
        return (t - 1.0f) / (t + 1.0f);
    }

    static float lfo_value(float phase) {
        return 0.5f + 0.5f * std::sin(6.2831853f * phase);
    }

    void process_tremolo(float* interleaved, std::size_t frames) {
        const float depth = depth_.load(std::memory_order_relaxed);
        const float inc = rate_.load(std::memory_order_relaxed) / fs_;

        for (std::size_t i = 0; i < frames; ++i) {
            const float lfo = lfo_value(phase_);
            const float gain = 1.0f - depth * (1.0f - lfo);
            interleaved[i * 2 + 0] *= gain;
            interleaved[i * 2 + 1] *= gain;
            phase_ += inc;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
        }
    }

    // ------------------------------------------------------------------ wah
    void update_wah_coeffs() {
        const float pos = position_.load(std::memory_order_relaxed);
        const float lo = low_hz_.load(std::memory_order_relaxed);
        const float hi = high_hz_.load(std::memory_order_relaxed);
        const float fc = lo * std::pow(hi / lo, pos);

        // Il Q scende verso la punta. A Q pieno la risonanza sulle alte, dove
        // il filtro si apre, diventa "acida": il picco stretto a 2 kHz suona
        // come un fischio, non come un wah. Al tacco resta selettivo.
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
        const float boost = 1.0f + 0.5f * mix * q_eff_ / 3.0f;

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

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

    // ------------------------------------------- univibe e phaser (4 celle)
    void process_phaser(float* interleaved, std::size_t frames, bool univibe) {
        const float rate = rate_.load(std::memory_order_relaxed);
        const float depth = depth_.load(std::memory_order_relaxed);
        const float mix = fx_mix_.load(std::memory_order_relaxed);
        const float fb = univibe ? 0.0f : feedback_.load(std::memory_order_relaxed);
        const float inc = rate / fs_;
        const float hpf_coef = 1.0f - std::exp(-2.0f * 3.14159265f * 280.0f / fs_);

        for (std::size_t i = 0; i < frames; ++i) {
            const float lfo = lfo_value(phase_);

            // Il LFO sposta lo sfasamento su due decadi (200 Hz..2 kHz). I
            // coefficienti si aggiornano ogni 16 campioni: costa un tan ogni 16
            // e il passo e' comunque molto piu' fine di quanto serva all'orecchio.
            if ((i & 15u) == 0u) {
                // Sweep piu' alto del primo tentativo (era 200 Hz..2 kHz): con le
                // tacche nei bassi il phaser suonava "scatoloso". 350 Hz..3 kHz
                // lo tiene nella zona dove il comb si sente come movimento.
                const float fc = 350.0f * std::pow(3000.0f / 350.0f, depth * lfo);
                const float a = allpass_coef(fc);
                for (auto& ap : ap_l_) ap.a = a;
                for (auto& ap : ap_r_) ap.a = a;
            }

            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            float xl = in_l + fb * fb_l_;
            float xr = in_r + fb * fb_r_;
            for (auto& ap : ap_l_) xl = ap.process(xl);
            for (auto& ap : ap_r_) xr = ap.process(xr);
            fb_l_ = xl;
            fb_r_ = xr;

            // Passa-alto sul solo segnale trattato: le tacche del comb sulle
            // basse non si sentono come effetto, si sentono come scatola.
            // L'univibe non ce l'ha: gli toglierebbe il corpo che gli piace.
            if (!univibe) {
                hpf_lp_l_ += (xl - hpf_lp_l_) * hpf_coef;
                hpf_lp_r_ += (xr - hpf_lp_r_) * hpf_coef;
                xl -= hpf_lp_l_;
                xr -= hpf_lp_r_;
            }

            // L'univibe non e' solo fase: la lampada modula anche l'ampiezza,
            // ed e' quella pulsazione a dargli il carattere "vivo".
            const float am = univibe ? (1.0f - 0.18f * depth + 0.18f * depth * lfo) : 1.0f;

            interleaved[i * 2 + 0] = in_l * (1.0f - mix) + xl * mix * am;
            interleaved[i * 2 + 1] = in_r * (1.0f - mix) + xr * mix * am;

            phase_ += inc;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
        }
    }

    // ---------------------------------------------------------------- flanger
    static constexpr std::size_t kDelayLen = 2048;  // 42 ms a 48 kHz

    float read_delay(const std::array<float, kDelayLen>& dl, float pos) const {
        const std::size_t i0 = static_cast<std::size_t>(pos);
        const float frac = pos - static_cast<float>(i0);
        const std::size_t i1 = (i0 + 1) % kDelayLen;
        return dl[i0] + (dl[i1] - dl[i0]) * frac;
    }

    void process_flanger(float* interleaved, std::size_t frames) {
        const float rate = rate_.load(std::memory_order_relaxed);
        const float depth = depth_.load(std::memory_order_relaxed);
        const float mix = fx_mix_.load(std::memory_order_relaxed);
        const float fb = feedback_.load(std::memory_order_relaxed);
        const float inc = rate / fs_;
        const float hpf_coef = 1.0f - std::exp(-2.0f * 3.14159265f * 280.0f / fs_);

        for (std::size_t i = 0; i < frames; ++i) {
            const float lfo = lfo_value(phase_);
            // 0.5 ms di base, fino a 6.5 ms: la corsa del flanger classico.
            const float delay = (0.0005f + 0.006f * depth * lfo) * fs_;

            float pos = static_cast<float>(write_) - delay;
            while (pos < 0.0f) pos += static_cast<float>(kDelayLen);

            // Il wet passa-alto: il comb sulle basse e' quello che fa "scatola".
            hpf_lp_l_ += (read_delay(dl_l_, pos) - hpf_lp_l_) * hpf_coef;
            hpf_lp_r_ += (read_delay(dl_r_, pos) - hpf_lp_r_) * hpf_coef;
            const float yl = read_delay(dl_l_, pos) - hpf_lp_l_;
            const float yr = read_delay(dl_r_, pos) - hpf_lp_r_;

            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            // Il tanh nel percorso di retroazione tiene a bada l'autooscillazione
            // quando il feedback e' alto: un flanger che si incastra non serve.
            dl_l_[write_] = std::tanh(in_l + fb * yl);
            dl_r_[write_] = std::tanh(in_r + fb * yr);
            write_ = (write_ + 1) % kDelayLen;

            interleaved[i * 2 + 0] = in_l * (1.0f - mix) + yl * mix;
            interleaved[i * 2 + 1] = in_r * (1.0f - mix) + yr * mix;

            phase_ += inc;
            if (phase_ >= 1.0f) phase_ -= 1.0f;
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

    std::atomic<float> fx_mix_{0.5f};
    std::atomic<float> feedback_{0.35f};

    float fs_ = 48000.0f;
    float phase_ = 0.0f;

    float q_eff_ = 3.0f;
    float a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_l_ = 0.0f, ic2_l_ = 0.0f;
    float ic1_r_ = 0.0f, ic2_r_ = 0.0f;
    bool coef_valid_ = false;

    std::array<AllPass, 4> ap_l_{};
    std::array<AllPass, 4> ap_r_{};
    float fb_l_ = 0.0f;
    float fb_r_ = 0.0f;
    float hpf_lp_l_ = 0.0f;
    float hpf_lp_r_ = 0.0f;

    std::array<float, kDelayLen> dl_l_{};
    std::array<float, kDelayLen> dl_r_{};
    std::size_t write_ = 0;
};

}  // namespace namzero
