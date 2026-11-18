#include <cassert>
#include <cstdio>
#include "control/wah_autoengage.h"

int main() {
    namzero::WahAutoEngage ae;
    ae.configure(0.10f, 100, 500);
    ae.reset(0.0);

    // Pedale a fondo (heel down): niente, resta spento.
    assert(ae.update(0.0f, 10.0) == 0);
    assert(!ae.engaged());

    // Sale sopra soglia: non deve accendersi subito.
    assert(ae.update(0.5f, 100.0) == 0);
    assert(!ae.engaged());

    // A 199 ms (99 sopra soglia): ancora spento.
    assert(ae.update(0.5f, 199.0) == 0);
    assert(!ae.engaged());

    // A 201 ms (101 sopra soglia): si accende.
    assert(ae.update(0.5f, 201.0) == 1);
    assert(ae.engaged());

    // Scende sotto soglia: non si spegne subito.
    assert(ae.update(0.0f, 300.0) == 0);
    assert(ae.engaged());

    // A 799 ms (499 sotto): ancora acceso.
    assert(ae.update(0.0f, 799.0) == 0);
    assert(ae.engaged());

    // A 801 ms (501 sotto): si spegne.
    assert(ae.update(0.0f, 801.0) == -1);
    assert(!ae.engaged());

    // Rimbalzo breve sopra soglia (60 ms) poi giu': non deve accendersi.
    ae.reset(1000.0);
    ae.update(0.4f, 1000.0);
    assert(ae.update(0.4f, 1060.0) == 0);
    assert(ae.update(0.0f, 1061.0) == 0);
    assert(ae.update(0.0f, 1700.0) == 0);
    assert(!ae.engaged());

    std::printf("wah_autoengage: tutti i casi ok (soglie 100/500 ms, isteresi temporale)\n");
    return 0;
}
