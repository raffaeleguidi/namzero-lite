#pragma once

// Rilevatore di intonazione per l'accordatore.
//
// Il thread audio non analizza nulla: copia il segnale DRY (input prima della
// catena) in un ring buffer lock-free con TunerTap. Un thread dedicato legge
// l'ultima finestra e chiama TunerDetector::analyze() ~15 volte al secondo.
//
// Il rilevatore e' una YIN: filtro anti-alias FIR e decimazione x4
// (48 kHz -> 12 kHz), funzione di differenza, cumulative mean normalized
// difference (CMNDF) e interpolazione parabolica del minimo.
//
// Perche' il filtro anti-alias: la sola media boxcar x4 non attenua le
// armoniche fra 6 e 12 kHz, che nei cantini sono molto energetiche e si
// ripiegano nella banda 0-6 kHz come componenti non armoniche: la CMNDF
// perde il minimo giusto e la nota sparisce o si sposta. Il FIR Blackman
// (63 tap, taglio ~3.6 kHz a 48 kHz) le elimina prima della decimazione.
//
// Il gate RMS e' adattivo e basso (pavimento 0.0003, circa -70 dBFS): i
// cantini sono piu' deboli dei bassi e decadono prima, con la vecchia soglia
// fissa a 0.0015 la nota spariva dopo poche centinaia di ms. In piu' la
// lettura resta agganciata per kHoldSeconds (600 ms) dopo che la confidenza
// scende, con la confidenza in fade: la nota e l'ago non saltano via al
// primo frame incerto. Una nuova nota forte sostituisce subito quella
// vecchia, una piu' debole si conferma in due analisi: l'hold non mostra la
// corda precedente quando se ne suona un'altra.
//
// Costo: ~0,9 ms per analisi su Pi Zero 2W (~1,3% di un core a 15 Hz),
// misurato con ./build/tuner_bench (G3 e Mi4). Il filtro calcola solo le
// uscite decimate e la funzione di differenza accumula in float (NEON):
// nonostante il FIR in piu' il totale e' un po' piu' basso di prima.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace namzero {

// Limiti delle opzioni dell'accordatore. Il riferimento e' la frequenza del La4
// "vero" dello strumento; transpose sposta solo la lettura della nota (non il
// suono) di -6..+6 semitoni.
//
// La specifica indicava 430..450 Hz, ma chiede anche che 415.3 Hz (La4 un
// semitono sotto) sia utilizzabile come alternativa a transpose=-1. Il limite
// inferiore e' quindi 415.0 Hz: 430..450 resta l'intervallo tipico e la UI lo
// mostra come campo 415.0..450.0 con passo 0.1.
constexpr double kTunerRefMin = 415.0;
constexpr double kTunerRefMax = 450.0;
constexpr double kTunerRefDefault = 440.0;
constexpr int kTunerTransposeMin = -6;
constexpr int kTunerTransposeMax = 6;

constexpr double kTunerMinHz = 50.0;    // sotto: non e' una corda di chitarra
constexpr double kTunerMaxHz = 1500.0;  // sopra: armoniche/rumore

// Riferimento effettivo: e' quello che il rilevatore usa per decidere quale
// nota sta suonando. Con transpose=-1 e reference 440 il La "vero" e' 415.3 Hz,
// quindi chi suona un semitono sotto continua a leggere A4.
inline double tuner_effective_reference(double reference_hz, int transpose) {
    const double ref = std::clamp(reference_hz, kTunerRefMin, kTunerRefMax);
    const int tr = std::clamp(transpose, kTunerTransposeMin, kTunerTransposeMax);
    return ref * std::pow(2.0, static_cast<double>(tr) / 12.0);
}

// Risultato grezzo del rilevatore: solo frequenza e confidenza. La nota e i
// cent dipendono da reference/transpose e si calcolano in build_state(), cosi'
// cambiare riferimento si vede subito senza aspettare una nuova analisi.
struct TunerPitch {
    bool voiced = false;
    double freq = 0.0;
    double confidence = 0.0;
    double rms = 0.0;
};

struct TunerNote {
    bool valid = false;
    std::string name;  // "A", "C#", ...
    int octave = 0;
    int midi = -1;
    double cents = 0.0;       // -50..+50
    double reference_hz = 0.0;  // riferimento effettivo usato
};

// Nota piu' vicina a freq, usando reference_hz e transpose. Per esempio con
// reference 440 e transpose 0: 440 -> A4 0 cent, 445 -> A4 +19.6 cent,
// 415.3 -> G#4 0 cent. Con transpose -1 (riferimento effettivo 415.3):
// 415.3 -> A4 0 cent, 440 -> A#4 0 cent.
inline TunerNote tuner_note_from_freq(double freq, double reference_hz, int transpose) {
    TunerNote out;
    out.reference_hz = tuner_effective_reference(reference_hz, transpose);
    if (!(freq > 0.0) || !(out.reference_hz > 0.0) || freq < kTunerMinHz ||
        freq > kTunerMaxHz) {
        return out;
    }
    const double semitones = 12.0 * std::log2(freq / out.reference_hz);
    const int nearest = static_cast<int>(std::lround(semitones));
    const int midi = 69 + nearest;  // A4 = MIDI 69
    if (midi < 0 || midi > 127) return out;
    static const char* kNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                   "F#", "G",  "G#", "A",  "A#", "B"};
    out.midi = midi;
    out.name = kNames[midi % 12];
    out.octave = midi / 12 - 1;
    const double target = out.reference_hz * std::pow(2.0, nearest / 12.0);
    out.cents = std::clamp(1200.0 * std::log2(freq / target), -50.0, 50.0);
    out.valid = true;
    return out;
}

// Ring buffer SPSC audio -> analisi. La scrittura avviene nel thread audio e
// non alloca ne' prende lock: ogni campione e' un atomic<float> relaxed, la
// testa viene pubblicata con release. Il lettore prende la testa con acquire e
// legge gli ultimi N campioni: non serve un mutex e non ci sono corse sui dati.
class TunerTap {
public:
    // 8192 campioni = 170 ms a 48 kHz: piu' del doppio della finestra, cosi' il
    // lettore trova sempre campioni completi anche se arriva in ritardo.
    static constexpr std::size_t kSize = 8192;
    static_assert((kSize & (kSize - 1)) == 0, "kSize deve essere una potenza di 2");
    static_assert(std::atomic<float>::is_always_lock_free,
                  "atomic<float> deve essere lock-free");

    TunerTap() {
        for (auto& s : samples_) s.store(0.0f, std::memory_order_relaxed);
    }

    // interleaved: stereo L,R,L,R...; si salva il mono (L+R)/2.
    void push(const float* interleaved, std::size_t frames) {
        std::uint32_t head = head_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < frames; ++i) {
            const float mono = 0.5f * (interleaved[2 * i] + interleaved[2 * i + 1]);
            samples_[(head + i) & (kSize - 1)].store(mono, std::memory_order_relaxed);
        }
        head_.store(head + static_cast<std::uint32_t>(frames),
                    std::memory_order_release);
    }

    // Copia gli ultimi n campioni (n <= kSize) in dst, dal piu' vecchio al
    // piu' recente. Chiamata solo dal thread di analisi.
    void read_latest(float* dst, std::size_t n) const {
        if (n > kSize) n = kSize;
        const std::uint32_t head = head_.load(std::memory_order_acquire);
        const std::uint32_t start = head - static_cast<std::uint32_t>(n);
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = samples_[(start + i) & (kSize - 1)].load(std::memory_order_relaxed);
        }
    }

private:
    std::array<std::atomic<float>, kSize> samples_;
    std::atomic<std::uint32_t> head_{0};
};

class TunerDetector {
public:
    static constexpr std::size_t kWindow = 4096;  // 85 ms a 48 kHz
    static constexpr std::size_t kDecim = 4;
    static constexpr std::size_t kMaxLag = 512;

    // Anti-alias: FIR Blackman simmetrica, 63 tap, taglio al 7.5% del rate
    // (~3.6 kHz a 48 kHz; stopband da ~5.9 kHz, cioe' sopra il Nyquist della
    // banda decimata). Si calcolano solo le uscite che servono (una ogni 4
    // campioni), quindi il costo e' 63 * 1009 moltiplicazioni per analisi.
    static constexpr std::size_t kAaTaps = 63;
    static constexpr std::size_t kAaCenter = (kAaTaps - 1) / 2;
    static constexpr double kAaCutoffFrac = 0.075;
    // Prima uscita decimata con tutti i tap dentro la finestra (`center` e'
    // 31, non multiplo di 4).
    static constexpr std::size_t kAaFirst = (kAaCenter + kDecim - 1) / kDecim;
    // Ultima uscita decimata: l'ultimo tap deve cadere entro kWindow-1.
    static constexpr std::size_t kDecValid =
        (kWindow - 1 - kAaCenter) / kDecim - kAaFirst + 1;

    // Gate: pavimento assoluto basso + rumore di fondo stimato. La stima sale
    // solo se il livello resta sotto kNoiseTrackMax, altrimenti in una stanza
    // rumorosa la soglia taglierebbe anche le note vere.
    static constexpr double kRmsFloor = 0.0003;   // ~-70 dBFS
    static constexpr double kNoiseGate = 2.0;     // 2x il rumore stimato
    static constexpr double kNoiseTrackMax = 0.002;  // sopra: non e' rumore
    static constexpr double kNoiseDownTau = 0.4;  // s, insegue il silenzio
    static constexpr double kNoiseUpTau = 3.0;    // s, impara il ronzio stabile

    static constexpr double kThreshold = 0.15;  // CMNDF: primo minimo valido
    static constexpr double kFallback = 0.45;   // oltre: niente nota
    static constexpr double kMinConfidence = 0.55;  // per una nota nuova fresca
    // Cambio nota: una nota diversa da quella agganciata deve essere forte
    // (kImmediateConfidence) oppure confermarsi per kConfirmFrames frame con
    // confidenza >= kConfirmConfidence. Serve a non scambiare per "nuova
    // corda" un frame di rumore nella coda del cantino, che ha poca energia e
    // puo' far scegliere alla YIN un lag sbagliato.
    static constexpr double kImmediateConfidence = 0.90;
    static constexpr double kConfirmConfidence = 0.75;
    static constexpr int kConfirmFrames = 2;
    static constexpr double kSamePitchRatio = 0.03;   // ~50 cent
    static constexpr double kSwitchRmsFrac = 0.03;    // -30 dB dal picco nota
    static constexpr double kPeakReleaseTau = 2.0;    // s

    // Hold: la lettura resta per 600 ms dopo che la confidenza scende, con
    // fade lineare. 400-700 ms e' il compromesso fra "non sparisce subito" e
    // "non mostra una nota vecchia".
    static constexpr double kHoldSeconds = 0.6;

    TunerDetector() = default;

    // hold=false serve ai test che vogliono misurare la sola detezione, senza
    // l'estensione artificiale della coda.
    explicit TunerDetector(bool hold) : hold_enabled_(hold) {}

    // Da chiamare quando l'accordatore viene spento: senza questo, riaccendendolo
    // in silenzio si vedrebbe per 600 ms l'ultima nota tenuta in memoria.
    void reset() {
        have_note_ = false;
        hold_elapsed_ = 0.0;
        held_freq_ = 0.0;
        held_conf_ = 0.0;
        noise_rms_ = 0.0;
        note_peak_rms_ = 0.0;
        pending_count_ = 0;
    }

    // dt e' il tempo dall'analisi precedente (per il fade dell'hold e per il
    // rumore di fondo). Default 1/15 s: il thread tuner gira a ~15 Hz.
    TunerPitch analyze(const float* x, std::size_t n, double rate,
                       double dt = 1.0 / 15.0) {
        TunerPitch out;
        if (x == nullptr || n < kWindow || rate <= 0.0) return out;
        if (!(dt > 0.0)) dt = 1.0 / 15.0;
        dt = std::clamp(dt, 0.001, 0.5);
        configure(rate);

        // Anti-alias FIR + decimazione x4. L'uscita i e' centrata sul campione
        // (kAaFirst+i)*kDecim: si usa la forma non causale, che e' simmetrica e
        // non introduce ritardo di gruppo (irrilevante per la YIN, ma comoda).
        double sum_sq = 0.0;
        for (std::size_t i = 0; i < kDecValid; ++i) {
            const float* p = x + (kAaFirst + i) * kDecim - kAaCenter;
            float acc = 0.0f;
            for (std::size_t k = 0; k < kAaTaps; ++k) {
                acc += aa_[k] * p[k];
            }
            dec_[i] = acc;
            sum_sq += static_cast<double>(acc) * acc;
        }
        out.rms = std::sqrt(sum_sq / static_cast<double>(kDecValid));

        const double gate = std::max(kRmsFloor, kNoiseGate * noise_rms_);
        if (out.rms < gate) {
            update_noise(out.rms, false, dt);
            hold_or_publish(out, false, 0.0, 0.0, dt);
            return out;
        }

        // Toglie la continua: un offset di ingresso falserebbe il minimo.
        double mean = 0.0;
        for (std::size_t i = 0; i < kDecValid; ++i) mean += dec_[i];
        mean /= static_cast<double>(kDecValid);
        for (std::size_t i = 0; i < kDecValid; ++i) {
            dec_[i] = static_cast<float>(dec_[i] - mean);
        }

        const double dr = rate / static_cast<double>(kDecim);
        std::size_t min_lag = static_cast<std::size_t>(std::floor(dr / kTunerMaxHz));
        if (min_lag < 1) min_lag = 1;
        std::size_t max_lag = static_cast<std::size_t>(std::ceil(dr / kTunerMinHz));
        max_lag = std::min(max_lag, kMaxLag);
        if (max_lag <= min_lag + 2 || max_lag + 1 >= kDecValid) {
            update_noise(out.rms, false, dt);
            hold_or_publish(out, false, 0.0, 0.0, dt);
            return out;
        }

        // Funzione di differenza. Si usano kDecValid - max_lag campioni per
        // ogni lag, cosi' tutti i lag guardano la stessa porzione di segnale.
        const std::size_t w = kDecValid - max_lag;
        for (std::size_t tau = 0; tau <= max_lag; ++tau) {
            float sum = 0.0f;
            const float* a = dec_.data();
            const float* b = dec_.data() + tau;
            for (std::size_t i = 0; i < w; ++i) {
                const float d = a[i] - b[i];
                sum += d * d;
            }
            diff_[tau] = sum;
        }
        double running = 0.0;
        cmndf_[0] = 1.0;
        for (std::size_t tau = 1; tau <= max_lag; ++tau) {
            running += diff_[tau];
            cmndf_[tau] = running > 1e-12 ? diff_[tau] * tau / running : 1.0;
        }

        // Primo minimo locale sotto soglia (YIN). Se non c'e', si tiene il
        // minimo globale solo se abbastanza netto, altrimenti niente nota.
        std::size_t best = min_lag;
        double best_val = cmndf_[min_lag];
        for (std::size_t tau = min_lag; tau <= max_lag; ++tau) {
            const double v = cmndf_[tau];
            if (v < best_val) {
                best_val = v;
                best = tau;
            }
            if (v < kThreshold && tau > min_lag && v <= cmndf_[tau - 1] &&
                (tau == max_lag || v <= cmndf_[tau + 1])) {
                best_val = v;
                best = tau;
                break;
            }
        }
        if (best_val > kFallback) {
            update_noise(out.rms, false, dt);
            hold_or_publish(out, false, 0.0, 0.0, dt);
            return out;
        }

        double shift = 0.0;
        if (best > min_lag && best < max_lag) {
            const double a = diff_[best - 1];
            const double b = diff_[best];
            const double c = diff_[best + 1];
            const double denom = a - 2.0 * b + c;
            if (std::fabs(denom) > 1e-12) shift = 0.5 * (a - c) / denom;
            shift = std::clamp(shift, -0.5, 0.5);
        }
        const double period = static_cast<double>(best) + shift;
        if (period <= 0.0) {
            update_noise(out.rms, false, dt);
            hold_or_publish(out, false, 0.0, 0.0, dt);
            return out;
        }
        const double freq = dr / period;
        const double confidence = std::clamp(1.0 - best_val, 0.0, 1.0);
        const bool fresh = freq >= kTunerMinHz && freq <= kTunerMaxHz;
        update_noise(out.rms, fresh && out.rms >= kNoiseTrackMax, dt);
        hold_or_publish(out, fresh, freq, confidence, dt);
        return out;
    }

private:
    // Coefficienti del FIR in ordine "forward" rispetto al buffer p[] usato in
    // analyze(): aa_[k] = h[kAaTaps-1-k], cosi' il ciclo interno legge in
    // avanti ed e' piu' facile da vettorizzare.
    void configure(double rate) {
        if (rate == cfg_rate_) return;
        cfg_rate_ = rate;
        constexpr double kPi = 3.14159265358979323846;
        const double fc = kAaCutoffFrac * rate;
        const double center = static_cast<double>(kAaCenter);
        std::array<float, kAaTaps> h{};
        double sum = 0.0;
        for (std::size_t k = 0; k < kAaTaps; ++k) {
            const double t = static_cast<double>(k) - center;
            double s = 1.0;
            if (std::fabs(t) > 1e-12) {
                const double x = 2.0 * kPi * fc * t / rate;
                s = std::sin(x) / x;
            }
            const double ph = 2.0 * kPi * static_cast<double>(k) /
                              static_cast<double>(kAaTaps - 1);
            const double window = 0.42 - 0.5 * std::cos(ph) + 0.08 * std::cos(2.0 * ph);
            h[k] = 2.0 * fc / rate * s * window;
            sum += h[k];
        }
        for (std::size_t k = 0; k < kAaTaps; ++k) {
            aa_[k] = h[kAaTaps - 1 - k] / sum;
        }
    }

    // Stima del rumore di fondo: scende in fretta verso il silenzio, sale
    // piano e solo quando il livello e' abbastanza basso da essere rumore.
    // Anche un segnale "voiced" ma molto basso (es. ronzio di rete stabile)
    // entra nella stima: altrimenti resterebbe una nota fantasma per sempre.
    // Una nota vera e forte (sopra kNoiseTrackMax) non tocca la stima.
    void update_noise(double rms, bool voiced, double dt) {
        if (voiced) return;
        if (rms < noise_rms_) {
            noise_rms_ += (rms - noise_rms_) * std::min(1.0, dt / kNoiseDownTau);
        } else if (rms < kNoiseTrackMax) {
            noise_rms_ += (rms - noise_rms_) * std::min(1.0, dt / kNoiseUpTau);
        }
    }

    // Se la detezione e' fresca e abbastanza sicura, pubblica la nuova nota.
    // Altrimenti tiene l'ultima per kHoldSeconds con confidenza in fade.
    // Una nuova nota forte sostituisce subito la precedente; una piu' debole
    // deve confermarsi un paio di frame, cosi' la coda rumorosa del cantino
    // non fa saltare l'ago su una nota fantasma. L'hold non sopravvive al
    // cambio di corda perche' una nuova nota confermata lo azzera.
    void hold_or_publish(TunerPitch& out, bool fresh, double freq,
                         double confidence, double dt) {
        if (note_peak_rms_ > 0.0) {
            note_peak_rms_ *= std::exp(-dt / kPeakReleaseTau);
        }
        if (fresh && confidence >= kMinConfidence) {
            const bool significant_level =
                note_peak_rms_ <= 0.0 || out.rms >= kSwitchRmsFrac * note_peak_rms_;
            if (!have_note_) {
                const int count = confirm(freq);
                if (confidence >= kImmediateConfidence || count >= kConfirmFrames) {
                    accept(out, freq, confidence);
                    return;
                }
            } else {
                const double ratio = freq / held_freq_;
                if (std::fabs(ratio - 1.0) <= kSamePitchRatio) {
                    // Stessa nota: si aggiorna la frequenza (l'ago segue) e si
                    // tiene la confidenza migliore vista, cosi' un frame
                    // rumoroso non abbassa la soglia per il cambio nota.
                    held_freq_ = freq;
                    held_conf_ = std::max(held_conf_, confidence);
                    hold_elapsed_ = 0.0;
                    pending_count_ = 0;
                    out.voiced = true;
                    out.freq = freq;
                    out.confidence = confidence;
                    return;
                }
                if (significant_level) {
                    const bool strong = confidence >= kImmediateConfidence;
                    const int count = confirm(freq);
                    if (strong ||
                        (count >= kConfirmFrames &&
                         confidence >= kConfirmConfidence)) {
                        accept(out, freq, confidence);
                        return;
                    }
                }
            }
        }
        if (hold_enabled_ && have_note_) {
            hold_elapsed_ += dt;
            const double fade = 1.0 - hold_elapsed_ / kHoldSeconds;
            if (fade > 0.0) {
                out.voiced = true;
                out.freq = held_freq_;
                out.confidence = held_conf_ * fade;
                return;
            }
            have_note_ = false;
            note_peak_rms_ = 0.0;
        }
        pending_count_ = 0;
        out.voiced = false;
        out.freq = 0.0;
        out.confidence = 0.0;
    }

    // Conta i frame consecutivi in cui la stessa candidata (entro il 3%) e'
    // stata proposta. Un cambio corda vero resta stabile, il rumore no.
    int confirm(double freq) {
        if (pending_count_ > 0 &&
            std::fabs(freq / pending_freq_ - 1.0) <= kSamePitchRatio) {
            ++pending_count_;
        } else {
            pending_freq_ = freq;
            pending_count_ = 1;
        }
        return pending_count_;
    }

    void accept(TunerPitch& out, double freq, double confidence) {
        have_note_ = true;
        hold_elapsed_ = 0.0;
        held_freq_ = freq;
        held_conf_ = confidence;
        note_peak_rms_ = std::max(note_peak_rms_, out.rms);
        pending_count_ = 0;
        out.voiced = true;
        out.freq = freq;
        out.confidence = confidence;
    }

    std::array<float, kAaTaps> aa_{};
    std::array<float, kDecValid> dec_{};
    std::array<double, kMaxLag + 2> diff_{};
    std::array<double, kMaxLag + 2> cmndf_{};
    double cfg_rate_ = 0.0;
    double noise_rms_ = 0.0;
    double note_peak_rms_ = 0.0;
    bool hold_enabled_ = true;
    bool have_note_ = false;
    double hold_elapsed_ = 0.0;
    double held_freq_ = 0.0;
    double held_conf_ = 0.0;
    double pending_freq_ = 0.0;
    int pending_count_ = 0;
};

}  // namespace namzero
