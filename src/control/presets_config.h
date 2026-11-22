#pragma once

#include <array>
#include <cstddef>
#include <string>

namespace namzero {

// Tipi del blocco FILTER (fisso prima del drive). "wah" e' il default storico:
// un preset senza "type" continua a suonare come prima. "whammy" usa il pedale
// di espressione per trasporre di 0..range semitoni.
constexpr const char* kFilterTypeNames[] = {"wah", "whammy"};
constexpr std::size_t kFilterTypeCount =
    sizeof(kFilterTypeNames) / sizeof(kFilterTypeNames[0]);

inline int filter_type_index(const std::string& name) {
    for (std::size_t i = 0; i < kFilterTypeCount; ++i) {
        if (name == kFilterTypeNames[i]) return static_cast<int>(i);
    }
    return -1;
}

// Limiti del range del whammy, in semitoni. Il segno decide la direzione.
constexpr double kFilterRangeMin = -24.0;
constexpr double kFilterRangeMax = 24.0;
constexpr double kFilterRangeDefault = 12.0;
constexpr double kFilterMixDefault = 1.0;

// Parametri del gate validi per tutti i preset: il singolo preset dice solo se
// il gate e' acceso. Valori di partenza pensati per non tagliare le code:
// soglia bassa e release lungo.
struct GateSettings {
    double threshold_db = -70.0;
    double attack_ms = 2.0;
    double release_ms = 3000.0;
};

// config/presets.json: slot 0-9 richiamabili da tasto e Program Change.
struct PresetsConfig {
    GateSettings gate;
    std::array<std::string, 10> slots;  // vuoto = slot libero
    int start_slot = -1;                // -1 = ultimo usato
};

bool load_presets_config(const std::string& path, PresetsConfig& out, std::string& err);

}  // namespace namzero
