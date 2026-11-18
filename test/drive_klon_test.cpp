#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/drive_slot.h"

namespace {

constexpr double kFs = 48000.0;
// Frequenza con un numero intero di cicli nella finestra di misura: niente
// leakage spettrale che falserebbe il THD.
constexpr double kF0 = 218.75;
constexpr std::size_t kFrames = 8192;
constexpr std::size_t kSkip = 2048;

int g_fail = 0;

// In Release NDEBUG spegne assert(): i controlli devono restare attivi.
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
        const float v = amp * std::sin(2.0 * M_PI * f0 * static_cast<double>(i) / kFs);
        x[i * 2 + 0] = v;
        x[i * 2 + 1] = v;
    }
    return x;
}

std::vector<float> run(namzero::DriveSlot::Type type, float drive, float mid_db, float low_db,
                       const std::vector<float>& in) {
    namzero::DriveSlot slot;
    slot.set_type(type);
    slot.set_drive(drive);
    slot.set_tone(0.5f);
    slot.set_blend(0.40f);
    slot.set_mid_db(mid_db);
    slot.set_low_db(low_db);
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
        for (std::size_t i = kSkip; i < kFrames; ++i) {
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
    for (std::size_t i = kSkip; i < kFrames; ++i) acc += x[i * 2] * x[i * 2];
    return std::sqrt(acc / static_cast<double>(kFrames - kSkip));
}

// Guadagno a una data frequenza, con il modello e i parametri indicati.
double gain_at(namzero::DriveSlot::Type type, float drive, float mid_db, float low_db,
               double f0) {
    const auto in = sine(f0, 0.25f);
    const auto out = run(type, drive, mid_db, low_db, in);
    return rms(out) / rms(in);
}

}  // namespace

int main() {
    const auto in = sine(kF0, 0.25f);

    // A guadagno zero il Klon non deve distorcere, ma ora spinge: il makeup di
    // +3 dB e la gobba sulle medie sono voluti (non e' piu' "trasparente" al
    // punto da sembrare spento).
    {
        const auto out = run(namzero::DriveSlot::Type::kKlon, 0.0f, 4.0f, 5.0f, in);
        const double ratio = rms(out) / rms(in);
        CHECK(ratio > 1.2 && ratio < 1.6);
        CHECK(thd(out) < 0.02);
    }

    // Stesso drive: il Klon deve sporcare meno del fuzz (e' il senso del blend
    // col pulito). Il margine e' largo, non un numero fine.
    const auto fuzz = run(namzero::DriveSlot::Type::kFuzz, 0.4f, 4.0f, 5.0f, in);
    const auto klon = run(namzero::DriveSlot::Type::kKlon, 0.4f, 4.0f, 5.0f, in);
    const double thd_fuzz = thd(fuzz);
    const double thd_klon = thd(klon);
    CHECK(thd_klon < thd_fuzz * 0.75);

    // Il Klon deve spingere, non attenuare: prima sembrava abbassare il volume
    // dell'ampli. A drive 0.4 il livello sale in modo netto.
    {
        const double g = gain_at(namzero::DriveSlot::Type::kKlon, 0.4f, 4.0f, 5.0f, kF0);
        CHECK(g > 1.5);
    }

    // Gobba sulle medie: a 800 Hz sale con mid_db, a 4 kHz resta ferma.
    const double mid0 = gain_at(namzero::DriveSlot::Type::kKlon, 0.4f, 0.0f, 5.0f, 800.0);
    const double mid4 = gain_at(namzero::DriveSlot::Type::kKlon, 0.4f, 4.0f, 5.0f, 800.0);
    const double hi_mid0 = gain_at(namzero::DriveSlot::Type::kKlon, 0.4f, 0.0f, 5.0f, 4000.0);
    const double hi_mid4 = gain_at(namzero::DriveSlot::Type::kKlon, 0.4f, 4.0f, 5.0f, 4000.0);
    CHECK(mid4 > mid0 * 1.3);
    CHECK(hi_mid4 < hi_mid0 * 1.15);

    // Tube Screamer: il passa-alto a 720 Hz prima del clipping gli toglie le
    // basse e gli dà la gobba sulle medie. A 100 Hz deve rendere molto meno che
    // a 800, e deve restare piu' pulito del fuzz.
    {
        const auto ts = run(namzero::DriveSlot::Type::kTubescreamer, 0.4f, 4.0f, 5.0f, in);
        const double ts_bass = gain_at(namzero::DriveSlot::Type::kTubescreamer, 0.4f, 4.0f, 5.0f, 100.0);
        const double ts_mid = gain_at(namzero::DriveSlot::Type::kTubescreamer, 0.4f, 4.0f, 5.0f, 800.0);
        CHECK(ts_bass < ts_mid * 0.7);
        CHECK(gain_at(namzero::DriveSlot::Type::kTubescreamer, 0.4f, 4.0f, 5.0f, kF0) > 1.2);
        CHECK(thd(ts) < thd_fuzz * 0.5);
    }

    // Basse del fuzz: il low-shelf deve dare peso senza toccare il resto.
    const double low0 = gain_at(namzero::DriveSlot::Type::kFuzz, 0.4f, 4.0f, 0.0f, 80.0);
    const double low5 = gain_at(namzero::DriveSlot::Type::kFuzz, 0.4f, 4.0f, 5.0f, 80.0);
    CHECK(low5 > low0 * 1.3);

    // Nessun NaN/inf e niente uscite assurde al massimo del drive.
    for (float drive : {0.0f, 0.5f, 1.0f}) {
        const auto out = run(namzero::DriveSlot::Type::kKlon, drive, 4.0f, 5.0f, in);
        for (float v : out) {
            CHECK(std::isfinite(v));
            CHECK(std::fabs(v) < 4.0f);
        }
        CHECK(rms(out) < rms(in) * 6.0);
    }

    // Il blend a zero e' solo segnale pulito piu' il makeup di uscita.
    {
        namzero::DriveSlot slot;
        slot.set_type(namzero::DriveSlot::Type::kKlon);
        slot.set_drive(1.0f);
        slot.set_blend(0.0f);
        slot.set_mid_db(0.0f);
        slot.prepare(kFs, 64);
        std::vector<float> dry = in;
        slot.process(dry.data(), kFrames);
        const double ratio = rms(dry) / rms(in);
        CHECK(ratio > 1.3 && ratio < 1.5);  // +3 dB, nessun clipping
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "drive: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("drive: klon THD %.3f contro fuzz %.3f; medi %+.0f%%; basse fuzz %+.0f%%\n",
                thd_klon, thd_fuzz, 100.0 * (mid4 / mid0 - 1.0),
                100.0 * (low5 / low0 - 1.0));
    return 0;
}
