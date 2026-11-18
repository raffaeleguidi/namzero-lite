#include <cstdio>

#include "control/switch_debounce.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

using Mode = namzero::SwitchDebounce::Mode;

// Controller "a stato": un messaggio per pressione, il valore alterna 127/0.
// Con questo hardware il valore non conta: anche 0 e' una pressione.
static void test_any() {
    namzero::SwitchDebounce sw;  // default: kAny
    CHECK(sw.pressed(10, 0, 0.0));      // prima pressione, valore basso
    CHECK(sw.pressed(10, 127, 600.0));  // seconda pressione
    CHECK(sw.pressed(10, 0, 1200.0));   // terza
    CHECK(sw.pressed(20, 0, 1300.0));   // CC diverso, indipendente
    // Rimbalzo/raffica: due messaggi ravvicinati sono una sola pressione.
    CHECK(!sw.pressed(10, 127, 1250.0));
}

// Controller momentaneo: 127 alla pressione, 0 al rilascio.
static void test_edge_momentary() {
    namzero::SwitchDebounce sw;
    sw.set_mode(Mode::kEdge);
    CHECK(sw.pressed(10, 127, 0.0));
    CHECK(!sw.pressed(10, 0, 400.0));    // rilascio dopo 400 ms: non conta
    CHECK(sw.pressed(10, 127, 800.0));   // nuova pressione
    CHECK(!sw.pressed(10, 0, 5000.0));   // premuto a lungo: il rilascio tace
    CHECK(sw.pressed(10, 127, 5400.0));
}

// Rimbalzo in modalita' edge: 127, 0, 127 in pochi ms = una sola pressione.
static void test_edge_bounce() {
    namzero::SwitchDebounce sw;
    sw.set_mode(Mode::kEdge);
    CHECK(sw.pressed(20, 127, 0.0));
    CHECK(!sw.pressed(20, 0, 10.0));
    CHECK(!sw.pressed(20, 127, 20.0));
    CHECK(!sw.pressed(20, 0, 30.0));
    CHECK(sw.pressed(20, 127, 400.0));
}

// Controller che manda solo 127, senza rilascio: due pressioni si distinguono
// solo dal tempo.
static void test_edge_only_press() {
    namzero::SwitchDebounce sw;
    sw.set_mode(Mode::kEdge);
    CHECK(sw.pressed(30, 127, 0.0));
    CHECK(!sw.pressed(30, 127, 300.0));  // troppo vicina: probabilmente la stessa
    CHECK(sw.pressed(30, 127, 800.0));   // dopo la finestra: nuova pressione
}

int main() {
    test_any();
    test_edge_momentary();
    test_edge_bounce();
    test_edge_only_press();
    if (g_fail != 0) {
        std::fprintf(stderr, "switch_debounce: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("switch_debounce: modalita' a stato e momentanea ok\n");
    return 0;
}
