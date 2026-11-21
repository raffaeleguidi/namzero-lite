#pragma once

#include <cstddef>
#include <string>

namespace namzero {

// Tipi di modulazione, in ordine di ciclo. La stessa tabella serve a validare i
// preset, al tasto `w`, alla mappa `mod_select` di midi.json e ai Program Change
// del G-Board.
//
// Il wah NON e' un tipo di mod: vive nel blocco FILTER separato, prima del
// drive, e si accende solo con l'auto-engage dell'espressione. Un preset che
// contiene ancora "wah" viene letto come tremolo spento (compatibilita').
constexpr const char* kModTypeNames[] = {"tremolo", "univibe", "flanger", "phaser"};
constexpr std::size_t kModTypeCount = sizeof(kModTypeNames) / sizeof(kModTypeNames[0]);

inline int mod_type_index(const std::string& name) {
    for (std::size_t i = 0; i < kModTypeCount; ++i) {
        if (name == kModTypeNames[i]) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace namzero
