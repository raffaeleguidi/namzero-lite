#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "dsp/slot.h"

namespace namzero {

// Catena a slot fissi, processata nell'ordine di inserimento.
// Il bypass di ogni slot e' un crossfade lineare di 5 ms tra dry e wet:
// niente click quando si accende/spegne un pedale.
class Chain {
public:
    void add(std::unique_ptr<Slot> slot);

    void prepare(double sample_rate, std::size_t max_frames);

    // Chiamabili dal thread di controllo; il thread audio legge atomici.
    void set_enabled(std::size_t index, bool on);
    bool enabled(std::size_t index) const;

    std::size_t size() const { return entries_.size(); }
    const char* name(std::size_t index) const { return entries_[index]->slot->name(); }

    void process(float* interleaved, std::size_t frames);

    // Latenza totale della catena, in campioni.
    std::size_t latency_frames() const;
    void reset();

private:
    struct Entry {
        std::unique_ptr<Slot> slot;
        std::atomic<bool> enabled{true};
        float mix = 1.0f;  // 0 = dry, 1 = wet
    };

    std::vector<std::unique_ptr<Entry>> entries_;
    std::vector<float> dry_;
    float ramp_ = 1.0f / 240.0f;
};

}  // namespace namzero
