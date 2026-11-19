#pragma once

#include <cstddef>
#include <string>

namespace namzero {

// Modi del delay. L'ordine combacia con DelaySlot::Mode (kStereo, kPingPong,
// kPingPongStereo): la stessa tabella serve a validare i preset, al tasto `p`,
// alla mappa delay_select di midi.json e al ripristino dello stato.
constexpr const char* kDelayModeNames[] = {"stereo", "ping_pong", "ping_pong_stereo"};
constexpr std::size_t kDelayModeCount = sizeof(kDelayModeNames) / sizeof(kDelayModeNames[0]);

inline int delay_mode_index(const std::string& name) {
    for (std::size_t i = 0; i < kDelayModeCount; ++i) {
        if (name == kDelayModeNames[i]) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace namzero
