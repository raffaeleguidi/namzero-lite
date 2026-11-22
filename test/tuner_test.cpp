// Unit test del rilevatore di intonazione (nessuna dipendenza audio/ALSA).
//
// Oltre alle sinusoidi pulite, ai riferimento/transpose e al silenzio, simula
// corde pizzicate realistiche (attacco, decadimento per armonica, leggera
// inharmonicita', rumore) su tutte e sei le corde. I cantini B3/E4 sono i casi
// che in passato sparivano: il test verifica che la nota resti per almeno il
// 90% di 1,5 s con cent coerenti, sia con la sola detezione sia con l'hold.
// Un test con una componente forte a 7,3 kHz (che senza anti-alias si ripiega
// a 4,7 kHz) blinda il filtro prima della decimazione.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "dsp/tuner.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 48000.0;
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

// Corda pizzicata: la posizione del plettro d dà le ampiezze |sin(pi*h*d)|/h,
// ogni armonica decade con tau/h^0.8 (le alte prima), leggera inharmonicita',
// attacco di 4 ms e rumore bianco deterministico. peak e' il picco del segnale
// pulito prima del rumore.
std::vector<float> pluck(double f0, double dur, double peak, double pluck_pos,
                         double tau, double fast_tau, double noise,
                         unsigned seed, int maxh = 60) {
    const std::size_t n = static_cast<std::size_t>(kRate * dur);
    std::vector<float> out(n, 0.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    for (int h = 1; h <= maxh; ++h) {
        const double amp = std::fabs(std::sin(kPi * h * pluck_pos)) / h;
        if (amp < 1e-5) continue;
        const double fh = h * f0 * std::sqrt(1.0 + 1e-4 * h * h);
        if (fh >= kRate * 0.48) break;
        double th = tau / std::pow(static_cast<double>(h), 0.8);
        th = std::min(th, fast_tau);
        th = std::max(th, 0.07);
        for (std::size_t i = 0; i < n; ++i) {
            const double t = static_cast<double>(i) / kRate;
            out[i] += static_cast<float>(amp * std::exp(-t / th) *
                                         std::sin(2.0 * kPi * fh * t + h * 0.9));
        }
    }
    const std::size_t attack = static_cast<std::size_t>(0.004 * kRate);
    for (std::size_t i = 0; i < attack && i < n; ++i) {
        out[i] *= static_cast<float>(0.5 - 0.5 * std::cos(kPi * i / attack));
    }
    float pk = 1e-9f;
    for (float v : out) pk = std::max(pk, std::fabs(v));
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = out[i] / pk * static_cast<float>(peak) +
                 static_cast<float>(noise * nd(rng));
    }
    return out;
}

// Aggiunge una sinusoide fuori banda (sopra il Nyquist dei 12 kHz) per
// verificare che il filtro anti-alias la elimini prima della decimazione.
void add_high_tone(std::vector<float>& x, double freq, double amp) {
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double t = static_cast<double>(i) / kRate;
        x[i] += static_cast<float>(amp * std::sin(2.0 * kPi * freq * t));
    }
}

struct SeqStat {
    int frames = 0;
    int voiced = 0;
    int wrong_note = 0;  // nota sbagliata o |cent| > 10
    double cents_sum = 0.0;
    int cents_n = 0;
    double last_voiced_t = 0.0;
};

// Analizza la sequenza come fa il thread tuner (hop 1/15 s) fra t0 e t1.
SeqStat analyze_sequence(const std::vector<float>& x, double f0, double t0,
                         double t1, bool hold) {
    namzero::TunerDetector d(hold);
    const std::size_t w = namzero::TunerDetector::kWindow;
    const std::size_t hop = static_cast<std::size_t>(kRate / 15.0);
    SeqStat st;
    for (std::size_t end = w; end + hop < x.size(); end += hop) {
        const double t = static_cast<double>(end) / kRate;
        if (t < t0 || t > t1) continue;
        const auto p = d.analyze(x.data() + end - w, w, kRate);
        ++st.frames;
        if (!p.voiced) continue;
        ++st.voiced;
        const double cents = 1200.0 * std::log2(p.freq / f0);
        st.last_voiced_t = t;
        st.cents_sum += std::fabs(cents);
        ++st.cents_n;
        if (std::fabs(cents) > 10.0) ++st.wrong_note;
    }
    return st;
}

void expect_coverage(const char* label, const SeqStat& st, double min_frames,
                     double max_cents) {
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "%s: %d/%d frame con nota (%.0f%%), ultima a %.2f s, "
                  "|cent| medio %.2f, sbagliate %d",
                  label, st.voiced, st.frames,
                  100.0 * st.voiced / std::max(1, st.frames), st.last_voiced_t,
                  st.cents_n ? st.cents_sum / st.cents_n : -1.0, st.wrong_note);
    std::printf("  %s\n", msg);
    check(st.frames >= static_cast<int>(min_frames),
          (std::string(label) + ": abbastanza frame analizzati").c_str());
    check(st.voiced * 10 >= st.frames * 9,
          (std::string(label) + ": nota per almeno il 90% della durata").c_str());
    check(st.wrong_note == 0,
          (std::string(label) + ": nessuna nota/ottava sbagliata").c_str());
    const double mean_cents = st.cents_n ? st.cents_sum / st.cents_n : 999.0;
    check(mean_cents <= max_cents,
          (std::string(label) + ": cent coerenti").c_str());
}

void expect_note(const char* label, double freq, double ref, int transpose,
                 const char* want_name, int want_octave, double tol_cents) {
    namzero::TunerDetector d;
    const auto x = tone(freq, 0.5, kRate, namzero::TunerDetector::kWindow);
    const auto p = d.analyze(x.data(), x.size(), kRate);
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
        const auto x = tone(440.0, 0.5, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p = d.analyze(x.data(), x.size(), kRate);
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
        const auto x = tone(415.3, 0.5, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p = d.analyze(x.data(), x.size(), kRate);
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
        const auto x415 = tone(415.3, 0.5, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p415 = d.analyze(x415.data(), x415.size(), kRate);
        const auto n415 = namzero::tuner_note_from_freq(p415.freq, 440.0, -1);
        check(n415.name == "A" && n415.octave == 4,
              "transpose -1: 415.3 = A4 (rif. effettivo 415.3)");
        check(std::fabs(n415.cents) < 1.0, "transpose -1: 415.3 = 0 cent");
        const auto x440 = tone(440.0, 0.5, kRate, TunerDetector::kWindow);
        const auto p440 = d.analyze(x440.data(), x440.size(), kRate);
        const auto n440 = namzero::tuner_note_from_freq(p440.freq, 440.0, -1);
        check(n440.name == "A#" && n440.octave == 4,
              "transpose -1: 440 = A#4 (un semitono sopra il rif.)");
        check(std::fabs(n440.cents) < 1.0, "transpose -1: 440 = 0 cent su A#4");
        expect_note("transpose-1/415.3", 415.3, 440.0, -1, "A", 4, 1.0);
    }

    // 4) Nota sbagliata: 445 Hz e' A4 ma calante di ~19.6 cent.
    {
        const auto x = tone(445.0, 0.5, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p = d.analyze(x.data(), x.size(), kRate);
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        const double want = 1200.0 * std::log2(445.0 / 440.0);
        std::printf("  445: %s%d %+.2f cent (atteso %+.2f)\n", n.name.c_str(),
                    n.octave, n.cents, want);
        check(n.name == "A" && n.octave == 4, "445 = A4");
        check(std::fabs(n.cents - want) < 1.5, "445 cent coerenti con il rapporto");
    }

    // 5) 415.3 Hz con riferimento 440 senza transpose: G#4, 0 cent.
    {
        const auto x = tone(415.3, 0.5, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p = d.analyze(x.data(), x.size(), kRate);
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        check(n.name == "G#" && n.octave == 4, "415.3/ref440 = G#4");
        check(std::fabs(n.cents) < 1.0, "415.3/ref440 = 0 cent su G#4");
    }

    // 6) MI basso (82.41 Hz): E2, 0 cent. Verifica anche le finestre basse.
    {
        const auto x = tone(82.4069, 0.5, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p = d.analyze(x.data(), x.size(), kRate);
        std::printf("  82.41 Hz: %.3f Hz, conf %.3f\n", p.freq, p.confidence);
        check(p.voiced, "82.41 rilevato");
        const auto n = namzero::tuner_note_from_freq(p.freq, 440.0, 0);
        check(n.name == "E" && n.octave == 2, "82.41 = E2");
        check(std::fabs(n.cents) < 2.0, "82.41 = 0 cent (entro 2)");
    }

    // 7) Silenzio e segnale sotto il gate: nessuna nota.
    {
        std::vector<float> silence(TunerDetector::kWindow, 0.0f);
        TunerDetector d;
        const auto p = d.analyze(silence.data(), silence.size(), kRate);
        check(!p.voiced, "silenzio: nessuna nota");
        check(p.confidence < 0.5, "silenzio: confidenza bassa");
        const auto quiet = tone(440.0, 1e-4, kRate, TunerDetector::kWindow);
        const auto q = d.analyze(quiet.data(), quiet.size(), kRate);
        check(!q.voiced, "segnale sotto la soglia RMS: nessuna nota");
    }

    // 8) Gamma alta: 1.2 kHz (limite utile richiesto) e 50 Hz.
    {
        const auto x = tone(1200.0, 0.4, kRate, TunerDetector::kWindow);
        TunerDetector d;
        const auto p = d.analyze(x.data(), x.size(), kRate);
        check(p.voiced, "1200 Hz rilevato");
        check(std::fabs(1200.0 * std::log2(p.freq / 1200.0)) < 5.0,
              "1200 Hz entro 5 cent");
        const auto y = tone(82.4069, 0.4, kRate, TunerDetector::kWindow);
        const auto e2 = d.analyze(y.data(), y.size(), kRate);
        check(e2.voiced && std::fabs(1200.0 * std::log2(e2.freq / 82.4069)) < 5.0,
              "82.4 Hz ancora corretto dopo 1200 Hz");
    }

    // 9) Corde pizzicate realistiche, tutte e sei. Con l'hold attivo la nota
    //    deve restare per almeno il 90% di 1,5 s (0,1..1,6 s) con cent
    //    coerenti; i bassi devono restare stabili come prima.
    {
        struct Case {
            const char* name;
            double f0;
            double peak;
            double tau;
            double fast_tau;
            unsigned seed;
        };
        const Case cases[] = {
            {"E2", 82.4069, 0.06, 1.5, 0.35, 1}, {"A2", 110.0, 0.06, 1.4, 0.33, 2},
            {"D3", 146.832, 0.05, 1.3, 0.32, 3}, {"G3", 195.998, 0.05, 1.2, 0.30, 4},
            {"B3", 246.9417, 0.05, 1.0, 0.26, 5}, {"E4", 329.6276, 0.05, 0.9, 0.24, 6},
        };
        for (const auto& c : cases) {
            const auto x = pluck(c.f0, 2.0, c.peak, 0.10, c.tau, c.fast_tau,
                                 1e-4, c.seed);
            const auto st = analyze_sequence(x, c.f0, 0.1, 1.6, true);
            const std::string label = std::string(c.name) + " hold";
            expect_coverage(label.c_str(), st, 20, 5.0);
        }
        // Senza hold, la detezione vera dei cantini deve reggere almeno i
        // primi 0,8 s (non e' la coda tenuta artificialmente).
        for (const auto f0 : {246.9417, 329.6276}) {
            const auto x = f0 < 300.0 ? pluck(f0, 1.5, 0.05, 0.10, 1.0, 0.26, 1e-4, 11)
                                      : pluck(f0, 1.5, 0.05, 0.10, 0.9, 0.24, 1e-4, 12);
            const auto st = analyze_sequence(x, f0, 0.1, 0.9, false);
            const std::string label = f0 < 300.0 ? "B3 no-hold" : "E4 no-hold";
            expect_coverage(label.c_str(), st, 10, 5.0);
        }
    }

    // 10) Anti-alias: un cantino con una componente forte a 7,3 kHz. Senza
    //     filtro si ripiega a 4,7 kHz e sposta la lettura; col filtro la nota
    //     resta quella giusta. Il tono e' continuo, cosi' il test non dipende
    //     dal decadimento.
    {
        for (const double f0 : {246.9417, 329.6276}) {
            auto x = pluck(f0, 1.6, 0.10, 0.10, 1.2, 0.30, 1e-4,
                           f0 < 300.0 ? 21u : 22u, 70);
            add_high_tone(x, 7300.0, 0.3);
            const auto st = analyze_sequence(x, f0, 0.1, 1.1, false);
            const std::string label =
                f0 < 300.0 ? "B3 anti-alias 7.3k" : "E4 anti-alias 7.3k";
            expect_coverage(label.c_str(), st, 13, 5.0);
        }
    }

    // 11) Hold: dopo la fine del segnale la nota resta ~0,6 s con confidenza
    //     in fade, poi sparisce; una nuova nota la sostituisce subito.
    {
        TunerDetector d;  // hold attivo
        const auto note = pluck(246.9417, 1.0, 0.05, 0.10, 0.8, 0.25, 1e-4, 31);
        const std::size_t w = TunerDetector::kWindow;
        const std::size_t hop = static_cast<std::size_t>(kRate / 15.0);
        namzero::TunerPitch last;
        for (std::size_t end = w; end + hop < note.size(); end += hop) {
            last = d.analyze(note.data() + end - w, w, kRate);
        }
        check(last.voiced, "hold: nota presente prima del silenzio");
        const double fresh_conf = last.confidence;
        std::vector<float> silence(w, 0.0f);
        namzero::TunerPitch mid;
        for (int i = 0; i < 5; ++i) mid = d.analyze(silence.data(), w, kRate);
        check(mid.voiced && std::fabs(mid.freq - last.freq) < 1.0,
              "hold: nota mantenuta ~0,3 s dopo il silenzio");
        check(mid.confidence < fresh_conf && mid.confidence > 0.0,
              "hold: confidenza in fade");
        namzero::TunerPitch gone = mid;
        for (int i = 0; i < 8; ++i) gone = d.analyze(silence.data(), w, kRate);
        check(!gone.voiced, "hold: nota sparita dopo ~0,7 s");

        // Nuova nota durante l'hold: deve sostituire subito la vecchia.
        const auto a4 = tone(440.0, 0.4, kRate, w);
        const auto fresh = d.analyze(a4.data(), w, kRate);
        check(fresh.voiced && std::fabs(fresh.freq - 440.0) < 1.0,
              "hold: nuova nota sostituisce quella vecchia");
        const auto n = namzero::tuner_note_from_freq(fresh.freq, 440.0, 0);
        check(n.name == "A" && n.octave == 4, "hold: la nuova nota e' A4");
        d.reset();
        const auto after_reset = d.analyze(silence.data(), w, kRate);
        check(!after_reset.voiced, "reset: nessuna nota trattenuta al riavvio");
    }

    // 12) Rumore bianco continuo: il gate adattivo non deve produrre note.
    {
        std::mt19937 rng(99);
        std::normal_distribution<double> nd(0.0, 1.0);
        std::vector<float> noise(TunerDetector::kWindow);
        TunerDetector d;
        bool any = false;
        for (int f = 0; f < 12; ++f) {
            for (auto& v : noise) v = static_cast<float>(0.002 * nd(rng));
            const auto p = d.analyze(noise.data(), noise.size(), kRate);
            any = any || p.voiced;
        }
        check(!any, "rumore: nessuna nota fantasma");

        // Ronzio di rete stabile e basso: il gate adattivo lo impara e lo
        // scarta dopo qualche secondo, senza lasciare una nota fantasma.
        TunerDetector hum_detector;
        std::vector<float> hum(TunerDetector::kWindow);
        bool late_hum = false;
        for (int f = 0; f < 90; ++f) {
            for (std::size_t i = 0; i < hum.size(); ++i) {
                const double t = static_cast<double>(i) / kRate;
                hum[i] = static_cast<float>(0.001 * std::sin(2.0 * kPi * 100.0 * t));
            }
            const auto p = hum_detector.analyze(hum.data(), hum.size(), kRate);
            if (f >= 75 && p.voiced) late_hum = true;
        }
        check(!late_hum, "ronzio stabile: il gate adattivo lo scarta");
    }

    // 13) TunerTap: push/read_latest restituisce gli ultimi campioni.
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

    // 14) Riferimento/transpose fuori scala: clampati.
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
