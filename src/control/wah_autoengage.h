#pragma once

namespace namzero {

// Macchina a stati per l'accensione automatica del wah comandata dal pedale.
//
// Specifica: si accende quando la posizione resta sopra soglia per engage_ms,
// si spegne quando resta sotto per release_ms. I due tempi sono asimmetrici di
// proposito: accendere deve essere immediato, spegnere non deve tagliare una
// nota che sta sfumando.
//
// E' separata dal resto perche' e' logica con tempi precisi: isolata si puo'
// verificare con un test, dentro un lambda no.
class WahAutoEngage {
public:
    void configure(float threshold, int engage_ms, int release_ms) {
        threshold_ = threshold;
        engage_ms_ = engage_ms;
        release_ms_ = release_ms;
    }

    void reset(double now_ms) {
        engaged_ = false;
        above_ = false;
        below_ = true;
        above_since_ = now_ms;
        below_since_ = now_ms;
    }

    // Aggiorna lo stato. Ritorna:
    //    1 appena acceso
    //   -1 appena spento
    //    0 nessun cambiamento
    int update(float position, double now_ms) {
        if (position > threshold_) {
            if (!above_) {
                above_ = true;
                above_since_ = now_ms;
                below_ = false;
            }
        } else {
            if (!below_) {
                below_ = true;
                below_since_ = now_ms;
                above_ = false;
            }
        }

        if (!engaged_ && above_ && (now_ms - above_since_) >= engage_ms_) {
            engaged_ = true;
            return 1;
        }
        if (engaged_ && below_ && (now_ms - below_since_) >= release_ms_) {
            engaged_ = false;
            return -1;
        }
        return 0;
    }

    bool engaged() const { return engaged_; }

private:
    float threshold_ = 0.10f;
    int engage_ms_ = 100;
    int release_ms_ = 500;

    bool engaged_ = false;
    bool above_ = false;
    bool below_ = true;
    double above_since_ = 0.0;
    double below_since_ = 0.0;
};

}  // namespace namzero
