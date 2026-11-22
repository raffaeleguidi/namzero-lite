#pragma once

// Rilevatore di intonazione per l'accordatore.
//
// Il thread audio non analizza nulla: copia il segnale DRY (input prima della
// catena) in un ring buffer lock-free con TunerTap. Un thread dedicato legge
// l'ultima finestra e chiama TunerDetector::analyze() ~15 volte al secondo.
//
// Il rilevatore e' una YIN semplificata: decimazione x4 (48 kHz -> 12 kHz),
// funzione di differenza, cumulative mean normalized difference (CMNDF) e
// interpolazione parabolica del minimo. Costa poche centinaia di migliaia di
// operazioni per analisi, quindi resta sotto l'1% di CPU su un Pi Zero 2W.

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
    static constexpr std::size_t kDecWindow = kWindow / kDecim;  // 1024 @ 12 kHz
    static constexpr std::size_t kMaxLag = 512;
    static constexpr double kRmsMin = 0.0015;  // circa -56 dBFS: sotto = silenzio
    static constexpr double kThreshold = 0.15;   // CMNDF: primo minimo valido
    static constexpr double kFallback = 0.45;    // oltre: niente nota

    TunerPitch analyze(const float* x, std::size_t n, double rate) {
        TunerPitch out;
        if (x == nullptr || n < kWindow || rate <= 0.0) return out;

        // Decimazione boxcar x4: 48 kHz -> 12 kHz. Il filtro ha zeri a 12k,
        // 24k, ... e attenua abbastanza le armoniche alte per la YIN.
        double sum_sq = 0.0;
        for (std::size_t i = 0; i < kDecWindow; ++i) {
            const float* p = x + i * kDecim;
            float v = 0.0f;
            for (std::size_t j = 0; j < kDecim; ++j) v += p[j];
            v *= 1.0f / static_cast<float>(kDecim);
            dec_[i] = v;
            sum_sq += static_cast<double>(v) * v;
        }
        out.rms = std::sqrt(sum_sq / static_cast<double>(kDecWindow));
        if (out.rms < kRmsMin) return out;

        // Toglie la continua: un offset di ingresso falserebbe il minimo.
        double mean = 0.0;
        for (float v : dec_) mean += v;
        mean /= static_cast<double>(kDecWindow);
        for (float& v : dec_) v = static_cast<float>(v - mean);

        const double dr = rate / static_cast<double>(kDecim);
        std::size_t min_lag = static_cast<std::size_t>(std::floor(dr / kTunerMaxHz));
        if (min_lag < 1) min_lag = 1;
        std::size_t max_lag = static_cast<std::size_t>(std::ceil(dr / kTunerMinHz));
        max_lag = std::min(max_lag, kMaxLag);
        if (max_lag <= min_lag + 2 || max_lag >= kDecWindow) return out;

        // Funzione di differenza. Si usano kDecWindow - max_lag campioni per
        // ogni lag, cosi' tutti i lag guardano la stessa porzione di segnale.
        const std::size_t w = kDecWindow - max_lag;
        for (std::size_t tau = 0; tau <= max_lag; ++tau) {
            double sum = 0.0;
            const float* a = dec_.data();
            const float* b = dec_.data() + tau;
            for (std::size_t i = 0; i < w; ++i) {
                const double d = static_cast<double>(a[i]) - b[i];
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
        if (best_val > kFallback) return out;

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
        if (period <= 0.0) return out;
        out.freq = dr / period;
        out.confidence = std::clamp(1.0 - best_val, 0.0, 1.0);
        out.voiced = out.freq >= kTunerMinHz && out.freq <= kTunerMaxHz;
        return out;
    }

private:
    std::array<float, kDecWindow> dec_{};
    std::array<double, kMaxLag + 2> diff_{};
    std::array<double, kMaxLag + 2> cmndf_{};
};

}  // namespace namzero
