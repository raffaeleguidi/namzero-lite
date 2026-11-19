#include <cmath>
#include <cstddef>
#include <cstdio>
#include <vector>

#include "dsp/mod_slot.h"

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kBlock = 64;
constexpr std::size_t kTotal = 96000;  // 2 s

int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

struct Metrics {
    double min_rms = 0.0;
    double max_rms = 0.0;
    bool finite = true;
};

// Manda un tono e misura come varia il livello nel tempo: un effetto modulato
// deve "respirare", uno fermo no.
Metrics run(namzero::ModSlot::Type type, float depth, double f0) {
    namzero::ModSlot slot;
    slot.set_type(type);
    slot.set_rate_hz(3.0f);
    slot.set_depth(depth);
    slot.set_fx_mix(0.5f);
    slot.set_feedback(0.40f);
    slot.prepare(kFs, kBlock);

    std::vector<float> buf(kBlock * 2);
    Metrics m;
    double acc = 0.0;
    std::size_t n = 0;
    m.min_rms = 1e9;

    for (std::size_t b = 0; b < kTotal / kBlock; ++b) {
        for (std::size_t i = 0; i < kBlock; ++i) {
            const double k = static_cast<double>(b * kBlock + i);
            const float v = 0.2f * static_cast<float>(std::sin(2.0 * M_PI * f0 * k / kFs));
            buf[i * 2 + 0] = v;
            buf[i * 2 + 1] = v;
        }
        slot.process(buf.data(), kBlock);

        for (std::size_t i = 0; i < kBlock; ++i) {
            if (!std::isfinite(buf[i * 2])) m.finite = false;
            acc += buf[i * 2] * buf[i * 2];
            ++n;
        }
        if (n >= 480) {  // finestre da 10 ms
            const double rms = std::sqrt(acc / static_cast<double>(n));
            if (b * kBlock > kFs / 2) {  // salta il transitorio
                if (rms < m.min_rms) m.min_rms = rms;
                if (rms > m.max_rms) m.max_rms = rms;
            }
            acc = 0.0;
            n = 0;
        }
    }
    return m;
}

double ratio(const Metrics& m) { return m.min_rms > 0.0 ? m.max_rms / m.min_rms : 0.0; }

}  // namespace

int main() {
    const struct {
        const char* name;
        namzero::ModSlot::Type type;
    } effects[] = {
        {"tremolo", namzero::ModSlot::Type::kTremolo},
        {"univibe", namzero::ModSlot::Type::kUnivibe},
        {"phaser", namzero::ModSlot::Type::kPhaser},
        {"flanger", namzero::ModSlot::Type::kFlanger},
    };

    for (const auto& fx : effects) {
        const auto mod = run(fx.type, 0.8f, 800.0);
        CHECK(mod.finite);
        CHECK(mod.max_rms < 2.0);
        CHECK(ratio(mod) > 3.0);  // l'effetto deve modulare davvero

        // A profondita' zero non deve muoversi: se "respira" lo stesso, c'e'
        // qualcosa che oscilla per conto suo.
        const auto still = run(fx.type, 0.0f, 800.0);
        CHECK(still.finite);
        CHECK(ratio(still) < 1.2);
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "mod_fx: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("mod_fx: tremolo, univibe, phaser e flanger modulano (e tacciono a depth 0)\n");
    return 0;
}
