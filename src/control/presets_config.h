#pragma once

#include <array>
#include <string>

namespace namzero {

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
