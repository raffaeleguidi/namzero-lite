#pragma once

#include <cstddef>
#include <string>

namespace namzero {

// Tipi di modulazione, in ordine di ciclo. La stessa tabella serve a validare i
// preset, al tasto `w`, alla mappa `mod_select` di midi.json e al ripristino
// dello stato dello slot da parte dell'auto-engage.
//
// L'indice 1 (wah) non e' selezionabile da footswitch: si usa dal pedale.
constexpr const char* kModTypeNames[] = {"tremolo", "wah", "univibe", "flanger", "phaser"};
constexpr std::size_t kModTypeCount = sizeof(kModTypeNames) / sizeof(kModTypeNames[0]);

inline int mod_type_index(const std::string& name) {
    for (std::size_t i = 0; i < kModTypeCount; ++i) {
        if (name == kModTypeNames[i]) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace namzero
