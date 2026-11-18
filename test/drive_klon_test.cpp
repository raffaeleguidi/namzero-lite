#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/drive_slot.h"

namespace {

constexpr double kFs = 48000.0;
constexpr double kF0 = 218.75;  // 14 cicli esatti in 3072 campioni: niente leakage
constexpr std::size_t kFrames = 4096;

int g_fail = 0;

// In Release NDEBUG spegne assert(): i controlli devono restare attivi.
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// Seno stereo: per misurare la distorsione armonica un tono continuo basta.
std::vector<float> sine(float amp) {
    std::vector<float> x(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float v = amp * std::sin(2.0 * M_PI * kF0 * static_cast<double>(i) / kFs);
        x[i * 2 + 0] = v;
        x[i * 2 + 1] = v;
    }
    return x;
}

std::vector<float> run(namzero::DriveSlot::Type type, float drive, float blend,
                       const std::vector<float>& in) {
    namzero::DriveSlot slot;
    slot.set_type(type);
    slot.set_drive(drive);
    slot.set_tone(0.5f);
    slot.set_blend(blend);
    slot.prepare(kFs, 64);
    std::vector<float> out = in;
    slot.process(out.data(), kFrames);
    return out;
}

// Distorsione armonica: rapporto tra le armoniche 2..8 e la fondamentale.
double thd(const std::vector<float>& x) {
    auto mag = [&](double f) {
        double re = 0.0;
        double im = 0.0;
        for (std::size_t i = 1024; i < kFrames; ++i) {  // salta il transitorio
            const double a = 2.0 * M_PI * f * static_cast<double>(i) / kFs;
            re += x[i * 2] * std::cos(a);
            im -= x[i * 2] * std::sin(a);
        }
        return std::sqrt(re * re + im * im);
    };

    const double fund = mag(kF0);
    double harmonics = 0.0;
    for (int h = 2; h <= 8; ++h) {
        const double m = mag(kF0 * h);
        harmonics += m * m;
    }
    return fund > 0.0 ? std::sqrt(harmonics) / fund : 0.0;
}

double rms(const std::vector<float>& x) {
    double acc = 0.0;
    for (std::size_t i = 1024; i < kFrames; ++i) acc += x[i * 2] * x[i * 2];
    return std::sqrt(acc / static_cast<double>(kFrames - 1024));
}

}  // namespace

int main() {
    const auto in = sine(0.25f);

    // A guadagno zero il Klon deve essere trasparente: stesso livello e
    // distorsione trascurabile (lo sfasamento del filtro di tono non conta).
    {
        const auto out = run(namzero::DriveSlot::Type::kKlon, 0.0f, 0.40f, in);
        const double ratio = rms(out) / rms(in);
        CHECK(ratio > 0.95 && ratio < 1.05);
        CHECK(thd(out) < 0.02);
    }

    // Stesso drive: il Klon deve sporcare meno del fuzz (e' il senso del blend
    // col pulito). Il margine e' largo, non un numero fine.
    const auto fuzz = run(namzero::DriveSlot::Type::kFuzz, 0.4f, 0.40f, in);
    const auto klon = run(namzero::DriveSlot::Type::kKlon, 0.4f, 0.40f, in);
    const double thd_fuzz = thd(fuzz);
    const double thd_klon = thd(klon);
    CHECK(thd_klon < thd_fuzz * 0.75);

    // Nessun NaN/inf e niente uscite assurde al massimo del drive.
    for (float drive : {0.0f, 0.5f, 1.0f}) {
        const auto out = run(namzero::DriveSlot::Type::kKlon, drive, 0.40f, in);
        for (float v : out) {
            CHECK(std::isfinite(v));
            CHECK(std::fabs(v) < 4.0f);
        }
        // A drive alto il pedale alza il livello (come il fuzz): il trim e'
        // level_db, non una compensazione nascosta. Deve pero' restare in una
        // fascia ragionevole, non esplodere.
        CHECK(rms(out) < rms(in) * 4.0);
    }

    // Il blend a zero e' solo segnale pulito (a parte il filtro di tono).
    {
        const auto out = run(namzero::DriveSlot::Type::kKlon, 1.0f, 0.0f, in);
        const double ratio = rms(out) / rms(in);
        CHECK(ratio > 0.8 && ratio < 1.25);
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "drive: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("drive: klon trasparente (THD %.3f) contro fuzz (THD %.3f)\n", thd_klon,
                thd_fuzz);
    return 0;
}
