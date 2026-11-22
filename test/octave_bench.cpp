#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/drive_slot.h"

// Microbench del fuzz con e senza ottava. Si lancia sul Pi:
//   ./build/octave_bench
// Misura il costo per frame stereo (48 kHz) e il tempo reale fattibile.
int main() {
    constexpr double kFs = 48000.0;
    constexpr std::size_t kBlocks = 1500;   // 2 s di audio
    constexpr std::size_t kBlock = 64;
    constexpr std::size_t kFrames = kBlocks * kBlock;
    constexpr double kBudgetNs = 1e9 / kFs;  // un frame a 48 kHz

    std::vector<float> in(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float v = 0.10f * static_cast<float>(
                                     std::sin(2.0 * M_PI * 220.0 * static_cast<double>(i) / kFs));
        in[i * 2 + 0] = v;
        in[i * 2 + 1] = v;
    }

    auto measure = [&](float octave) {
        namzero::DriveSlot slot;
        slot.set_type(namzero::DriveSlot::Type::kFuzz);
        slot.set_drive(0.3f);
        slot.set_tone(0.5f);
        slot.set_low_db(5.0f);
        slot.set_octave(octave);
        slot.prepare(kFs, kBlock);
        std::vector<float> buf(kFrames * 2);

        // Un giro di riscaldamento (cache, ramo del filtro, DVFS).
        for (std::size_t b = 0; b < kBlocks; b += 100) {
            std::copy(in.begin() + static_cast<std::ptrdiff_t>(b * kBlock * 2),
                      in.begin() + static_cast<std::ptrdiff_t>((b + 100) * kBlock * 2),
                      buf.begin() + static_cast<std::ptrdiff_t>(b * kBlock * 2));
            slot.process(buf.data() + b * kBlock * 2, 100 * kBlock);
        }
        slot.reset();

        const auto t0 = std::chrono::steady_clock::now();
        for (std::size_t b = 0; b < kBlocks; ++b) {
            std::copy(in.begin() + static_cast<std::ptrdiff_t>(b * kBlock * 2),
                      in.begin() + static_cast<std::ptrdiff_t>((b + 1) * kBlock * 2),
                      buf.begin() + static_cast<std::ptrdiff_t>(b * kBlock * 2));
            slot.process(buf.data() + b * kBlock * 2, kBlock);
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double seconds =
            std::chrono::duration<double>(t1 - t0).count();
        const double ns_per_frame = seconds * 1e9 / static_cast<double>(kFrames);
        const double audio_seconds = static_cast<double>(kFrames) / kFs;
        return std::make_pair(ns_per_frame, audio_seconds / seconds);
    };

    const auto off = measure(0.0f);
    const auto on = measure(1.0f);
    const double delta = on.first - off.first;

    std::printf("fuzz octave: %.3f ns/frame (%.2f%% di un core @48k) a octave 0, "
                "%.3f ns/frame (%.2f%%) a octave 1\n",
                off.first, 100.0 * off.first / kBudgetNs,
                on.first, 100.0 * on.first / kBudgetNs);
    std::printf("costo ottava: %+.3f ns/frame (%+.2f%% di un core, "
                "%+.1f%% sul fuzz), x%.1f e x%.1f tempo reale\n",
                delta, 100.0 * delta / kBudgetNs,
                off.first > 0.0 ? 100.0 * delta / off.first : 0.0,
                off.second, on.second);
    return 0;
}
