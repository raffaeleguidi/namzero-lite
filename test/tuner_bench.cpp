// Benchmark del rilevatore di intonazione (gira sul Pi, non fa parte di ctest).
//
// Misura il costo di un'analisi completa (copia dal ring lock-free + YIN) su
// una finestra con una nota reale e armoniche, cosi' il numero non e' falsato
// dal gate RMS sul silenzio.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/tuner.h"

int main() {
    constexpr double rate = 48000.0;
    constexpr std::size_t frames = 4096;
    constexpr int kIterations = 2000;
    constexpr double kPi = 3.14159265358979323846;

    // Segnale tipo corda: fondamentale 196 Hz (G3) + armoniche decrescenti.
    std::vector<float> interleaved(2 * frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / rate;
        double v = 0.0;
        for (int h = 1; h <= 6; ++h) {
            v += std::sin(2.0 * kPi * 196.0 * h * t) / h;
        }
        const float s = static_cast<float>(0.22 * v);
        interleaved[2 * i] = s;
        interleaved[2 * i + 1] = s;
    }

    namzero::TunerTap tap;
    tap.push(interleaved.data(), frames);

    namzero::TunerDetector detector;
    std::vector<float> window(namzero::TunerDetector::kWindow);
    // Prima analisi fuori misura (cache, rampa).
    tap.read_latest(window.data(), window.size());
    detector.analyze(window.data(), window.size(), rate);

    const auto t0 = std::chrono::steady_clock::now();
    namzero::TunerPitch last;
    for (int i = 0; i < kIterations; ++i) {
        tap.read_latest(window.data(), window.size());
        last = detector.analyze(window.data(), window.size(), rate);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double total_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double avg_us = total_us / kIterations;
    std::printf("tuner_bench: %d analisi, %.1f us/analisi, %.2f Hz, conf %.2f\n",
                kIterations, avg_us, last.freq, last.confidence);
    std::printf("tuner_bench: a 15 Hz = %.2f%% di un core, a 20 Hz = %.2f%%\n",
                avg_us * 15.0 / 1e4, avg_us * 20.0 / 1e4);
    return last.voiced ? 0 : 1;
}
