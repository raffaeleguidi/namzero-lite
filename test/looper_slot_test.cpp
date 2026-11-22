// Test del looper (header-only, nessuna dipendenza audio).
//
// Verifica:
//   1) autostart OFF: il toggle da fermo registra subito (comportamento storico);
//   2) autostart ON: il toggle da fermo mette in attesa (stato "armed") e non
//      registra finche' l'ingresso non resta sopra soglia per due blocchi;
//   3) il trigger registra dal PRIMO campione del blocco che supera la soglia
//      (il blocco precedente, quello dell'attacco, non viene registrato);
//   4) un click isolato (un solo blocco forte) non fa partire la registrazione;
//   5) cancel (toggle), stop e clear dall'attesa tornano a idle;
//   6) soglia: sotto non parte, sopra si'; i setter fanno clamp;
//   7) spegnere l'autostart mentre e' in attesa fa partire subito (equivale a
//      un toggle da fermo con il flag OFF);
//   8) non regressione di rec/play/dub/arm quantizzato/stop/riprendi/clear e
//      distinzione fra "armed" (attesa) e armed() (overdub quantizzato).
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "dsp/looper_slot.h"

namespace {

constexpr double kFs = 48000.0;
constexpr std::size_t kBlock = 64;

int g_fail = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                           \
        }                                                                       \
    } while (0)

using namzero::LooperSlot;

bool close_to(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

std::vector<float> make_block(float l, float r) {
    std::vector<float> b(2 * kBlock);
    for (std::size_t i = 0; i < kBlock; ++i) {
        b[2 * i] = l;
        b[2 * i + 1] = r;
    }
    return b;
}

std::vector<float> run(LooperSlot& lp, float l, float r) {
    auto b = make_block(l, r);
    lp.process(b.data(), kBlock);
    return b;
}

// Applica il comando depositato senza far avanzare il buffer (utile per
// osservare lo stato subito dopo un comando, prima del blocco successivo).
void step(LooperSlot& lp) {
    float dummy[2] = {0.0f, 0.0f};
    lp.process(dummy, 0);
}

// Tutti i campioni del blocco (dopo il process) valgono "v".
bool block_is(const std::vector<float>& b, float v, double tol = 1e-6) {
    for (float x : b) {
        if (std::fabs(static_cast<double>(x) - static_cast<double>(v)) > tol) return false;
    }
    return true;
}

constexpr LooperSlot::State kIdle = LooperSlot::State::kIdle;
constexpr LooperSlot::State kRec = LooperSlot::State::kRecording;
constexpr LooperSlot::State kPlay = LooperSlot::State::kPlaying;
constexpr LooperSlot::State kDub = LooperSlot::State::kOverdub;
constexpr LooperSlot::State kArmed = LooperSlot::State::kArmed;

void test_names_and_limits() {
    CHECK(std::string(LooperSlot::state_name(kIdle)) == "idle");
    CHECK(std::string(LooperSlot::state_name(kRec)) == "rec");
    CHECK(std::string(LooperSlot::state_name(kPlay)) == "play");
    CHECK(std::string(LooperSlot::state_name(kDub)) == "dub");
    // Il nome pubblico dell'attesa e' "armed"; armed() resta l'overdub.
    CHECK(std::string(LooperSlot::state_name(kArmed)) == "armed");

    LooperSlot tiny(0.0001);
    CHECK(close_to(tiny.max_seconds(), LooperSlot::kMaxSecondsMin));
    LooperSlot huge(1e9);
    CHECK(close_to(huge.max_seconds(), LooperSlot::kMaxSecondsMax));
    LooperSlot def;
    CHECK(close_to(def.max_seconds(), LooperSlot::kDefaultMaxSeconds));
    CHECK(!def.autostart());
    CHECK(close_to(def.autostart_threshold_db(),
                   LooperSlot::kDefaultAutostartThresholdDb));
}

// 1) Autostart OFF: toggle = rec immediato, come prima della modifica.
void test_autostart_off_records_immediately() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);
    CHECK(lp.state() == kIdle);
    CHECK(!lp.autostart());

    lp.command(LooperSlot::Command::kToggle);
    auto b = run(lp, 0.25f, 0.25f);
    CHECK(lp.state() == kRec);
    CHECK(lp.length_seconds() == 0.0);
    CHECK(close_to(lp.position_seconds(), static_cast<double>(kBlock) / kFs));
    CHECK(!lp.armed());

    // Fine registrazione: play del loop appena fatto.
    lp.command(LooperSlot::Command::kToggle);
    auto out = run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kPlay);
    CHECK(close_to(lp.length_seconds(), static_cast<double>(kBlock) / kFs));
    CHECK(block_is(out, 0.25f));
    CHECK(!lp.armed());

    // In play il toggle arma l'overdub: parte al confine del loop.
    lp.command(LooperSlot::Command::kToggle);
    step(lp);
    CHECK(lp.state() == kPlay);
    CHECK(lp.armed());
    // Un giro intero: armed() si consuma al confine e entra in dub.
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kDub);
    CHECK(!lp.armed());
    lp.command(LooperSlot::Command::kToggle);  // chiude il dub
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kPlay);
}

// 2) + 3) Autostart ON: attesa, nessuna registrazione sotto soglia, trigger dal
// primo campione del secondo blocco forte.
void test_autostart_waits_and_triggers_from_first_sample() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);
    lp.set_autostart(true);
    lp.set_autostart_threshold_db(-42.0);

    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);
    CHECK(!lp.armed());
    CHECK(lp.length_seconds() == 0.0);

    // Silenzio: resta in attesa, niente loop.
    for (int i = 0; i < 5; ++i) run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);
    CHECK(lp.length_seconds() == 0.0);
    CHECK(close_to(lp.position_seconds(), 0.0));

    // Primo blocco forte (attacco): non basta, serve il secondo consecutivo.
    auto first = run(lp, 0.5f, 0.5f);
    CHECK(lp.state() == kArmed);
    CHECK(close_to(lp.position_seconds(), 0.0));
    // Il segnale passa dry mentre e' in attesa (nessun loop da sommare).
    CHECK(block_is(first, 0.5f));

    // Secondo blocco forte: parte la registrazione, dal campione 0 del blocco.
    auto second = run(lp, 0.25f, 0.25f);
    CHECK(lp.state() == kRec);
    CHECK(close_to(lp.position_seconds(), static_cast<double>(kBlock) / kFs));
    CHECK(lp.length_seconds() == 0.0);
    CHECK(block_is(second, 0.25f));  // il looper non colora l'ingresso in rec

    // Terzo blocco: continua a registrare.
    run(lp, 0.125f, 0.125f);
    CHECK(lp.state() == kRec);
    CHECK(close_to(lp.position_seconds(), 2.0 * kBlock / kFs));

    // Stop: finalizza il loop (solo i due blocchi registrati).
    lp.command(LooperSlot::Command::kStop);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);
    CHECK(close_to(lp.length_seconds(), 2.0 * static_cast<double>(kBlock) / kFs));
    CHECK(close_to(lp.position_seconds(), 0.0));

    // Play: il primo giro restituisce il SECONDO blocco (0.25), non l'attacco
    // (0.5): la registrazione e' partita dal primo campione del trigger.
    lp.command(LooperSlot::Command::kToggle);
    auto p1 = run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kPlay);
    CHECK(block_is(p1, 0.25f));
    auto p2 = run(lp, 0.0f, 0.0f);
    CHECK(block_is(p2, 0.125f));
    // Al confine del loop l'overdub armato parte, ma solo perche' armed() era
    // true: lo stato "armed" dell'attesa e' un'altra cosa.
    CHECK(lp.state() == kDub);
    CHECK(!lp.armed());
}

// 4) Un click isolato non deve far partire la registrazione.
void test_click_rejection() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);
    lp.set_autostart(true);
    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);

    run(lp, 0.9f, 0.9f);  // click
    CHECK(lp.state() == kArmed);
    for (int i = 0; i < 3; ++i) run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);
    CHECK(lp.length_seconds() == 0.0);

    // Un forte isolato seguito da silenzio: ancora in attesa.
    run(lp, 0.9f, 0.9f);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);

    // Due forti consecutivi: parte.
    run(lp, 0.9f, 0.9f);
    run(lp, 0.9f, 0.9f);
    CHECK(lp.state() == kRec);
}

// 5) Dall'attesa si esce con toggle (annulla), stop e clear.
void test_cancel_stop_clear_from_armed() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);
    lp.set_autostart(true);

    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);
    lp.command(LooperSlot::Command::kToggle);  // annulla
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);
    CHECK(lp.length_seconds() == 0.0);

    lp.command(LooperSlot::Command::kToggle);  // ri-arma
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);
    lp.command(LooperSlot::Command::kStop);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);

    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);
    lp.command(LooperSlot::Command::kClear);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);
    CHECK(lp.length_seconds() == 0.0);
}

// 6) Soglia: sotto resta in attesa, sopra parte; setter con clamp.
void test_threshold() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);
    lp.set_autostart(true);
    lp.set_autostart_threshold_db(-6.0);
    CHECK(close_to(lp.autostart_threshold_db(), -6.0));

    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);

    // 0.3 su entrambi i canali = circa -10.5 dBFS, sotto la soglia di -6.
    for (int i = 0; i < 4; ++i) run(lp, 0.3f, 0.3f);
    CHECK(lp.state() == kArmed);
    CHECK(lp.length_seconds() == 0.0);

    // 0.9 = circa -0.9 dBFS, sopra -6: due blocchi e parte.
    run(lp, 0.9f, 0.9f);
    CHECK(lp.state() == kArmed);
    run(lp, 0.9f, 0.9f);
    CHECK(lp.state() == kRec);

    lp.set_autostart_threshold_db(-999.0);
    CHECK(close_to(lp.autostart_threshold_db(),
                   LooperSlot::kAutostartThresholdMinDb));
    lp.set_autostart_threshold_db(99.0);
    CHECK(close_to(lp.autostart_threshold_db(),
                   LooperSlot::kAutostartThresholdMaxDb));
    lp.set_autostart_threshold_db(std::numeric_limits<double>::quiet_NaN());
    CHECK(close_to(lp.autostart_threshold_db(),
                   LooperSlot::kDefaultAutostartThresholdDb));
}

// 7) Spegnere l'autostart mentre e' in attesa: registra subito.
void test_disable_while_armed_starts_recording() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);
    lp.set_autostart(true);
    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kArmed);

    lp.set_autostart(false);
    run(lp, 0.4f, 0.4f);
    CHECK(lp.state() == kRec);
    CHECK(close_to(lp.position_seconds(), static_cast<double>(kBlock) / kFs));
    lp.command(LooperSlot::Command::kStop);
    run(lp, 0.0f, 0.0f);
    lp.command(LooperSlot::Command::kToggle);
    step(lp);
    CHECK(lp.state() == kPlay);  // riprende il loop (e arma l'overdub)
    auto out = run(lp, 0.0f, 0.0f);
    CHECK(block_is(out, 0.4f));
}

// 8) Non regressione di stop/riprendi/clear con autostart OFF.
void test_stop_resume_clear() {
    LooperSlot lp(5.0);
    lp.prepare(kFs, kBlock);

    // Registra due blocchi e ferma: loop finalizzato, idle.
    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.2f, 0.2f);
    run(lp, 0.3f, 0.3f);
    lp.command(LooperSlot::Command::kStop);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);
    CHECK(close_to(lp.length_seconds(), 2.0 * static_cast<double>(kBlock) / kFs));

    // Stop da fermo con un loop: riprende in play.
    lp.command(LooperSlot::Command::kStop);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kPlay);
    CHECK(block_is(run(lp, 0.0f, 0.0f), 0.3f));

    // Stop da play: torna idle senza cancellare il loop.
    lp.command(LooperSlot::Command::kStop);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);
    CHECK(close_to(lp.length_seconds(), 2.0 * static_cast<double>(kBlock) / kFs));

    // Clear: svuota e torna idle; il toggle successivo registra da zero.
    lp.command(LooperSlot::Command::kClear);
    run(lp, 0.0f, 0.0f);
    CHECK(lp.state() == kIdle);
    CHECK(lp.length_seconds() == 0.0);
    lp.command(LooperSlot::Command::kToggle);
    run(lp, 0.1f, 0.1f);
    CHECK(lp.state() == kRec);
    CHECK(close_to(lp.position_seconds(), static_cast<double>(kBlock) / kFs));

    // reset() torna idle senza toccare la configurazione dell'autostart.
    lp.set_autostart(true);
    lp.reset();
    CHECK(lp.state() == kIdle);
    CHECK(lp.autostart());
}

}  // namespace

int main() {
    test_names_and_limits();
    test_autostart_off_records_immediately();
    test_autostart_waits_and_triggers_from_first_sample();
    test_click_rejection();
    test_cancel_stop_clear_from_armed();
    test_threshold();
    test_disable_while_armed_starts_recording();
    test_stop_resume_clear();

    if (g_fail != 0) {
        std::fprintf(stderr, "looper_slot_test: %d FAIL\n", g_fail);
        return 1;
    }
    std::printf("looper_slot_test: OK\n");
    return 0;
}
