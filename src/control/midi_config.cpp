#include "control/midi_config.h"

#include <fstream>

namespace namzero {
namespace {

bool in_cc_range(int cc) { return cc >= 0 && cc <= 127; }

// Legge un intero opzionale; false solo se il campo c'e' ed e' del tipo sbagliato.
bool get_int(const nlohmann::json& j, const char* key, int& out, std::string& err) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number_integer()) {
        err = std::string("midi.json: \"") + key + "\" deve essere un intero";
        return false;
    }
    out = j[key].get<int>();
    return true;
}

bool get_num(const nlohmann::json& j, const char* key, double& out, std::string& err) {
    if (!j.contains(key)) return true;
    if (!j[key].is_number()) {
        err = std::string("midi.json: \"") + key + "\" deve essere un numero";
        return false;
    }
    out = j[key].get<double>();
    return true;
}

bool get_bool(const nlohmann::json& j, const char* key, bool& out) {
    if (!j.contains(key) || !j[key].is_boolean()) return true;
    out = j[key].get<bool>();
    return true;
}

int action_index(const std::string& name) {
    for (std::size_t i = 0; i < kMidiActionCount; ++i) {
        if (name == midi_action_name(static_cast<MidiAction>(i))) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

bool parse_midi_config(const nlohmann::json& j, MidiConfig& cfg, std::string& err) {
    if (!j.is_object()) {
        err = "midi.json: la radice deve essere un oggetto";
        return false;
    }

    get_bool(j, "enabled", cfg.enabled);
    if (j.contains("device") && j["device"].is_string()) cfg.device = j["device"].get<std::string>();
    if (j.contains("switches")) {
        if (!j["switches"].is_string()) {
            err = "midi.json: \"switches\" deve essere \"any\" oppure \"edge\"";
            return false;
        }
        cfg.switches = j["switches"].get<std::string>();
        if (cfg.switches != "any" && cfg.switches != "edge") {
            err = "midi.json: \"switches\" vale \"" + cfg.switches + "\" (atteso any o edge)";
            return false;
        }
    }

    if (j.contains("expression")) {
        const auto& e = j["expression"];
        if (!e.is_object()) {
            err = "midi.json: \"expression\" deve essere un oggetto";
            return false;
        }
        if (!get_int(e, "cc", cfg.expression_cc, err)) return false;
        if (!get_int(e, "min_raw", cfg.expression_min_raw, err)) return false;
        if (!get_int(e, "max_raw", cfg.expression_max_raw, err)) return false;
        get_bool(e, "invert", cfg.expression_invert);
    }

    if (j.contains("wah_autoengage")) {
        const auto& w = j["wah_autoengage"];
        if (!w.is_object()) {
            err = "midi.json: \"wah_autoengage\" deve essere un oggetto";
            return false;
        }
        if (!get_num(w, "threshold", cfg.wah_threshold, err)) return false;
        if (!get_num(w, "release_threshold", cfg.wah_release_threshold, err)) return false;
        if (!get_int(w, "engage_ms", cfg.wah_engage_ms, err)) return false;
        if (!get_int(w, "release_ms", cfg.wah_release_ms, err)) return false;
        get_bool(w, "invert", cfg.wah_invert);
    }

    if (j.contains("cc")) {
        const auto& cc = j["cc"];
        if (!cc.is_object()) {
            err = "midi.json: \"cc\" deve essere un oggetto {azione: numero}";
            return false;
        }
        for (auto it = cc.begin(); it != cc.end(); ++it) {
            const int idx = action_index(it.key());
            if (idx < 0) {
                err = "midi.json: azione sconosciuta \"" + it.key() +
                      "\" (attese: gate, comp, drive, mod, amp, ir, delay, reverb)";
                return false;
            }
            if (!it.value().is_number_integer()) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve essere un intero";
                return false;
            }
            const int value = it.value().get<int>();
            if (value != -1 && !in_cc_range(value)) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve stare fra 0 e 127 (o -1)";
                return false;
            }
            cfg.slot_cc[static_cast<std::size_t>(idx)] = value;
        }
    }

    // Vincoli che rendono il file usabile: un CC duplicato accenderebbe due
    // blocchi insieme, e l'espressione non puo' coincidere con un footswitch.
    if (!in_cc_range(cfg.expression_cc)) {
        err = "midi.json: expression.cc deve stare fra 0 e 127";
        return false;
    }
    for (std::size_t i = 0; i < kMidiActionCount; ++i) {
        const int cc = cfg.slot_cc[i];
        if (cc < 0) continue;
        if (!in_cc_range(cc)) {
            err = std::string("midi.json: CC fuori range per ") +
                  midi_action_name(static_cast<MidiAction>(i));
            return false;
        }
        if (cc == cfg.expression_cc) {
            err = std::string("midi.json: ") + midi_action_name(static_cast<MidiAction>(i)) +
                  " usa lo stesso CC dell'espressione";
            return false;
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (cfg.slot_cc[k] == cc) {
                err = std::string("midi.json: ") + midi_action_name(static_cast<MidiAction>(k)) +
                      " e " + midi_action_name(static_cast<MidiAction>(i)) +
                      " hanno lo stesso CC";
                return false;
            }
        }
    }

    if (cfg.wah_engage_ms < 0 || cfg.wah_release_ms < 0) {
        err = "midi.json: engage_ms e release_ms devono essere positivi";
        return false;
    }
    return true;
}

bool load_midi_config(const std::string& path, MidiConfig& cfg, std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "midi.json: impossibile leggere " + path;
        return false;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        err = "midi.json: " + path + ": " + e.what();
        return false;
    }
    return parse_midi_config(j, cfg, err);
}

}  // namespace namzero
