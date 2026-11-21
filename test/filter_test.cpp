#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "control/mod_types.h"
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

// Guadagno del wah in una posizione, misurato con un tono a f0. Stessa tecnica
// di wah_tone_test: il filtro e' lo stesso ModSlot, ma qui interessa il blocco
// FILTER come seconda istanza indipendente dal mod.
double gain_at(float position, double f0) {
    namzero::ModSlot filter;
    filter.set_type(namzero::ModSlot::Type::kWah);
    filter.set_wah_q(3.5f);
    filter.set_wah_mix(1.0f);
    filter.set_wah_low_hz(350.0f);
    filter.set_wah_high_hz(2200.0f);
    filter.set_position(position);
    filter.prepare(kFs, 64);

    std::vector<float> in(kFrames * 2);
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float v = 0.05f * static_cast<float>(
                                     std::sin(2.0 * M_PI * f0 * static_cast<double>(i) / kFs));
        in[i * 2 + 0] = v;
        in[i * 2 + 1] = v;
    }
    std::vector<float> out = in;
    filter.process(out.data(), kFrames);

    for (float v : out) {
        if (!std::isfinite(v)) return -1.0;
    }
    return rms(out) / rms(in);
}

}  // namespace

int main() {
    // La tabella dei tipi di mod non contiene piu' il wah: la UI e i CC non
    // possono selezionarlo, e un preset con "wah" viene mappato a tremolo.
    CHECK(namzero::kModTypeCount == 4);
    CHECK(namzero::mod_type_index("tremolo") == 0);
    CHECK(namzero::mod_type_index("univibe") == 1);
    CHECK(namzero::mod_type_index("flanger") == 2);
    CHECK(namzero::mod_type_index("phaser") == 3);
    CHECK(namzero::mod_type_index("wah") < 0);

    // Il FILTER e' una seconda istanza di ModSlot con tipo wah: la posizione
    // del pedale deve cambiare la risposta.
    constexpr double kProbe = 900.0;
    // pos 0.514 -> fc = 350 * (2200/350)^0.514 ~ 900 Hz: il filtro e' accordato.
    const double tuned = gain_at(0.514f, kProbe);
    const double heel = gain_at(0.0f, kProbe);  // fc 350: 900 Hz fuori banda
    CHECK(tuned > 0.0);
    CHECK(heel > 0.0);
    CHECK(tuned > 2.0 * heel);

    // Due istanze indipendenti (filter e mod) non si pestano i piedi: ognuna
    // tiene la propria posizione.
    {
        namzero::ModSlot filter;
        namzero::ModSlot mod;
        filter.set_type(namzero::ModSlot::Type::kWah);
        mod.set_type(namzero::ModSlot::Type::kTremolo);
        filter.set_position(0.8f);
        mod.set_position(0.2f);
        CHECK(std::fabs(filter.position() - 0.8f) < 1e-6f);
        CHECK(std::fabs(mod.position() - 0.2f) < 1e-6f);

        // La posizione del filter non deve toccare il tipo del mod.
        CHECK(filter.type() == namzero::ModSlot::Type::kWah);
        CHECK(mod.type() == namzero::ModSlot::Type::kTremolo);
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "filter: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("filter: wah nel blocco dedicato, posizione indipendente (gain %.2f vs %.2f)\n",
                tuned, heel);
    return 0;
}
