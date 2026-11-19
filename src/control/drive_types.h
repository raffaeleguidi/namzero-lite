#pragma once

#include <cstddef>
#include <string>

namespace namzero {

// Tipi di drive, in ordine di ciclo. La stessa tabella serve a validare i
// preset, al tasto `k`, alla mappa `drive_select` di midi.json e al ripristino
// dello stato.
constexpr const char* kDriveTypeNames[] = {"fuzz", "klon", "tubescreamer"};
constexpr std::size_t kDriveTypeCount = sizeof(kDriveTypeNames) / sizeof(kDriveTypeNames[0]);

inline int drive_type_index(const std::string& name) {
    for (std::size_t i = 0; i < kDriveTypeCount; ++i) {
        if (name == kDriveTypeNames[i]) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace namzero
