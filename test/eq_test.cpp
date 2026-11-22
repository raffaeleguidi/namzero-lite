// Test dell'EQ globale a 5 bande (header-only, nessuna dipendenza audio).
//
// Verifica:
//   1) a 0 dB il segnale passa inalterato (coefficienti identita');
//   2) con +12 dB sulla banda 1 il guadagno a 250 Hz sale di ~4x;
//   3) cambiare un guadagno dal "thread ctl" aggiorna i coefficienti al blocco
//      successivo, senza prepare() ne' trig per frame;
//   4) on/off con Chain::set_enabled non azzera la memoria dei biquad.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "dsp/chain.h"
#include "dsp/eq_slot.h"

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kFrames = 8192;
constexpr std::size_t kSkip = 2048;

int g_fail = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

void fill_sine(std::vector<float>& buf, double freq) {
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float v = 0.05f * static_cast<float>(
                                     std::sin(2.0 * M_PI * freq * static_cast<double>(i) / kFs));
        buf[i * 2 + 0] = v;
        buf[i * 2 + 1] = v;
    }
}

double rms(const std::vector<float>& buf) {
    double acc = 0.0;
    for (std::size_t i = kSkip; i < kFrames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            const double v = buf[i * 2 + ch];
            acc += v * v;
        }
    }
    return std::sqrt(acc / static_cast<double>((kFrames - kSkip) * 2));
}

double max_abs(const std::vector<float>& buf) {
    double m = 0.0;
    for (float v : buf) m = std::max(m, std::fabs(static_cast<double>(v)));
    return m;
}

// Guadagno misurato con un tono alla frequenza scelta: si scarta il transitorio
// e si confronta l'RMS in ingresso e in uscita.
double gain_at(float band_db, double freq) {
    namzero::EqSlot eq;
    eq.set_gain(1, band_db);
    eq.prepare(kFs, 64);

    std::vector<float> in(kFrames * 2);
    fill_sine(in, freq);
    std::vector<float> out = in;
    eq.process(out.data(), kFrames);
    for (float v : out) {
        if (!std::isfinite(v)) return -1.0;
    }
    return rms(out) / rms(in);
}

}  // namespace

int main() {
    namzero::EqSlot eq;
    CHECK(std::string(eq.name()) == "eq");
    CHECK(namzero::EqSlot::kBandCount == 5);
    eq.prepare(kFs, 64);

    // ── 1) 0 dB: piatta e trasparente ──────────────────────────────────────
    {
        namzero::EqSlot flat;
        flat.prepare(kFs, 64);
        std::vector<float> in(kFrames * 2);
        for (std::size_t i = 0; i < in.size(); ++i) {
            // Segnale deterministico e "duro": se il filtro non fosse
            // identita', la differenza si vedrebbe subito.
            in[i] = std::sin(static_cast<float>(i) * 0.37f) * 0.4f;
        }
        std::vector<float> out = in;
        flat.process(out.data(), kFrames);
        double diff = 0.0;
        for (std::size_t i = 0; i < in.size(); ++i) {
            diff = std::max(diff, std::fabs(static_cast<double>(out[i] - in[i])));
        }
        CHECK(diff == 0.0);
        for (int b = 0; b < namzero::EqSlot::kBandCount; ++b) {
            CHECK(flat.gain_db(b) == 0.0f);
        }
    }

    // ── 2) +12 dB sulla banda 1 (250 Hz) ───────────────────────────────────
    {
        const double flat = gain_at(0.0f, 250.0);
        const double boosted = gain_at(12.0f, 250.0);
        // +12 dB = 3.98x. Il peaking Q 0.7 al centro vale esattamente +12 dB;
        // si controlla "chiaramente piu' alto" con un margine largo.
        CHECK(boosted > 3.0 * flat);
        std::printf("eq: guadagno a 250 Hz: %.2f (0 dB) -> %.2f (+12 dB banda 1)\n",
                    flat, boosted);
    }

    // ── 3) set_gain dal ctl: ricalcolo al blocco successivo ────────────────
    {
        namzero::EqSlot live;
        live.set_gain(2, 0.0f);
        live.prepare(kFs, 64);
        std::vector<float> in(kFrames * 2);
        fill_sine(in, 800.0);
        std::vector<float> before = in;
        live.process(before.data(), kFrames);
        const double g0 = rms(before) / rms(in);

        live.set_gain(2, 12.0f);  // come farebbe il comando {"cmd":"eq",...}
        std::vector<float> after = in;
        live.process(after.data(), kFrames);
        const double g1 = rms(after) / rms(in);
        CHECK(g1 > 3.0 * g0);
        CHECK(std::fabs(live.gain_db(2) - 12.0f) < 1e-6f);
    }

    // ── 4) on/off con set_enabled: la memoria dei biquad resta ─────────────
    {
        namzero::EqSlot* eq_ptr = nullptr;
        namzero::Chain chain;
        {
            auto slot = std::make_unique<namzero::EqSlot>();
            eq_ptr = slot.get();
            eq_ptr->set_gain(1, 12.0f);
            chain.add(std::move(slot));
        }
        chain.prepare(kFs, 64);
        chain.set_enabled(0, true);
        chain.reset();
        CHECK(chain.enabled(0));

        // Impulso: il peaking a 250 Hz continua a risuonare nei blocchi dopo.
        std::vector<float> impulse(64 * 2, 0.0f);
        impulse[0] = 1.0f;
        impulse[1] = 1.0f;
        chain.process(impulse.data(), 64);

        // Spento per un blocco e riacceso: se set_enabled azzerasse lo stato,
        // il blocco successivo (ingresso zero) uscirebbe muto.
        std::vector<float> zeros(64 * 2, 0.0f);
        chain.set_enabled(0, false);
        chain.process(zeros.data(), 64);
        CHECK(!chain.enabled(0));

        std::vector<float> tail(64 * 2, 0.0f);
        chain.set_enabled(0, true);
        chain.process(tail.data(), 64);
        CHECK(chain.enabled(0));
        CHECK(max_abs(tail) > 1e-5);

        // Riferimento: un EQ riavviato da zero, con ingresso zero, e' muto.
        namzero::EqSlot fresh;
        fresh.set_gain(1, 12.0f);
        fresh.prepare(kFs, 64);
        std::vector<float> fresh_out(64 * 2, 0.0f);
        fresh.process(fresh_out.data(), 64);
        CHECK(max_abs(fresh_out) == 0.0);

        std::printf("eq: coda dopo off/on = %.6f (memoria biquad preservata)\n",
                    max_abs(tail));
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "eq: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("eq: 5 bande, 0 dB trasparente, +12 dB a 250 Hz, memoria preservata\n");
    return 0;
}
