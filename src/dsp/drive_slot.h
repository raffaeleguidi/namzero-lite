#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "dsp/slot.h"

namespace namzero {

// Slot drive, con due modelli:
//
//   fuzz  — il boost/overdrive storico: pre-guadagno alto, tanh, filtro di tono.
//           Spinge presto e "sporca": su un ampli gia' carico suona da fuzz.
//
//   klon  — ispirato al Klon Centaur: guadagno piu' basso, pre-enfasi sulle
//           alte (il "treble bleed" del pedale) e clipping a diodi asimmetrico,
//           ma soprattutto un blend con il segnale pulito. E' quel blend che
//           rende il pedale trasparente: il clipping c'e', ma non mangia il
//           carattere della chitarra.
//
// Stanno nello stesso slot: si cambia modello senza toccare la catena.
class DriveSlot final : public Slot {
public:
    enum class Type { kFuzz, kKlon };

    void set_type(Type t) { type_.store(t, std::memory_order_relaxed); }
    Type type() const { return type_.load(std::memory_order_relaxed); }

    void set_drive(float d) { drive_.store(clamp(d, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_tone(float t) { tone_.store(clamp(t, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_level_db(float db) { level_db_.store(db, std::memory_order_relaxed); }
    // Solo klon: quota di segnale saturato nel mix (0 = solo pulito, 1 = solo
    // clipping). Il Klon vero sta verso il centro.
    void set_blend(float b) { blend_.store(clamp(b, 0.0f, 1.0f), std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        lp_l_ = lp_r_ = 0.0f;
        pre_lp_l_ = pre_lp_r_ = 0.0f;
        dc_x_l_ = dc_x_r_ = 0.0f;
        dc_y_l_ = dc_y_r_ = 0.0f;
        update_coeffs();
    }

    const char* name() const override { return "drive"; }

    void process(float* interleaved, std::size_t frames) override {
        update_coeffs();
        if (type_.load(std::memory_order_relaxed) == Type::kKlon) {
            process_klon(interleaved, frames);
        } else {
            process_fuzz(interleaved, frames);
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

    void update_coeffs() {
        const float t = tone_.load(std::memory_order_relaxed);
        // Il fuzz apre fino a 8 kHz, il Klon resta piu' chiuso: e' un pedale da
        // "piu' di tutto", non da bright.
        const float cutoff = (type_.load(std::memory_order_relaxed) == Type::kKlon)
                                 ? 1200.0f * std::pow(8.0f, t)
                                 : 800.0f * std::pow(10.0f, t);
        tone_coef_ = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff / fs_);
    }

    void process_fuzz(float* interleaved, std::size_t frames) {
        const float drive = drive_.load(std::memory_order_relaxed);
        // 1x .. 31x: il boost pulito sta in basso, il fuzz in alto.
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

    // Diodo al germanio semplificato, con ginocchio: sotto la soglia il diodo
    // non conduce e il segnale passa lineare, sopra la curva comprime. Le due
    // soglie sono diverse (asimmetria), da cui le armoniche pari del Klon.
    // Il ginocchio e' quello che rende il pedale pulito a guadagno basso: senza,
    // anche a riposo il tanh colorerebbe il segnale.
    static float knee_clip(float x, float knee) {
        if (x <= knee) return x;
        const float over = x - knee;
        return knee + (1.0f - knee) * std::tanh(over / (1.0f - knee));
    }

    static float clip_diode(float x) {
        constexpr float kKneePos = 0.50f;
        constexpr float kKneeNeg = 0.38f;
        return x >= 0.0f ? knee_clip(x, kKneePos) : -knee_clip(-x, kKneeNeg);
    }

    void process_klon(float* interleaved, std::size_t frames) {
        const float drive = drive_.load(std::memory_order_relaxed);
        const float blend = blend_.load(std::memory_order_relaxed);
        const float clean = 1.0f - blend;

        // Meno guadagno del fuzz: il Klon e' un boost/OD, non un distorsore.
        const float pre = 1.0f + drive * 12.0f;
        // Pre-enfasi: il pedale manda piu' alte nel clipping e le riprende dopo.
        const float emph = 1.2f * drive;
        const float level = db_to_lin(level_db_.load(std::memory_order_relaxed));
        // Nessuna compensazione automatica del guadagno: come sul fuzz il
        // livello lo fa level_db. Compensare qui attenuerebbe anche il segnale
        // pulito del blend, che deve restare intatto quando blend = 0.

        // Passa-alto della pre-enfasi (taglio a ~700 Hz) e DC blocker in uscita:
        // il clipping asimmetrico genera continua, che andrebbe ad abbassare il
        // punto di lavoro dei blocchi successivi.
        const float hp_coef = 1.0f - std::exp(-2.0f * 3.14159265f * 700.0f / fs_);
        const float dc_r = 0.995f;

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            pre_lp_l_ += (in_l - pre_lp_l_) * hp_coef;
            pre_lp_r_ += (in_r - pre_lp_r_) * hp_coef;
            const float v_l = pre * (in_l + emph * (in_l - pre_lp_l_));
            const float v_r = pre * (in_r + emph * (in_r - pre_lp_r_));

            const float y_l = clean * in_l + blend * clip_diode(v_l);
            const float y_r = clean * in_r + blend * clip_diode(v_r);

            // DC blocker: y[n] = x[n] - x[n-1] + r*y[n-1]
            const float d_l = y_l - dc_x_l_ + dc_r * dc_y_l_;
            const float d_r = y_r - dc_x_r_ + dc_r * dc_y_r_;
            dc_x_l_ = y_l;
            dc_x_r_ = y_r;
            dc_y_l_ = d_l;
            dc_y_r_ = d_r;

            lp_l_ += (d_l * level - lp_l_) * tone_coef_;
            lp_r_ += (d_r * level - lp_r_) * tone_coef_;

            interleaved[i * 2 + 0] = lp_l_;
            interleaved[i * 2 + 1] = lp_r_;
        }
    }

    std::atomic<Type> type_{Type::kFuzz};
    std::atomic<float> drive_{0.35f};
    std::atomic<float> tone_{0.5f};
    std::atomic<float> level_db_{0.0f};
    std::atomic<float> blend_{0.40f};

    float fs_ = 48000.0f;
    float tone_coef_ = 0.2f;
    float lp_l_ = 0.0f;
    float lp_r_ = 0.0f;
    float pre_lp_l_ = 0.0f;
    float pre_lp_r_ = 0.0f;
    float dc_x_l_ = 0.0f;
    float dc_x_r_ = 0.0f;
    float dc_y_l_ = 0.0f;
    float dc_y_r_ = 0.0f;
};

}  // namespace namzero
