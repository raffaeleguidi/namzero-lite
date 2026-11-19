#include "control/presets_config.h"

#include <fstream>

#include "json.hpp"

namespace namzero {

namespace {

bool get_number(const nlohmann::json& j, const char* key, double& out, std::string& err,
                const std::string& where) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number()) {
        err = where + ": \"" + key + "\" deve essere un numero";
        return false;
    }
    out = j[key].get<double>();
    return true;
}

}  // namespace

bool load_presets_config(const std::string& path, PresetsConfig& out, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "impossibile leggere " + path;
        return false;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        err = path + ": JSON non valido (" + e.what() + ")";
        return false;
    }
    if (!j.is_object()) {
        err = path + ": la radice deve essere un oggetto";
        return false;
    }

    if (j.contains("gate")) {
        const auto& g = j["gate"];
        if (!g.is_object()) {
            err = "presets.json: \"gate\" deve essere un oggetto";
            return false;
        }
        if (!get_number(g, "threshold_db", out.gate.threshold_db, err, "gate")) return false;
        if (!get_number(g, "attack_ms", out.gate.attack_ms, err, "gate")) return false;
        if (!get_number(g, "release_ms", out.gate.release_ms, err, "gate")) return false;
    }
    if (out.gate.threshold_db < -100.0 || out.gate.threshold_db > 0.0) {
        err = "presets.json: gate.threshold_db deve stare fra -100 e 0";
        return false;
    }
    if (out.gate.attack_ms < 0.0 || out.gate.attack_ms > 100.0) {
        err = "presets.json: gate.attack_ms deve stare fra 0 e 100";
        return false;
    }
    if (out.gate.release_ms < 0.0 || out.gate.release_ms > 20000.0) {
        err = "presets.json: gate.release_ms deve stare fra 0 e 20000";
        return false;
    }

    if (j.contains("slots")) {
        const auto& slots = j["slots"];
        if (!slots.is_array()) {
            err = "presets.json: \"slots\" deve essere un array di file";
            return false;
        }
        for (std::size_t i = 0; i < slots.size() && i < out.slots.size(); ++i) {
            if (slots[i].is_null()) continue;
            if (!slots[i].is_string()) {
                err = "presets.json: ogni slot deve essere un percorso o null";
                return false;
            }
            out.slots[i] = slots[i].get<std::string>();
        }
    }

    if (j.contains("start_slot")) {
        if (!j["start_slot"].is_number_integer()) {
            err = "presets.json: \"start_slot\" deve essere un intero";
            return false;
        }
        out.start_slot = j["start_slot"].get<int>();
        if (out.start_slot < -1 || out.start_slot > 9) {
            err = "presets.json: start_slot deve stare fra -1 (ultimo usato) e 9";
            return false;
        }
    }

    return true;
}

}  // namespace namzero
