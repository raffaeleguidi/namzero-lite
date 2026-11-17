#pragma once

#include <cstddef>

namespace namzero {

// Interfaccia comune di uno slot della catena.
// process() lavora in-place su un buffer interleaved stereo in float.
class Slot {
public:
    virtual ~Slot() = default;

    virtual void prepare(double sample_rate, std::size_t max_frames) = 0;
    virtual void process(float* interleaved, std::size_t frames) = 0;
    virtual void reset() = 0;
    virtual const char* name() const = 0;

    // Latenza introdotta dallo slot, in campioni. La somma sulla catena va
    // dichiarata all'utente: e' quella che si somma al round-trip.
    virtual std::size_t latency_frames() const { return 0; }
};

}  // namespace namzero
