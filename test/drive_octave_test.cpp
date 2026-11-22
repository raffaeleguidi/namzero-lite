#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/drive_slot.h"

namespace {

constexpr double kFs = 48000.0;
constexpr double kF0 = 220.0;
// 2 s esatti: 220 Hz -> 440 cicli, 440 Hz -> 880 cicli. Togliendo i primi
// 4800 campioni (0,1 s) restano 418 cicli esatti a 220 Hz: niente leakage.
constexpr std::size_t kFrames = 96000;
constexpr std::size_t kSkip = 4800;
constexpr float kAmp = 0.10f;

int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

std::vector<float> sine(double f0, float amp) {
    std::vector<float> x(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float v = amp * static_cast<float>(
                                  std::sin(2.0 * M_PI * f0 * static_cast<double>(i) / kFs));
        x[i * 2 + 0] = v;
        x[i * 2 + 1] = v;
    }
    return x;
}

// Ampiezza a una frequenza con la formula di Goertzel: basta una passata e
// non serve una FFT da 96k punti per due sole righe dello spettro.
double mag_at(const std::vector<float>& x, double f) {
    const double w = 2.0 * M_PI * f / kFs;
    const double coeff = 2.0 * std::cos(w);
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    for (std::size_t i = kSkip; i < kFrames; ++i) {
        s0 = x[i * 2] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return std::sqrt(s1 * s1 + s2 * s2 - coeff * s1 * s2);
}

std::vector<float> run(namzero::DriveSlot::Type type, float octave, float drive = 0.3f,
                       const std::vector<float>& in = sine(kF0, kAmp)) {
    namzero::DriveSlot slot;
    slot.set_type(type);
    slot.set_drive(drive);
    slot.set_tone(0.5f);
    slot.set_level_db(0.0f);
    slot.set_blend(0.40f);
    slot.set_mid_db(4.0f);
    slot.set_low_db(5.0f);
    slot.set_octave(octave);
    slot.prepare(kFs, 64);
    std::vector<float> out = in;
    slot.process(out.data(), kFrames);
    return out;
}

double second_ratio(const std::vector<float>& x) {
    const double f1 = mag_at(x, kF0);
    const double f2 = mag_at(x, 2.0 * kF0);
    return f1 > 0.0 ? f2 / f1 : 0.0;
}

bool all_finite(const std::vector<float>& x) {
    for (float v : x) {
        if (!std::isfinite(v)) return false;
    }
    return true;
}

double max_step(const std::vector<float>& x) {
    double m = 0.0;
    for (std::size_t i = 1; i < kFrames; ++i) {
        m = std::max(m, std::fabs(static_cast<double>(x[i * 2]) - x[(i - 1) * 2]));
    }
    return m;
}

}  // namespace

int main() {
    const auto in = sine(kF0, kAmp);

    // octave = 0: il fuzz non ha armonica pari (tanh simmetrico), quindi a
    // 440 Hz non deve esserci praticamente nulla.
    const auto oct0 = run(namzero::DriveSlot::Type::kFuzz, 0.0f);
    const auto oct1 = run(namzero::DriveSlot::Type::kFuzz, 1.0f);
    const double r0 = second_ratio(oct0);
    const double r1 = second_ratio(oct1);
    const double f2_0 = mag_at(oct0, 2.0 * kF0);
    const double f2_1 = mag_at(oct1, 2.0 * kF0);
    CHECK(all_finite(oct0));
    CHECK(all_finite(oct1));
    CHECK(r0 < 0.01);
    CHECK(f2_1 > 10.0 * f2_0);
    // A octave = 1 l'ottava e' il segnale utile: deve dominare la fondamentale.
    CHECK(r1 > 1.0);

    // Meta' ottava: l'armonica a 2f sale con la manopola, la fondamentale resta.
    const auto half = run(namzero::DriveSlot::Type::kFuzz, 0.5f);
    const double f2_half = mag_at(half, 2.0 * kF0);
    CHECK(f2_half > 3.0 * f2_0);
    CHECK(f2_half < f2_1 * 0.8);
    CHECK(mag_at(half, kF0) > 0.25 * mag_at(oct0, kF0));

    // Clamp 0..1 del parametro.
    {
        namzero::DriveSlot slot;
        slot.set_octave(-0.5f);
        CHECK(slot.octave() == 0.0f);
        slot.set_octave(2.0f);
        CHECK(slot.octave() == 1.0f);
        slot.set_octave(0.33f);
        CHECK(std::fabs(slot.octave() - 0.33f) < 1e-6f);
        slot.set_octave(std::nanf(""));
        CHECK(slot.octave() == 0.0f);
    }

    // Il cambio a runtime non genera NaN ne' scalini (click): si alterna il
    // target a ogni blocco di 64 campioni e si guarda il massimo salto
    // campione-campione, confrontato con il regime stazionario.
    {
        namzero::DriveSlot slot;
        slot.set_type(namzero::DriveSlot::Type::kFuzz);
        slot.set_drive(0.3f);
        slot.set_tone(0.5f);
        slot.set_low_db(5.0f);
        slot.prepare(kFs, 64);
        std::vector<float> out = in;
        const std::size_t block = 64;
        bool target = false;
        for (std::size_t off = 0; off < kFrames; off += block) {
            target = !target;
            slot.set_octave(target ? 1.0f : 0.0f);
            slot.process(out.data() + off * 2, std::min(block, kFrames - off));
        }
        CHECK(all_finite(out));
        const double steady = std::max(max_step(oct0), max_step(oct1));
        const double toggled = max_step(out);
        CHECK(toggled < steady * 2.5 + 0.005);
        // Nessuna uscita assurda.
        for (float v : out) CHECK(std::fabs(v) < 4.0f);
        std::printf("octave: salto massimo in commutazione %.4f (stazionario %.4f)\n",
                    toggled, steady);
    }

    // Klon e Tube Screamer ignorano l'ottava: il suono resta identico.
    for (namzero::DriveSlot::Type type :
         {namzero::DriveSlot::Type::kKlon, namzero::DriveSlot::Type::kTubescreamer}) {
        const auto a = run(type, 0.0f);
        const auto b = run(type, 1.0f);
        double diff = 0.0;
        for (std::size_t i = 0; i < a.size(); ++i) {
            diff = std::max(diff, std::fabs(static_cast<double>(a[i]) - b[i]));
        }
        CHECK(diff < 1e-9);
        CHECK(second_ratio(b) < 0.5);
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "drive ottava: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("drive ottava: 2f/fond %.4f (octave 0) -> %.3g (octave 1); "
                "2f assoluta %.2e -> %.2e; clamp e transizioni ok\n",
                r0, r1, f2_0, f2_1);
    return 0;
}
