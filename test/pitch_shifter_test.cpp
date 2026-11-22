#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/mod_slot.h"
#include "dsp/pitch_shifter.h"

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kBlocks = 1500;  // 2 s a 48 kHz con blocchi da 64
constexpr std::size_t kBlock = 64;
constexpr std::size_t kSkipFrames = 24000;  // 0.5 s: transienti e fade iniziali

int g_fail = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

std::vector<float> make_sine(double f0, double seconds, float amp) {
    const std::size_t frames = static_cast<std::size_t>(seconds * kFs);
    std::vector<float> x(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const float v =
            amp * static_cast<float>(std::sin(2.0 * M_PI * f0 * static_cast<double>(i) / kFs));
        x[i * 2 + 0] = v;
        x[i * 2 + 1] = v;
    }
    return x;
}

// Frequenza stimata contando gli attraversamenti dello zero in salita.
double zero_cross_freq(const std::vector<float>& x, std::size_t skip, double fs) {
    std::size_t cross = 0;
    const std::size_t frames = x.size() / 2;
    for (std::size_t i = std::max<std::size_t>(1, skip); i < frames; ++i) {
        const float a = x[(i - 1) * 2];
        const float b = x[i * 2];
        if (a < 0.0f && b >= 0.0f) ++cross;
    }
    const double seconds = static_cast<double>(frames - std::max<std::size_t>(1, skip)) / fs;
    return static_cast<double>(cross) / seconds;
}

bool all_finite(const std::vector<float>& x) {
    for (float v : x) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

double max_step(const std::vector<float>& x) {
    double m = 0.0;
    const std::size_t frames = x.size() / 2;
    for (std::size_t i = 1; i < frames; ++i) {
        const double d = std::fabs(static_cast<double>(x[i * 2] - x[(i - 1) * 2]));
        if (d > m) m = d;
    }
    return m;
}

// Passa un segnale nel pitch shifter a blocchi da 64 campioni, come il motore.
std::vector<float> run_pitch(float semitones, float mix, double f0 = 440.0) {
    namzero::PitchShifter ps;
    ps.set_semitones(semitones);
    ps.set_mix(mix);
    ps.prepare(kFs);

    std::vector<float> x = make_sine(f0, 2.0, 0.2f);
    for (std::size_t b = 0; b < kBlocks; ++b) {
        ps.process(x.data() + b * kBlock * 2, kBlock);
    }
    return x;
}

}  // namespace

int main() {
    // +12 semitoni: 440 Hz -> ~880 Hz.
    {
        const std::vector<float> out = run_pitch(12.0f, 1.0f);
        CHECK(all_finite(out));
        const double f = zero_cross_freq(out, kSkipFrames, kFs);
        CHECK(f > 880.0 * 0.98 && f < 880.0 * 1.02);
        std::printf("pitch +12: %.1f Hz (atteso 880)\n", f);
    }

    // 0 semitoni: unisono (stessa frequenza, con il ritardo della finestra).
    {
        const std::vector<float> out = run_pitch(0.0f, 1.0f);
        CHECK(all_finite(out));
        const double f = zero_cross_freq(out, kSkipFrames, kFs);
        CHECK(f > 440.0 * 0.99 && f < 440.0 * 1.01);
        std::printf("pitch 0: %.1f Hz (atteso 440)\n", f);
    }

    // -12 semitoni: 440 Hz -> ~220 Hz.
    {
        const std::vector<float> out = run_pitch(-12.0f, 1.0f);
        CHECK(all_finite(out));
        const double f = zero_cross_freq(out, kSkipFrames, kFs);
        CHECK(f > 220.0 * 0.98 && f < 220.0 * 1.02);
        std::printf("pitch -12: %.1f Hz (atteso 220)\n", f);
    }

    // Mix 0 = dry puro: l'uscita deve coincidere col segnale d'ingresso.
    {
        namzero::PitchShifter ps;
        ps.set_semitones(12.0f);
        ps.set_mix(0.0f);
        ps.prepare(kFs);
        std::vector<float> x = make_sine(440.0, 0.5, 0.2f);
        const std::vector<float> in = x;
        for (std::size_t b = 0; b < x.size() / 2 / kBlock; ++b) {
            ps.process(x.data() + b * kBlock * 2, kBlock);
        }
        double max_diff = 0.0;
        for (std::size_t i = 0; i < x.size(); ++i) {
            max_diff = std::max(max_diff, std::fabs(static_cast<double>(x[i] - in[i])));
        }
        CHECK(max_diff < 1e-6);
        std::printf("mix 0 (dry): diff max %.2e\n", max_diff);
    }

    // Latenza dichiarata: la finestra di crossfade (~30 ms a 48 kHz).
    {
        namzero::PitchShifter ps;
        ps.prepare(kFs);
        const std::size_t lat = ps.latency_frames();
        CHECK(lat > 1400 && lat < 1480);  // ~1440 campioni @ 48 kHz
        std::printf("latenza dichiarata: %zu campioni (%.2f ms)\n", lat,
                    static_cast<double>(lat) * 1000.0 / kFs);
    }

    // Cambio tipo a runtime (wah <-> whammy) senza NaN e senza click: il
    // ModSlot sfuma per ~10 ms fra i due tipi, e il pitch shifter riparte da
    // zero. Si controlla anche che il range arrivi al DSP.
    {
        namzero::ModSlot filter;
        filter.set_type(namzero::ModSlot::Type::kWah);
        filter.set_wah_q(3.0f);
        filter.set_whammy_range(12.0f);
        filter.prepare(kFs, kBlock);

        std::vector<float> x = make_sine(220.0, 1.0, 0.2f);
        const std::size_t total = x.size() / 2;
        std::size_t pos = 0;

        // 0.3 s di wah.
        while (pos + kBlock <= total * 3 / 10) {
            filter.process(x.data() + pos * 2, kBlock);
            pos += kBlock;
        }

        // Passa a whammy con il pedale a fondo: +12 semitoni.
        const std::size_t until_whammy = total * 3 / 5;
        filter.set_type(namzero::ModSlot::Type::kWhammy);
        filter.set_position(1.0f);
        while (pos < until_whammy) {
            filter.process(x.data() + pos * 2, kBlock);
            pos += kBlock;
        }
        CHECK(all_finite(x));
        CHECK(filter.type() == namzero::ModSlot::Type::kWhammy);
        CHECK(filter.whammy_ratio() > 1.9f && filter.whammy_ratio() < 2.1f);
        const double step_pitch = max_step(x);
        CHECK(step_pitch < 0.5);  // un click vero farebbe un salto ~2
        std::printf("wah -> whammy: ratio %.3f, passo max %.3f\n",
                    filter.whammy_ratio(), step_pitch);

        // Torna al wah a meta' corsa.
        filter.set_type(namzero::ModSlot::Type::kWah);
        filter.set_position(0.5f);
        while (pos < total) {
            filter.process(x.data() + pos * 2, kBlock);
            pos += kBlock;
        }
        CHECK(all_finite(x));
        CHECK(filter.type() == namzero::ModSlot::Type::kWah);
        const double step_wah = max_step(x);
        CHECK(step_wah < 0.5);
        std::printf("whammy -> wah: passo max %.3f\n", step_wah);
    }

    // Range negativo: il pedale a fondo abbassa di 12 semitoni.
    {
        namzero::ModSlot filter;
        filter.set_type(namzero::ModSlot::Type::kWhammy);
        filter.set_whammy_range(-12.0f);
        filter.set_position(1.0f);
        filter.prepare(kFs, kBlock);
        std::vector<float> x = make_sine(440.0, 1.0, 0.2f);
        for (std::size_t b = 0; b < x.size() / 2 / kBlock; ++b) {
            filter.process(x.data() + b * kBlock * 2, kBlock);
        }
        CHECK(all_finite(x));
        CHECK(filter.whammy_ratio() > 0.49f && filter.whammy_ratio() < 0.51f);
        std::printf("range -12, pos 1: ratio %.3f\n", filter.whammy_ratio());
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "pitch_shifter: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("pitch_shifter: ok\n");
    return 0;
}
