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

// Pedale momentaneo: 127 alla pressione, 0 al rilascio. Il rilascio non deve
// mai contare, e tenere premuto non deve produrre un secondo scatto.
static void test_momentary() {
    namzero::SwitchDebounce sw;
    CHECK(sw.pressed(10, 127, 0.0));
    CHECK(!sw.pressed(10, 0, 400.0));    // rilascio dopo 400 ms
    CHECK(sw.pressed(10, 127, 800.0));   // nuova pressione
    CHECK(!sw.pressed(10, 0, 5000.0));   // premuto a lungo: il rilascio tace
    CHECK(sw.pressed(10, 127, 5400.0));
}

// Rimbalzo: 127, 0, 127 nel giro di pochi ms = una sola pressione.
static void test_bounce() {
    namzero::SwitchDebounce sw;
    CHECK(sw.pressed(20, 127, 0.0));
    CHECK(!sw.pressed(20, 0, 10.0));
    CHECK(!sw.pressed(20, 127, 20.0));
    CHECK(!sw.pressed(20, 0, 30.0));
    CHECK(sw.pressed(20, 127, 400.0));
}

// Controller che manda solo 127, senza rilascio: due pressioni si distinguono
// solo dal tempo.
static void test_only_press() {
    namzero::SwitchDebounce sw;
    CHECK(sw.pressed(30, 127, 0.0));
    CHECK(!sw.pressed(30, 127, 300.0));   // troppo vicina: probabilmente la stessa
    CHECK(sw.pressed(30, 127, 800.0));    // dopo la finestra: nuova pressione
}

// Canali CC diversi non si influenzano.
static void test_independent() {
    namzero::SwitchDebounce sw;
    CHECK(sw.pressed(10, 127, 0.0));
    CHECK(sw.pressed(20, 127, 10.0));
    CHECK(sw.pressed(11, 127, 20.0));
    CHECK(!sw.pressed(10, 127, 30.0));
}

int main() {
    test_momentary();
    test_bounce();
    test_only_press();
    test_independent();
    if (g_fail != 0) {
        std::fprintf(stderr, "switch_debounce: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("switch_debounce: fronte di salita, rimbalzi e soli-127 ok\n");
    return 0;
}
