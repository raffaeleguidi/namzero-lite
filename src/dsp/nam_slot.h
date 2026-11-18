#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "dsp/slot.h"

namespace namzero {

// Slot amp: esegue un modello NAM tramite NeuralAmpModelerCore.
//
// Il core richiede C++20 e trascina Eigen: gli header restano confinati in
// nam_slot.cpp dietro un Pimpl, cosi' il resto del motore resta C++17.
//
// Il modello e' mono: si somma l'ingresso stereo, si processa, si replica
// l'uscita sui due canali.
class NamSlot final : public Slot {
public:
    NamSlot();
    ~NamSlot() override;

    NamSlot(const NamSlot&) = delete;
    NamSlot& operator=(const NamSlot&) = delete;

    // slim in [0,1]: per i modelli SlimmableContainer valori < 0.5 selezionano
    // il sottomodello piccolo. Viene applicato solo se il modello lo supporta.
    bool load(const std::string& path, double slim, std::string& err);

    void prepare(double sample_rate, std::size_t max_frames) override;
    void process(float* interleaved, std::size_t frames) override;
    void reset() override;

    // Trim utente in dB, applicato sopra al gain dichiarato dal modello.
    void set_trim_db(double db);
    const char* name() const override { return "amp"; }

    double expected_sample_rate() const;
    bool slimmable() const;
    bool loaded() const;
    double model_gain() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace namzero
