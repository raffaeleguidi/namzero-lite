#pragma once

namespace namzero {

// Valore su cui lavora il gate. Di norma la posizione del pedale (tacco 0,
// punta 1): si accende verso la punta. Con invert la polarita' si rovescia e
// l'accesso sta verso il tacco, mentre la punta e' la zona di riposo/spento.
//
// Nota: e' diverso da expr_invert, che rovescia la mappatura della posizione
// (e quindi anche la corsa del filtro). Qui il filtro continua a seguire il
// pedale normalmente: cambia solo dove il gate considera "acceso".
inline float wah_activation(float position, bool invert) {
    return invert ? 1.0f - position : position;
}

// Macchina a stati per l'accensione automatica del wah comandata dal pedale.
//
// Specifica: si accende quando l'attivazione resta sopra soglia per engage_ms,
// si spegne quando resta sotto per release_ms. I due tempi sono asimmetrici di
// proposito: accendere deve essere immediato, spegnere non deve tagliare una
// nota che sta sfumando.
//
// Le due soglie sono separate: una isteresi di posizione evita che il pedale
// appoggiato a meta' corsa faccia sfarfallare on/off sopra il punto di scatto.
//
// E' separata dal resto perche' e' logica con tempi precisi: isolata si puo'
// verificare con un test, dentro un lambda no.
class WahAutoEngage {
public:
    // release_threshold < 0 significa "uguale a threshold" (comportamento
    // storico, una sola soglia).
    void configure(float threshold, int engage_ms, int release_ms,
                   float release_threshold = -1.0f) {
        threshold_ = threshold;
        engage_ms_ = engage_ms;
        release_ms_ = release_ms;
        release_threshold_ = release_threshold < 0.0f ? threshold : release_threshold;
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
        const bool above = position > threshold_;
        const bool below = position < release_threshold_;

        if (above != above_) {
            above_ = above;
            if (above) above_since_ = now_ms;
        }
        if (below != below_) {
            below_ = below;
            if (below) below_since_ = now_ms;
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
    float release_threshold_ = 0.10f;
    int engage_ms_ = 100;
    int release_ms_ = 500;

    bool engaged_ = false;
    bool above_ = false;
    bool below_ = true;
    double above_since_ = 0.0;
    double below_since_ = 0.0;
};

// Sovraintende alla proprieta' dello slot mod, che e' condiviso.
//
// Regola: il wah non e' mai "acceso" di suo, lo accende solo l'auto-engage.
// Quando si accende, lo stato corrente dello slot (acceso/spento e tipo) viene
// messo da parte; quando si spegne, quello stato torna esattamente com'era.
// L'arbitro non tocca il DSP: dice solo cosa salvare e cosa ripristinare, cosi'
// la regola si puo' testare senza audio.
class ModArbiter {
public:
    struct State {
        bool on = false;
        bool wah = false;  // true = tipo wah, false = tremolo
    };

    // Il gate ha acceso il wah: memorizza lo stato da ripristinare.
    // Idempotente: un secondo engage senza release non sovrascrive il salvataggio.
    void engage(const State& current) {
        if (engaged_) return;
        saved_ = current;
        engaged_ = true;
    }

    // Il gate ha spento il wah: ritorna lo stato precedente.
    // Chiamata a vuoto (senza engage) ritorna "spento, tremolo": non deve
    // ripristinare lo stato di un ingaggio vecchio.
    State release() {
        if (!engaged_) return State{};
        engaged_ = false;
        return saved_;
    }

    bool engaged() const { return engaged_; }

private:
    bool engaged_ = false;
    State saved_{};
};

}  // namespace namzero
