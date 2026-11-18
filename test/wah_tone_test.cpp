#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/mod_slot.h"

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kFrames = 8192;
constexpr std::size_t kSkip = 2048;

int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

double rms(const std::vector<float>& x) {
    double acc = 0.0;
    for (std::size_t i = kSkip; i < kFrames; ++i) acc += x[i * 2] * x[i * 2];
    return std::sqrt(acc / static_cast<double>(kFrames - kSkip));
}

// Guadagno del filtro alla propria frequenza centrale, con il pedale in una
// posizione data. La posizione determina fc sulla scala logaritmica del wah.
double gain_at(float pos, float q, float low_hz, float high_hz) {
    namzero::ModSlot slot;
    slot.set_type(namzero::ModSlot::Type::kWah);
    slot.set_wah_q(q);
    slot.set_wah_mix(1.0f);
    slot.set_wah_low_hz(low_hz);
    slot.set_wah_high_hz(high_hz);
    slot.set_position(pos);
    slot.prepare(kFs, 64);

    const double fc = low_hz * std::pow(static_cast<double>(high_hz) / low_hz, pos);
    std::vector<float> in(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float v = 0.05f * std::sin(2.0 * M_PI * fc * static_cast<double>(i) / kFs);
        in[i * 2 + 0] = v;
        in[i * 2 + 1] = v;
    }
    std::vector<float> out = in;
    slot.process(out.data(), kFrames);

    for (float v : out) {
        if (!std::isfinite(v)) return -1.0f;
    }
    return rms(out) / rms(in);
}

}  // namespace

int main() {
    // Preset wah: 350..2000 Hz, Q 3.5.
    const double toe = gain_at(1.0f, 3.5f, 350.0f, 2000.0f);
    const double heel = gain_at(0.0f, 3.5f, 350.0f, 2000.0f);

    // Alla punta la risonanza deve essere addolcita: senza la riduzione del Q
    // il picco era ~5.5x e suonava "acido", un fischio piuttosto che un wah.
    CHECK(toe > 1.5 && toe < 3.2);

    // Al tacco il filtro resta piu' selettivo: e' li' che serve il carattere.
    CHECK(heel > toe);

    // Il Q non deve pero' crollare a meta' corsa.
    const double mid = gain_at(0.5f, 3.5f, 350.0f, 2000.0f);
    CHECK(mid > 2.0);

    if (g_fail != 0) {
        std::fprintf(stderr, "wah_tone: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("wah: guadagno tacco %.2f, meta' %.2f, punta %.2f\n", heel, mid, toe);
    return 0;
}
