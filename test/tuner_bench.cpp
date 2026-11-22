// Benchmark del rilevatore di intonazione (gira sul Pi, non fa parte di ctest).
//
// Misura il costo di un'analisi completa (copia dal ring lock-free + filtro
// anti-alias + YIN) su finestre con note reali e armoniche, cosi' il numero non
// e' falsato dal gate RMS sul silenzio. Due casi: G3 (basso, stesso segnale di
// prima per confrontare i numeri) ed E4 (cantino, il caso che perdeva la nota).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/tuner.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
constexpr std::size_t kFrames = 4096;
constexpr int kIterations = 2000;

bool bench_case(const char* label, double f0, int harmonics) {
    std::vector<float> interleaved(2 * kFrames);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        double v = 0.0;
        for (int h = 1; h <= harmonics; ++h) {
            v += std::sin(2.0 * kPi * f0 * h * t) / h;
        }
        const float s = static_cast<float>(0.22 * v);
        interleaved[2 * i] = s;
        interleaved[2 * i + 1] = s;
    }

    namzero::TunerTap tap;
    tap.push(interleaved.data(), kFrames);
    namzero::TunerDetector detector;
    std::vector<float> window(namzero::TunerDetector::kWindow);
    // Prima analisi fuori misura (cache, rampa).
    tap.read_latest(window.data(), window.size());
    detector.analyze(window.data(), window.size(), kRate);

    const auto t0 = std::chrono::steady_clock::now();
    namzero::TunerPitch last;
    for (int i = 0; i < kIterations; ++i) {
        tap.read_latest(window.data(), window.size());
        last = detector.analyze(window.data(), window.size(), kRate);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double avg_us = total_us / kIterations;
    std::printf("tuner_bench %s: %d analisi, %.1f us/analisi, %.2f Hz, conf %.2f\n",
                label, kIterations, avg_us, last.freq, last.confidence);
    std::printf("tuner_bench %s: a 15 Hz = %.2f%% di un core, a 20 Hz = %.2f%%\n",
                label, avg_us * 15.0 / 1e4, avg_us * 20.0 / 1e4);
    return last.voiced;
}

}  // namespace

int main() {
    const bool g3 = bench_case("G3", 196.0, 6);
    const bool e4 = bench_case("E4", 329.6276, 12);
    return g3 && e4 ? 0 : 1;
}
