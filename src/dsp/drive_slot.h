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
//           Ha un low-shelf in uscita perche' senza basse il clipping suona
//           sottile, piu' "zanzara" che fuzz.
//
//   klon  — ispirato al Klon Centaur: guadagno piu' basso, pre-enfasi sulle
//           alte (il "treble bleed" del pedale) e clipping a diodi asimmetrico,
//           ma soprattutto un blend con il segnale pulito. E' quel blend che
//           rende il pedale trasparente: il clipping c'e', ma non mangia il
//           carattere della chitarra. In piu' ha la gobba sulle medie che e' il
//           segno del Klon: senza, risulta piatto e "piu' piano" dell'ampli.
//
// Stanno nello stesso slot: si cambia modello senza toccare la catena.
class DriveSlot final : public Slot {
public:
    enum class Type { kFuzz, kKlon, kTubescreamer };

    void set_type(Type t) { type_.store(t, std::memory_order_relaxed); }
    Type type() const { return type_.load(std::memory_order_relaxed); }

    void set_drive(float d) { drive_.store(clamp(d, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_tone(float t) { tone_.store(clamp(t, 0.0f, 1.0f), std::memory_order_relaxed); }
    void set_level_db(float db) { level_db_.store(db, std::memory_order_relaxed); }
    // Solo klon: quota di segnale saturato nel mix (0 = solo pulito, 1 = solo
    // clipping). Il Klon vero sta verso il centro.
    void set_blend(float b) { blend_.store(clamp(b, 0.0f, 1.0f), std::memory_order_relaxed); }
    // Gobba sulle medie del Klon (default +4 dB a 800 Hz).
    void set_mid_db(float db) { mid_db_.store(clamp(db, -12.0f, 12.0f), std::memory_order_relaxed); }
    // Basse del fuzz: low-shelf a 110 Hz (default +5 dB).
    void set_low_db(float db) { low_db_.store(clamp(db, -12.0f, 12.0f), std::memory_order_relaxed); }
    // Ottava del fuzz (stile Octavia): raddrizza il fuzz a onda intera, toglie
    // la continua con un passa-alto e lo miscela al fuzz. 0 = solo fuzz,
    // 1 = solo ottava. Significativo solo per kFuzz: klon e tubescreamer lo
    // ignorano (il valore resta comunque salvato nel preset).
    //
    // Il mix (set_octave) e l'interruttore (set_octave_on) sono indipendenti:
    // a interruttore spento il ramo e' bypassato ma il mix resta memorizzato,
    // cosi' riaccendendo si ritrova la manopola com'era.
    void set_octave(float o) {
        const float v = std::isfinite(o) ? clamp(o, 0.0f, 1.0f) : 0.0f;
        octave_.store(v, std::memory_order_relaxed);
    }
    void set_octave_on(bool on) { octave_on_.store(on, std::memory_order_relaxed); }

    float drive() const { return drive_.load(std::memory_order_relaxed); }
    float tone() const { return tone_.load(std::memory_order_relaxed); }
    float level_db() const { return level_db_.load(std::memory_order_relaxed); }
    float blend() const { return blend_.load(std::memory_order_relaxed); }
    float mid_db() const { return mid_db_.load(std::memory_order_relaxed); }
    float low_db() const { return low_db_.load(std::memory_order_relaxed); }
    float octave() const { return octave_.load(std::memory_order_relaxed); }
    bool octave_on() const { return octave_on_.load(std::memory_order_relaxed); }

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        fs_ = static_cast<float>(sample_rate);
        reset();
    }

    void reset() override {
        lp_l_ = lp_r_ = 0.0f;
        pre_lp_l_ = pre_lp_r_ = 0.0f;
        hp_lp_l_ = hp_lp_r_ = 0.0f;
        dc_x_l_ = dc_x_r_ = 0.0f;
        dc_y_l_ = dc_y_r_ = 0.0f;
        oct_dc_l_ = oct_dc_r_ = 0.0f;
        // All'accensione l'ottava parte gia' al valore del preset: il fade-in
        // dello smussamento serve ai cambi a runtime, non all'avvio. Se
        // l'interruttore e' spento si parte da 0 anche col mix salvato.
        oct_smooth_ = octave_on_.load(std::memory_order_relaxed)
                          ? octave_.load(std::memory_order_relaxed)
                          : 0.0f;
        mid_l_.reset();
        mid_r_.reset();
        low_l_.reset();
        low_r_.reset();
        update_coeffs();
    }

    const char* name() const override { return "drive"; }

    void process(float* interleaved, std::size_t frames) override {
        update_coeffs();
        switch (type_.load(std::memory_order_relaxed)) {
            case Type::kKlon: process_klon(interleaved, frames); break;
            case Type::kTubescreamer: process_tubescreamer(interleaved, frames); break;
            default: process_fuzz(interleaved, frames); break;
        }
    }

private:
    static float clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static float db_to_lin(float db) { return std::pow(10.0f, db / 20.0f); }

    // Biquad RBJ: serve solo per due filtri fissi (gobba del Klon, basse del
    // fuzz), quindi niente libreria.
    struct Biquad {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

        void reset() { x1 = x2 = y1 = y2 = 0.0f; }

        // Aggiorna i soli coefficienti: lo stato del filtro resta, altrimenti a
        // ogni blocco si azzererebbe la coda e si sentirebbe un click.
        void coefficients(const Biquad& o) {
            b0 = o.b0;
            b1 = o.b1;
            b2 = o.b2;
            a1 = o.a1;
            a2 = o.a2;
        }

        void set(float nb0, float nb1, float nb2, float na1, float na2) {
            b0 = nb0;
            b1 = nb1;
            b2 = nb2;
            a1 = na1;
            a2 = na2;
        }

        float process(float x) {
            const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            return y;
        }
    };

    // Coefficienti gia' normalizzati per a0.
    static void peaking(Biquad& q, float fs, float fc, float qq, float db) {
        const float a = std::pow(10.0f, db / 40.0f);
        const float w0 = 2.0f * 3.14159265f * fc / fs;
        const float c = std::cos(w0);
        const float alpha = std::sin(w0) / (2.0f * qq);
        const float a0 = 1.0f + alpha / a;
        q.set((1.0f + alpha * a) / a0, (-2.0f * c) / a0, (1.0f - alpha * a) / a0,
              (-2.0f * c) / a0, (1.0f - alpha / a) / a0);
    }

    static void low_shelf(Biquad& bq, float fs, float fc, float db) {
        const float a = std::pow(10.0f, db / 40.0f);
        const float w0 = 2.0f * 3.14159265f * fc / fs;
        const float c = std::cos(w0);
        const float s = std::sin(w0);
        const float alpha = s * 0.70710678f;  // S = 1
        const float beta = 2.0f * std::sqrt(a) * alpha;
        const float a0 = (a + 1.0f) + (a - 1.0f) * c + beta;
        bq.set((a * ((a + 1.0f) - (a - 1.0f) * c + beta)) / a0,
               (2.0f * a * ((a - 1.0f) - (a + 1.0f) * c)) / a0,
               (a * ((a + 1.0f) - (a - 1.0f) * c - beta)) / a0,
               (-2.0f * ((a - 1.0f) + (a + 1.0f) * c)) / a0,
               ((a + 1.0f) + (a - 1.0f) * c - beta) / a0);
    }

    void update_coeffs() {
        const Type type = type_.load(std::memory_order_relaxed);
        const bool klon = (type == Type::kKlon);
        const float t = tone_.load(std::memory_order_relaxed);
        // Il fuzz apre fino a 8 kHz, il Klon resta piu' chiuso, il Tube Screamer
        // e' il piu' scuro dei tre: il suo tono taglia, non apre.
        float cutoff = 800.0f * std::pow(10.0f, t);
        if (klon) cutoff = 1200.0f * std::pow(8.0f, t);
        else if (type == Type::kTubescreamer) cutoff = 700.0f * std::pow(6.0f, t);
        tone_coef_ = 1.0f - std::exp(-2.0f * 3.14159265f * cutoff / fs_);

        // Ottava del fuzz: passa-alto a 200 Hz per togliere la continua che
        // crea il raddrizzamento. E' un one-pole: costa 3 operazioni per
        // campione e per canale. Lo smussamento (~8 ms) evita il click quando
        // si muove la manopola o si richiama un preset.
        oct_hp_coef_ = 1.0f - std::exp(-2.0f * 3.14159265f * 200.0f / fs_);
        oct_smooth_coef_ = 1.0f - std::exp(-1.0f / (0.008f * fs_));

        // I filtri fissi non si accendono insieme: si calcola solo quello del
        // modello in uso (un sin/cos per blocco, non per campione).
        if (klon) {
            Biquad proto;
            peaking(proto, fs_, 800.0f, 1.0f, mid_db_.load(std::memory_order_relaxed));
            mid_l_.coefficients(proto);
            mid_r_.coefficients(proto);
        } else {
            Biquad proto;
            low_shelf(proto, fs_, 110.0f, low_db_.load(std::memory_order_relaxed));
            low_l_.coefficients(proto);
            low_r_.coefficients(proto);
        }
    }

    void process_fuzz(float* interleaved, std::size_t frames) {
        const float drive = drive_.load(std::memory_order_relaxed);
        // 1x .. 31x: il boost pulito sta in basso, il fuzz in alto.
        const float pre = 1.0f + drive * 30.0f;
        const float level = db_to_lin(level_db_.load(std::memory_order_relaxed));
        // Compensa il guadagno: senza, alzare il drive fa solo piu' volume.
        const float norm = level / std::tanh(pre);
        // L'interruttore decide se il ramo esiste, il mix quanto pesa. Con
        // l'interruttore spento il target e' 0 anche se il mix salvato e' 1:
        // cosi' lo smussamento fa il fade-out (~8 ms) senza click e poi il
        // ramo si salta del tutto. Il mix resta intatto nello slot.
        const bool enabled = octave_on_.load(std::memory_order_relaxed);
        const float oct_target = enabled ? octave_.load(std::memory_order_relaxed) : 0.0f;
        // A target 0 e smussamento esaurito non si paga nulla: il ramo si
        // salta del tutto. Appena il target sale, la rampa riparte da zero.
        const bool oct_on = oct_target > 0.0f || oct_smooth_ > 1e-4f;
        if (!oct_on) {
            oct_smooth_ = 0.0f;
            oct_dc_l_ = oct_dc_r_ = 0.0f;
        }

        for (std::size_t i = 0; i < frames; ++i) {
            const float l = std::tanh(pre * interleaved[i * 2 + 0]) * norm;
            const float r = std::tanh(pre * interleaved[i * 2 + 1]) * norm;

            lp_l_ += (l - lp_l_) * tone_coef_;
            lp_r_ += (r - lp_r_) * tone_coef_;

            float y_l = lp_l_;
            float y_r = lp_r_;
            if (oct_on) {
                // Un solo smussamento per frame, uguale sui due canali: il
                // crossfade fuzz/ottava non deve creare scalini.
                oct_smooth_ += (oct_target - oct_smooth_) * oct_smooth_coef_;
                const float o = oct_smooth_;
                const float a_l = std::fabs(y_l);
                const float a_r = std::fabs(y_r);
                oct_dc_l_ += (a_l - oct_dc_l_) * oct_hp_coef_;
                oct_dc_r_ += (a_r - oct_dc_r_) * oct_hp_coef_;
                // Raddrizzamento a onda intera meno la continua: del fuzz
                // resta l'armonica a 2f (l'ottava), non un pitch shift.
                y_l += (a_l - oct_dc_l_ - y_l) * o;
                y_r += (a_r - oct_dc_r_ - y_r) * o;
            }

            // Low-shelf in uscita: ridona peso alle basse che il clipping
            // sottrae, senza rimandarle dentro la saturazione (farebbe fango).
            interleaved[i * 2 + 0] = low_l_.process(y_l);
            interleaved[i * 2 + 1] = low_r_.process(y_r);
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

    // Tube Screamer: il passa-alto prima del clipping e' quello che lo rende
    // "stretto" e con la gobba sulle medie; il tono dopo taglia le alte.
    // Il clipping e' simmetrico (due diodi in controfase), quindi armoniche
    // dispari: piu' "canto" che "gracchio".
    void process_tubescreamer(float* interleaved, std::size_t frames) {
        const float drive = drive_.load(std::memory_order_relaxed);
        const float pre = 1.0f + drive * 30.0f;
        const float level = db_to_lin(level_db_.load(std::memory_order_relaxed));
        const float norm = level / std::tanh(pre);
        // 720 Hz: sotto quella frequenza il pedale non manda nulla nel clipping.
        const float hp_coef = 1.0f - std::exp(-2.0f * 3.14159265f * 720.0f / fs_);

        for (std::size_t i = 0; i < frames; ++i) {
            const float in_l = interleaved[i * 2 + 0];
            const float in_r = interleaved[i * 2 + 1];

            hp_lp_l_ += (in_l - hp_lp_l_) * hp_coef;
            hp_lp_r_ += (in_r - hp_lp_r_) * hp_coef;
            const float v_l = pre * (in_l - hp_lp_l_);
            const float v_r = pre * (in_r - hp_lp_r_);

            const float y_l = std::tanh(v_l) * norm;
            const float y_r = std::tanh(v_r) * norm;

            lp_l_ += (y_l - lp_l_) * tone_coef_;
            lp_r_ += (y_r - lp_r_) * tone_coef_;

            interleaved[i * 2 + 0] = lp_l_;
            interleaved[i * 2 + 1] = lp_r_;
        }
    }

    void process_klon(float* interleaved, std::size_t frames) {
        const float drive = drive_.load(std::memory_order_relaxed);
        const float blend = blend_.load(std::memory_order_relaxed);
        const float clean = 1.0f - blend;

        // Guadagno invariato: il boost lo danno la gobba sulle medie e il
        // makeup in uscita. Alzare il pre-guadagno renderebbe il pedale un
        // fuzz, che non e' quello che deve fare.
        const float pre = 1.0f + drive * 12.0f;
        // Pre-enfasi: il pedale manda piu' alte nel clipping e le riprende dopo.
        const float emph = 1.2f * drive;
        // +3 dB fissi: il Klon vero ha un buffer di uscita che spinge, e senza
        // questo il pedale sembrava abbassare il volume dell'ampli.
        constexpr float kMakeup = 1.4125f;  // +3 dB
        const float level = db_to_lin(level_db_.load(std::memory_order_relaxed)) * kMakeup;

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

            // Gobba sulle medie: e' il carattere del Klon, e senza il pedale
            // risulta piatto e piu' debole dell'ampli.
            interleaved[i * 2 + 0] = mid_l_.process(lp_l_);
            interleaved[i * 2 + 1] = mid_r_.process(lp_r_);
        }
    }

    std::atomic<Type> type_{Type::kFuzz};
    std::atomic<float> drive_{0.35f};
    std::atomic<float> tone_{0.5f};
    std::atomic<float> level_db_{0.0f};
    std::atomic<float> blend_{0.40f};
    std::atomic<float> mid_db_{4.0f};
    std::atomic<float> low_db_{5.0f};
    std::atomic<float> octave_{0.0f};
    std::atomic<bool> octave_on_{false};

    float fs_ = 48000.0f;
    float tone_coef_ = 0.2f;
    float oct_hp_coef_ = 0.026f;
    float oct_smooth_coef_ = 0.0026f;
    float lp_l_ = 0.0f;
    float lp_r_ = 0.0f;
    float pre_lp_l_ = 0.0f;
    float pre_lp_r_ = 0.0f;
    float hp_lp_l_ = 0.0f;
    float hp_lp_r_ = 0.0f;
    float dc_x_l_ = 0.0f;
    float dc_x_r_ = 0.0f;
    float dc_y_l_ = 0.0f;
    float dc_y_r_ = 0.0f;
    float oct_smooth_ = 0.0f;
    float oct_dc_l_ = 0.0f;
    float oct_dc_r_ = 0.0f;
    Biquad mid_l_, mid_r_, low_l_, low_r_;
};

}  // namespace namzero
