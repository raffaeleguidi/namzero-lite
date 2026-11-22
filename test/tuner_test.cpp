// Unit test del rilevatore di intonazione (nessuna dipendenza audio/ALSA).
//
// Costruisce sinusoidi pulite alla frequenza nota e verifica frequenza, nota,
// ottava e cent. Include i casi del riferimento/transpose e il silenzio.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "dsp/tuner.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_fail = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("PASS %s\n", what);
    } else {
        std::printf("FAIL %s\n", what);
        ++g_fail;
    }
}

std::vector<float> tone(double freq, double amp, double rate, std::size_t n,
                        double phase = 0.37) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / rate;
        v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * freq * t + phase));
    }
    return v;
}

// Analizza un tono e restituisce la nota letta con il riferimento richiesto.
namzero::TunerPitch analyze(double freq, double rate, double amp = 0.5) {
    const auto x = tone(freq, amp, rate, namzero::TunerDetector::kWindow);
    namzero::TunerDetector d;
    return d.analyze(x.data(), x.size(), rate);
}

void expect_note(const char* label, double freq, double ref, int transpose,
                 const char* want_name, int want_octave, double tol_cents) {
    namzero::TunerDetector d;
    const auto x = tone(freq, 0.5, 48000.0, namzero::TunerDetector::kWindow);
    const auto p = d.analyze(x.data(), x.size(), 48000.0);
    const auto n = namzero::tuner_note_from_freq(p.freq, ref, transpose);
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "%s: %.2f Hz -> %s%d %+.2f cent (conf %.2f)", label, p.freq,
                  n.name.c_str(), n.octave, n.cents, p.confidence);
    std::printf("  %s\n", msg);
    check(p.voiced, (std::string(label) + ": rilevato").c_str());
    check(n.valid, (std::string(label) + ": nota valida").c_str());
    check(n.name == want_name, (std::string(label) + ": nome nota").c_str());
    check(n.octave == want_octave, (std::string(label) + ": ottava").c_str());
    check(std::fabs(n.cents) <= tol_cents,
          (std::string(label) + ": cent vicini a 0").c_str());
}

}  // namespace

int main() {
    using namzero::TunerDetector;

    // 1) La4 a 440 Hz con riferimento 440: A4, 0 cent.
    {
        const auto p = analyze(440.0, 48000.0);
        std::printf("440 Hz: freq %.3f Hz, conf %.3f\n", p.freq, p.confidence);
        check(p.voiced, "440 rilevato");
        check(std::fabs(p.freq - 440.0) < 0.5, "440 frequenza entro 0.5 Hz");
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        check(n.name == "A" && n.octave == 4, "440 = A4 con ref 440");
        check(std::fabs(n.cents) < 1.0, "440 = 0 cent con ref 440");
        expect_note("440/ref440", 440.0, 440.0, 0, "A", 4, 1.0);
    }

    // 2) 415.3 Hz con riferimento 415.3: A4, 0 cent.
    {
        const auto p = analyze(415.3, 48000.0);
        check(p.voiced, "415.3 rilevato");
        check(std::fabs(p.freq - 415.3) < 0.5, "415.3 frequenza entro 0.5 Hz");
        const auto n = namzero::tuner_note_from_freq(p.freq, 415.3, 0);
        check(n.name == "A" && n.octave == 4, "415.3 = A4 con ref 415.3");
        check(std::fabs(n.cents) < 1.0, "415.3 = 0 cent con ref 415.3");
        expect_note("415.3/ref415.3", 415.3, 415.3, 0, "A", 4, 1.0);
    }

    // 3) Transpose -1 con riferimento 440 = riferimento effettivo 415.3:
    //    415.3 Hz diventa A4, 440 Hz diventa A#4 (un semitono sopra).
    {
        const auto p415 = analyze(415.3, 48000.0);
        const auto n415 = namzero::tuner_note_from_freq(p415.freq, 440.0, -1);
        check(n415.name == "A" && n415.octave == 4,
              "transpose -1: 415.3 = A4 (rif. effettivo 415.3)");
        check(std::fabs(n415.cents) < 1.0, "transpose -1: 415.3 = 0 cent");
        const auto p440 = analyze(440.0, 48000.0);
        const auto n440 = namzero::tuner_note_from_freq(p440.freq, 440.0, -1);
        check(n440.name == "A#" && n440.octave == 4,
              "transpose -1: 440 = A#4 (un semitono sopra il rif.)");
        check(std::fabs(n440.cents) < 1.0, "transpose -1: 440 = 0 cent su A#4");
        expect_note("transpose-1/415.3", 415.3, 440.0, -1, "A", 4, 1.0);
    }

    // 4) Nota sbagliata: 445 Hz e' A4 ma calante di ~19.6 cent.
    {
        const auto p = analyze(445.0, 48000.0);
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        const double want = 1200.0 * std::log2(445.0 / 440.0);
        std::printf("  445: %s%d %+.2f cent (atteso %+.2f)\n", n.name.c_str(),
                    n.octave, n.cents, want);
        check(n.name == "A" && n.octave == 4, "445 = A4");
        check(std::fabs(n.cents - want) < 1.5, "445 cent coerenti con il rapporto");
    }

    // 5) 415.3 Hz con riferimento 440 senza transpose: G#4, 0 cent.
    {
        const auto p = analyze(415.3, 48000.0);
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        check(n.name == "G#" && n.octave == 4, "415.3/ref440 = G#4");
        check(std::fabs(n.cents) < 1.0, "415.3/ref440 = 0 cent su G#4");
    }

    // 6) MI basso (82.41 Hz): E2, 0 cent. Verifica anche le finestre basse.
    {
        const auto p = analyze(82.4069, 48000.0);
        std::printf("  82.41 Hz: %.3f Hz, conf %.3f\n", p.freq, p.confidence);
        check(p.voiced, "82.41 rilevato");
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        check(n.name == "E" && n.octave == 2, "82.41 = E2");
        check(std::fabs(n.cents) < 2.0, "82.41 = 0 cent (entro 2)");
    }

    // 7) Silenzio: nessuna nota e confidenza bassa.
    {
        std::vector<float> silence(TunerDetector::kWindow, 0.0f);
        TunerDetector d;
        const auto p = d.analyze(silence.data(), silence.size(), 48000.0);
        check(!p.voiced, "silenzio: nessuna nota");
        check(p.confidence < 0.5, "silenzio: confidenza bassa");
        std::vector<float> quiet(TunerDetector::kWindow, 0.0f);
        for (std::size_t i = 0; i < quiet.size(); ++i) {
            quiet[i] = static_cast<float>(1e-4 * std::sin(2.0 * kPi * 440.0 * i / 48000.0));
        }
        const auto q = d.analyze(quiet.data(), quiet.size(), 48000.0);
        check(!q.voiced, "segnale sotto la soglia RMS: nessuna nota");
    }

    // 8) TunerTap: push/read_latest restituisce gli ultimi campioni.
    {
        namzero::TunerTap tap;
        std::vector<float> interleaved(2 * 10);
        for (int i = 0; i < 10; ++i) {
            interleaved[2 * i] = static_cast<float>(i);
            interleaved[2 * i + 1] = static_cast<float>(i);
        }
        tap.push(interleaved.data(), 10);
        float out[4] = {0, 0, 0, 0};
        tap.read_latest(out, 4);
        check(out[0] == 6.0f && out[1] == 7.0f && out[2] == 8.0f && out[3] == 9.0f,
              "TunerTap: ultimi campioni corretti");
    }

    // 9) Riferimento/transpose fuori scala: clampati.
    {
        const double a = namzero::tuner_effective_reference(500.0, 99);
        const double b = namzero::tuner_effective_reference(400.0, -99);
        check(std::fabs(a - 450.0 * std::pow(2.0, 6.0 / 12.0)) < 1e-6,
              "reference/transpose clamp alto");
        check(std::fabs(b - 415.0 * std::pow(2.0, -6.0 / 12.0)) < 1e-6,
              "reference/transpose clamp basso");
    }

    if (g_fail == 0) {
        std::printf("== tuner: tutti i test passano\n");
        return 0;
    }
    std::printf("== tuner: %d test falliti\n", g_fail);
    return 1;
}
