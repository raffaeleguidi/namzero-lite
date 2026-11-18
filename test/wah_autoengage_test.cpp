#include <cstdio>

#include "control/wah_autoengage.h"

// Controllo sempre attivo: in Release NDEBUG spegnerebbe assert(), e il test
// passerebbe senza verificare niente.
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

// Gate: tempi asimmetrici e isteresi di posizione.
static void test_gate() {
    namzero::WahAutoEngage ae;
    ae.configure(0.10f, 100, 500);
    ae.reset(0.0);

    // Pedale a fondo (heel down): niente, resta spento.
    CHECK(ae.update(0.0f, 10.0) == 0);
    CHECK(!ae.engaged());

    // Sale sopra soglia: non deve accendersi subito.
    CHECK(ae.update(0.5f, 100.0) == 0);
    CHECK(!ae.engaged());

    // A 199 ms (99 sopra soglia): ancora spento.
    CHECK(ae.update(0.5f, 199.0) == 0);
    CHECK(!ae.engaged());

    // A 201 ms (101 sopra soglia): si accende.
    CHECK(ae.update(0.5f, 201.0) == 1);
    CHECK(ae.engaged());

    // Scende sotto soglia: non si spegne subito.
    CHECK(ae.update(0.0f, 300.0) == 0);
    CHECK(ae.engaged());

    // A 799 ms (499 sotto): ancora acceso.
    CHECK(ae.update(0.0f, 799.0) == 0);
    CHECK(ae.engaged());

    // A 801 ms (501 sotto): si spegne.
    CHECK(ae.update(0.0f, 801.0) == -1);
    CHECK(!ae.engaged());

    // Rimbalzo breve sopra soglia (60 ms) poi giu': non deve accendersi.
    ae.reset(1000.0);
    ae.update(0.4f, 1000.0);
    CHECK(ae.update(0.4f, 1060.0) == 0);
    CHECK(ae.update(0.0f, 1061.0) == 0);
    CHECK(ae.update(0.0f, 1700.0) == 0);
    CHECK(!ae.engaged());
}

// Isteresi: accende a 0.20, spegne sotto 0.05. In mezzo non cambia nulla,
// nemmeno dopo molto tempo (era il caso del pedale appoggiato a meta' corsa).
static void test_hysteresis() {
    namzero::WahAutoEngage ae;
    ae.configure(0.20f, 10, 50, 0.05f);
    ae.reset(2000.0);

    CHECK(ae.update(0.30f, 2000.0) == 0);
    CHECK(ae.update(0.30f, 2015.0) == 1);
    CHECK(ae.engaged());

    // Fuori dalla finestra di accensione ma ben sopra quella di rilascio.
    CHECK(ae.update(0.10f, 2100.0) == 0);
    CHECK(ae.update(0.10f, 2300.0) == 0);
    CHECK(ae.update(0.10f, 3000.0) == 0);
    CHECK(ae.engaged());

    // Sotto la soglia di rilascio, ma non abbastanza a lungo.
    CHECK(ae.update(0.01f, 3100.0) == 0);
    CHECK(ae.engaged());
    // Dopo 50 ms sotto: spento.
    CHECK(ae.update(0.01f, 3151.0) == -1);
    CHECK(!ae.engaged());
}

// Polarita' del gate: con invert la punta (pos 1) e' la zona spenta.
static void test_inverted_polarity() {
    CHECK(namzero::wah_activation(0.0f, false) == 0.0f);
    CHECK(namzero::wah_activation(1.0f, false) == 1.0f);
    CHECK(namzero::wah_activation(0.0f, true) == 1.0f);
    CHECK(namzero::wah_activation(1.0f, true) == 0.0f);
    CHECK(namzero::wah_activation(0.25f, true) == 0.75f);

    namzero::WahAutoEngage ae;
    ae.configure(0.10f, 100, 500, 0.05f);
    ae.reset(0.0);

    // Punta a fondo per un secondo: resta spento.
    for (int t = 0; t <= 1000; t += 50) {
        CHECK(ae.update(namzero::wah_activation(1.0f, true), t) == 0);
    }
    CHECK(!ae.engaged());

    // Si lascia la punta (pos 0.80 -> attivazione 0.20): si accende dopo 100 ms.
    CHECK(ae.update(namzero::wah_activation(0.80f, true), 1100.0) == 0);
    CHECK(ae.update(namzero::wah_activation(0.80f, true), 1210.0) == 1);
    CHECK(ae.engaged());

    // Torna sulla punta: si spegne dopo 500 ms.
    CHECK(ae.update(namzero::wah_activation(1.0f, true), 1300.0) == 0);
    CHECK(ae.update(namzero::wah_activation(1.0f, true), 1799.0) == 0);
    CHECK(ae.update(namzero::wah_activation(1.0f, true), 1801.0) == -1);
    CHECK(!ae.engaged());
}

// Proprieta' dello slot mod: si salva com'era e si ripristina uguale.
static void test_arbiter() {
    using namzero::ModArbiter;

    // Tremolo acceso -> il pedale accende il wah -> al rilascio torna il tremolo.
    {
        ModArbiter ar;
        ar.engage({true, 0});
        CHECK(ar.engaged());
        // Un secondo engage non deve sovrascrivere il salvataggio.
        ar.engage({false, 0});
        const auto st = ar.release();
        CHECK(st.on);
        CHECK(st.type == 0);
        CHECK(!ar.engaged());
        // Release a vuoto: neutro, non rispolvera il salvataggio vecchio.
        const auto again = ar.release();
        CHECK(!again.on);
        CHECK(again.type == 0);
    }

    // Slot spento -> al rilascio resta spento.
    {
        ModArbiter ar;
        ar.engage({false, 0});
        const auto st = ar.release();
        CHECK(!st.on);
    }

    // Stato con wah gia' selezionato ma spento: torna spento, stesso tipo.
    {
        ModArbiter ar;
        ar.engage({false, 1});
        const auto st = ar.release();
        CHECK(!st.on);
        CHECK(st.type == 1);
    }
}

int main() {
    test_gate();
    test_hysteresis();
    test_inverted_polarity();
    test_arbiter();
    if (g_fail != 0) {
        std::fprintf(stderr, "wah_autoengage: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("wah_autoengage: gate, isteresi e ripristino dello slot ok\n");
    return 0;
}
