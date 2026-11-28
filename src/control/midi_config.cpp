#include "control/midi_config.h"

#include <fstream>
#include <utility>

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

// Come get_bool, ma un valore del tipo sbagliato e' un errore: per le nuove
// chiavi G-Board vogliamo un file sbagliato che si ferma, non un default muto.
bool get_bool_strict(const nlohmann::json& j, const char* key, bool& out,
                     const char* section, std::string& err) {
    if (!j.contains(key)) return true;
    if (!j[key].is_boolean()) {
        err = std::string("midi.json: ") + section + "." + key +
              " deve essere true o false";
        return false;
    }
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

GboardPcRoute route_gboard_pc_effects(const MidiConfig& cfg, int pc) {
    GboardPcRoute route;
    if (pc < 0 || !in_cc_range(pc)) return route;
    if (!cfg.gboard_pc_effects) return route;
    // L'ordine (mod, boost, drive) e' quello del vecchio codice.
    for (std::size_t i = 0; i < cfg.gboard_mod_pc.size(); ++i) {
        if (cfg.gboard_mod_pc[i] == pc) {
            route.action = GboardPcAction::kMod;
            route.index = static_cast<int>(i);
            return route;
        }
    }
    if (cfg.gboard_boost_pc >= 0 && cfg.gboard_boost_pc == pc) {
        route.action = GboardPcAction::kBoost;
        route.index = 0;
        return route;
    }
    for (std::size_t i = 0; i < cfg.gboard_drive_pc.size(); ++i) {
        if (cfg.gboard_drive_pc[i] == pc) {
            route.action = GboardPcAction::kDrive;
            route.index = static_cast<int>(i);
            return route;
        }
    }
    return route;
}

GboardPcRoute route_gboard_pc(const MidiConfig& cfg, int pc) {
    GboardPcRoute route;
    if (pc < 0 || !in_cc_range(pc)) return route;

    // Program mode: PC 0-7 = slot 0-7. Ha la priorita' anche se pc_effects e'
    // true, cosi' un PC non puo' fare due cose insieme.
    if (cfg.gboard_preset_pc && pc <= 7) {
        route.action = GboardPcAction::kPreset;
        route.index = pc;
        return route;
    }

    return route_gboard_pc_effects(cfg, pc);
}

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
        if (e.contains("ccs")) {
            if (!e["ccs"].is_array() || e["ccs"].empty() || e["ccs"].size() > 4) {
                err = "midi.json: expression.ccs deve essere un array di 1-4 CC";
                return false;
            }
            cfg.expression_ccs.clear();
            for (const auto& v : e["ccs"]) {
                if (!v.is_number_integer() || !in_cc_range(v.get<int>())) {
                    err = "midi.json: expression.ccs fuori range";
                    return false;
                }
                cfg.expression_ccs.push_back(v.get<int>());
            }
            cfg.expression_cc = cfg.expression_ccs.front();
        } else if (!get_int(e, "cc", cfg.expression_cc, err)) {
            return false;
        } else {
            cfg.expression_ccs = {cfg.expression_cc};
        }
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

    if (j.contains("ble")) {
        const auto& ble = j["ble"];
        if (!ble.is_object()) {
            err = "midi.json: \"ble\" deve essere un oggetto";
            return false;
        }
        if (ble.contains("socket") && ble["socket"].is_string()) {
            cfg.ble_socket = ble["socket"].get<std::string>();
        }
    }

    if (j.contains("mod_select")) {
        const auto& ms = j["mod_select"];
        if (!ms.is_object()) {
            err = "midi.json: \"mod_select\" deve essere un oggetto {tipo: CC}";
            return false;
        }
        for (auto it = ms.begin(); it != ms.end(); ++it) {
            const int idx = mod_type_index(it.key());
            if (idx < 0) {
                err = "midi.json: tipo di modulazione sconosciuto \"" + it.key() +
                      "\" (attesi: tremolo, univibe, flanger, phaser; "
                      "il wah sta nel blocco filter e non ha CC)";
                return false;
            }
            if (!it.value().is_number_integer()) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve essere un intero";
                return false;
            }
            const int value = it.value().get<int>();
            if (!in_cc_range(value)) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve stare fra 0 e 127";
                return false;
            }
            cfg.mod_select_cc[static_cast<std::size_t>(idx)] = value;
        }
    }

    if (j.contains("drive_select")) {
        const auto& ds = j["drive_select"];
        if (!ds.is_object()) {
            err = "midi.json: \"drive_select\" deve essere un oggetto {tipo: CC}";
            return false;
        }
        for (auto it = ds.begin(); it != ds.end(); ++it) {
            const int idx = drive_type_index(it.key());
            if (idx < 0) {
                err = "midi.json: tipo di drive sconosciuto \"" + it.key() +
                      "\" (attesi: fuzz, klon, tubescreamer)";
                return false;
            }
            if (!it.value().is_number_integer()) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve essere un intero";
                return false;
            }
            const int value = it.value().get<int>();
            if (!in_cc_range(value)) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve stare fra 0 e 127";
                return false;
            }
            cfg.drive_select_cc[static_cast<std::size_t>(idx)] = value;
        }
    }

    if (j.contains("gboard")) {
        const auto& gb = j["gboard"];
        if (!gb.is_object()) {
            err = "midi.json: \"gboard\" deve essere un oggetto";
            return false;
        }
        if (gb.contains("device")) {
            if (!gb["device"].is_string()) {
                err = "midi.json: gboard.device deve essere una stringa";
                return false;
            }
            cfg.gboard_device = gb["device"].get<std::string>();
        }
        // Nuovo uso della G-Board: PC 0-7 = preset, CC = effetti. I due flag
        // sono indipendenti; se sono entrambi true i PC 0-7 restano preset
        // (route_gboard_pc) e main.cpp stampa un avviso all'avvio.
        if (!get_bool_strict(gb, "preset_pc", cfg.gboard_preset_pc, "gboard", err)) {
            return false;
        }
        if (!get_bool_strict(gb, "pc_effects", cfg.gboard_pc_effects, "gboard", err)) {
            return false;
        }
        // Canali dei PC (1-16): effetti e preset separati.
        {
            auto read_channel = [&](const char* key, int& out) -> bool {
                if (!gb.contains(key)) return true;
                if (!gb[key].is_number_integer()) {
                    err = std::string("midi.json: gboard.") + key + " deve essere un intero";
                    return false;
                }
                const int v = gb[key].get<int>();
                if (v < 1 || v > 16) {
                    err = std::string("midi.json: gboard.") + key + " fuori range (1-16)";
                    return false;
                }
                out = v;
                return true;
            };
            if (!read_channel("pc_effects_channel", cfg.gboard_pc_effects_channel)) {
                return false;
            }
            if (!read_channel("pc_preset_channel", cfg.gboard_pc_preset_channel)) {
                return false;
            }
        }
        // Legge un array di `count` interi. -1 disattiva la singola voce.
        auto read_ints = [&](const char* key, int* out, std::size_t count) -> bool {
            if (!gb.contains(key)) return true;
            const auto& arr = gb[key];
            if (!arr.is_array() || arr.size() != count) {
                err = std::string("midi.json: gboard.") + key + " deve avere " +
                      std::to_string(count) + " elementi";
                return false;
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!arr[i].is_number_integer()) {
                    err = std::string("midi.json: gboard.") + key +
                          " deve contenere interi";
                    return false;
                }
                const int v = arr[i].get<int>();
                if (v != -1 && !in_cc_range(v)) {
                    err = std::string("midi.json: gboard.") + key +
                          " fuori range (0-127, oppure -1)";
                    return false;
                }
                out[i] = v;
            }
            return true;
        };

        const bool has_mod_led = gb.contains("mod_led");
        const bool has_drive_led = gb.contains("drive_led");
        const bool has_boost_led = gb.contains("boost_led");

        if (!read_ints("mod_cc", cfg.gboard_mod_cc.data(), cfg.gboard_mod_cc.size())) {
            return false;
        }
        if (!read_ints("drive_cc", cfg.gboard_drive_cc.data(),
                       cfg.gboard_drive_cc.size())) {
            return false;
        }
        if (gb.contains("boost_cc")) {
            if (!gb["boost_cc"].is_number_integer()) {
                err = "midi.json: gboard.boost_cc deve essere un intero";
                return false;
            }
            cfg.gboard_boost_cc = gb["boost_cc"].get<int>();
            if (cfg.gboard_boost_cc != -1 && !in_cc_range(cfg.gboard_boost_cc)) {
                err = "midi.json: gboard.boost_cc fuori range (0-127, oppure -1)";
                return false;
            }
        }
        if (!read_ints("mod_led", cfg.gboard_mod_led.data(),
                       cfg.gboard_mod_led.size())) {
            return false;
        }
        if (!read_ints("drive_led", cfg.gboard_drive_led.data(),
                       cfg.gboard_drive_led.size())) {
            return false;
        }
        if (gb.contains("boost_led")) {
            if (!gb["boost_led"].is_number_integer()) {
                err = "midi.json: gboard.boost_led deve essere un intero";
                return false;
            }
            cfg.gboard_boost_led = gb["boost_led"].get<int>();
            if (cfg.gboard_boost_led != -1 && !in_cc_range(cfg.gboard_boost_led)) {
                err = "midi.json: gboard.boost_led fuori range (0-127, oppure -1)";
                return false;
            }
        }
        if (!read_ints("mod_pc", cfg.gboard_mod_pc.data(), cfg.gboard_mod_pc.size())) {
            return false;
        }
        if (!read_ints("drive_pc", cfg.gboard_drive_pc.data(),
                       cfg.gboard_drive_pc.size())) {
            return false;
        }
        if (gb.contains("boost_pc")) {
            if (!gb["boost_pc"].is_number_integer()) {
                err = "midi.json: gboard.boost_pc deve essere un intero";
                return false;
            }
            cfg.gboard_boost_pc = gb["boost_pc"].get<int>();
            if (cfg.gboard_boost_pc != -1 && !in_cc_range(cfg.gboard_boost_pc)) {
                err = "midi.json: gboard.boost_pc fuori range (0-127, oppure -1)";
                return false;
            }
        }
        // Retrocompatibilita': nei file vecchi lo stesso numero era sia PC sia
        // nota LED. Se manca il campo LED esplicito ma c'e' il PC, si continua
        // a usare quello; se mancano entrambi restano i default 0-3/4-6/7.
        if (!has_mod_led && gb.contains("mod_pc")) {
            cfg.gboard_mod_led = cfg.gboard_mod_pc;
        }
        if (!has_drive_led && gb.contains("drive_pc")) {
            cfg.gboard_drive_led = cfg.gboard_drive_pc;
        }
        if (!has_boost_led && gb.contains("boost_pc")) {
            cfg.gboard_boost_led = cfg.gboard_boost_pc;
        }

        get_bool(gb, "allow_cc_conflicts", cfg.gboard_allow_cc_conflicts);
        if (gb.contains("flash_ms")) {
            if (!gb["flash_ms"].is_number_integer()) {
                err = "midi.json: gboard.flash_ms deve essere un intero";
                return false;
            }
            cfg.gboard_flash_ms = gb["flash_ms"].get<int>();
            if (cfg.gboard_flash_ms < 0 || cfg.gboard_flash_ms > 2000) {
                err = "midi.json: gboard.flash_ms deve stare fra 0 e 2000";
                return false;
            }
        }
    }

    // Looper: stava per errore dentro il blocco gboard.flash_ms, quindi senza
    // flash_ms i tre CC non venivano letti.
    if (j.contains("looper")) {
        const auto& lp = j["looper"];
        if (!lp.is_object()) {
            err = "midi.json: \"looper\" deve essere un oggetto {toggle, stop, clear}";
            return false;
        }
        auto read_looper = [&](const char* key, int& out) -> bool {
            if (!lp.contains(key)) return true;
            if (!lp[key].is_number_integer()) {
                err = std::string("midi.json: looper.") + key + " deve essere un intero";
                return false;
            }
            out = lp[key].get<int>();
            if (out != -1 && !in_cc_range(out)) {
                err = std::string("midi.json: looper.") + key +
                      " fuori range (0-127, oppure -1)";
                return false;
            }
            return true;
        };
        if (!read_looper("toggle", cfg.looper_toggle_cc)) return false;
        if (!read_looper("stop", cfg.looper_stop_cc)) return false;
        if (!read_looper("clear", cfg.looper_clear_cc)) return false;
    }

    if (j.contains("boost")) {
        const auto& b = j["boost"];
        if (!b.is_object()) {
            err = "midi.json: \"boost\" deve essere un oggetto {cc, db}";
            return false;
        }
        if (!get_int(b, "cc", cfg.boost_cc, err)) return false;
        if (!get_num(b, "db", cfg.boost_db, err)) return false;
    }

    if (j.contains("delay_select")) {
        const auto& ls = j["delay_select"];
        if (!ls.is_object()) {
            err = "midi.json: \"delay_select\" deve essere un oggetto {modo: CC}";
            return false;
        }
        for (auto it = ls.begin(); it != ls.end(); ++it) {
            const int idx = delay_mode_index(it.key());
            if (idx < 0) {
                err = "midi.json: modo delay sconosciuto \"" + it.key() +
                      "\" (attesi: stereo, ping_pong, ping_pong_stereo)";
                return false;
            }
            if (!it.value().is_number_integer()) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve essere un intero";
                return false;
            }
            const int value = it.value().get<int>();
            if (!in_cc_range(value)) {
                err = "midi.json: il CC di \"" + it.key() + "\" deve stare fra 0 e 127";
                return false;
            }
            cfg.delay_select_cc[static_cast<std::size_t>(idx)] = value;
        }
    }

    if (j.contains("devices")) {
        if (!j["devices"].is_array()) {
            err = "midi.json: \"devices\" deve essere un array di nomi ALSA";
            return false;
        }
        cfg.devices.clear();
        for (const auto& d : j["devices"]) {
            if (!d.is_string() || d.get<std::string>().empty()) {
                err = "midi.json: ogni ingresso deve essere un nome non vuoto";
                return false;
            }
            cfg.devices.push_back(d.get<std::string>());
        }
        if (cfg.devices.size() > 4) {
            err = "midi.json: troppi ingressi (massimo 4)";
            return false;
        }
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
    if (cfg.expression_ccs.empty()) cfg.expression_ccs.push_back(cfg.expression_cc);
    for (int c : cfg.expression_ccs) {
        if (!in_cc_range(c)) {
            err = "midi.json: expression.cc deve stare fra 0 e 127";
            return false;
        }
    }
    auto is_expr_cc = [&](int cc) {
        for (int c : cfg.expression_ccs) {
            if (c == cc) return true;
        }
        return false;
    };
    for (std::size_t i = 0; i < kMidiActionCount; ++i) {
        const int cc = cfg.slot_cc[i];
        if (cc < 0) continue;
        if (!in_cc_range(cc)) {
            err = std::string("midi.json: CC fuori range per ") +
                  midi_action_name(static_cast<MidiAction>(i));
            return false;
        }
        if (is_expr_cc(cc)) {
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

    for (std::size_t i = 0; i < kModTypeCount; ++i) {
        const int cc = cfg.mod_select_cc[i];
        if (cc < 0) continue;
        if (is_expr_cc(cc)) {
            err = std::string("midi.json: mod_select \"") + kModTypeNames[i] +
                  "\" usa lo stesso CC dell'espressione";
            return false;
        }
        for (std::size_t k = 0; k < kMidiActionCount; ++k) {
            if (cfg.slot_cc[k] == cc) {
                err = std::string("midi.json: mod_select \"") + kModTypeNames[i] +
                      "\" usa lo stesso CC di " + midi_action_name(static_cast<MidiAction>(k));
                return false;
            }
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (cfg.mod_select_cc[k] == cc) {
                err = std::string("midi.json: mod_select \"") + kModTypeNames[k] +
                      "\" e \"" + kModTypeNames[i] + "\" hanno lo stesso CC";
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < kDriveTypeCount; ++i) {
        const int cc = cfg.drive_select_cc[i];
        if (cc < 0) continue;
        if (is_expr_cc(cc)) {
            err = std::string("midi.json: drive_select \"") + kDriveTypeNames[i] +
                  "\" usa lo stesso CC dell'espressione";
            return false;
        }
        for (std::size_t k = 0; k < kMidiActionCount; ++k) {
            if (cfg.slot_cc[k] == cc) {
                err = std::string("midi.json: drive_select \"") + kDriveTypeNames[i] +
                      "\" usa lo stesso CC di " + midi_action_name(static_cast<MidiAction>(k));
                return false;
            }
        }
        for (std::size_t k = 0; k < kModTypeCount; ++k) {
            if (cfg.mod_select_cc[k] == cc) {
                err = std::string("midi.json: drive_select \"") + kDriveTypeNames[i] +
                      "\" usa lo stesso CC di mod_select \"" + kModTypeNames[k] + "\"";
                return false;
            }
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (cfg.drive_select_cc[k] == cc) {
                err = std::string("midi.json: drive_select \"") + kDriveTypeNames[k] +
                      "\" e \"" + kDriveTypeNames[i] + "\" hanno lo stesso CC";
                return false;
            }
        }
    }

    for (std::size_t i = 0; i < kDelayModeCount; ++i) {
        const int cc = cfg.delay_select_cc[i];
        if (cc < 0) continue;
        if (is_expr_cc(cc)) {
            err = std::string("midi.json: delay_select \"") + kDelayModeNames[i] +
                  "\" usa lo stesso CC dell'espressione";
            return false;
        }
        for (std::size_t k = 0; k < kMidiActionCount; ++k) {
            if (cfg.slot_cc[k] == cc) {
                err = std::string("midi.json: delay_select \"") + kDelayModeNames[i] +
                      "\" usa lo stesso CC di " + midi_action_name(static_cast<MidiAction>(k));
                return false;
            }
        }
        for (std::size_t k = 0; k < kModTypeCount; ++k) {
            if (cfg.mod_select_cc[k] == cc) {
                err = std::string("midi.json: delay_select \"") + kDelayModeNames[i] +
                      "\" usa lo stesso CC di mod_select \"" + kModTypeNames[k] + "\"";
                return false;
            }
        }
        for (std::size_t k = 0; k < kDriveTypeCount; ++k) {
            if (cfg.drive_select_cc[k] == cc) {
                err = std::string("midi.json: delay_select \"") + kDelayModeNames[i] +
                      "\" usa lo stesso CC di drive_select \"" + kDriveTypeNames[k] + "\"";
                return false;
            }
        }
        for (std::size_t k = 0; k < i; ++k) {
            if (cfg.delay_select_cc[k] == cc) {
                err = std::string("midi.json: delay_select \"") + kDelayModeNames[k] +
                      "\" e \"" + kDelayModeNames[i] + "\" hanno lo stesso CC";
                return false;
            }
        }
    }

    if (cfg.boost_cc >= 0) {
        if (!in_cc_range(cfg.boost_cc)) {
            err = "midi.json: boost.cc deve stare fra 0 e 127";
            return false;
        }
        if (cfg.boost_db < 0.0 || cfg.boost_db > 24.0) {
            err = "midi.json: boost.db deve stare fra 0 e 24 dB";
            return false;
        }
        if (is_expr_cc(cfg.boost_cc)) {
            err = "midi.json: boost usa lo stesso CC dell'espressione";
            return false;
        }
        for (std::size_t k = 0; k < kMidiActionCount; ++k) {
            if (cfg.slot_cc[k] == cfg.boost_cc) {
                err = std::string("midi.json: boost usa lo stesso CC di ") +
                      midi_action_name(static_cast<MidiAction>(k));
                return false;
            }
        }
        for (std::size_t k = 0; k < kModTypeCount; ++k) {
            if (cfg.mod_select_cc[k] == cfg.boost_cc) {
                err = std::string("midi.json: boost usa lo stesso CC di mod_select \"") +
                      kModTypeNames[k] + "\"";
                return false;
            }
        }
        for (std::size_t k = 0; k < kDriveTypeCount; ++k) {
            if (cfg.drive_select_cc[k] == cfg.boost_cc) {
                err = std::string("midi.json: boost usa lo stesso CC di drive_select \"") +
                      kDriveTypeNames[k] + "\"";
                return false;
            }
        }
    }

    // G-Board: gli ingressi CC non possono coincidere con un altro binding,
    // altrimenti una pressione farebbe partire due azioni. I vecchi PC restano
    // accettati in parallelo: sono un altro tipo di messaggio, non collidono.
    {
        std::vector<std::pair<int, std::string>> gboard_ccs;
        for (std::size_t i = 0; i < cfg.gboard_mod_cc.size(); ++i) {
            if (cfg.gboard_mod_cc[i] >= 0) {
                gboard_ccs.emplace_back(cfg.gboard_mod_cc[i],
                                        "mod_cc[" + std::to_string(i) + "]");
            }
        }
        for (std::size_t i = 0; i < cfg.gboard_drive_cc.size(); ++i) {
            if (cfg.gboard_drive_cc[i] >= 0) {
                gboard_ccs.emplace_back(cfg.gboard_drive_cc[i],
                                        "drive_cc[" + std::to_string(i) + "]");
            }
        }
        if (cfg.gboard_boost_cc >= 0) {
            gboard_ccs.emplace_back(cfg.gboard_boost_cc, "boost_cc");
        }
        for (std::size_t i = 0; i < gboard_ccs.size(); ++i) {
            for (std::size_t k = 0; k < i; ++k) {
                if (gboard_ccs[i].first == gboard_ccs[k].first) {
                    err = "midi.json: gboard." + gboard_ccs[k].second + " e gboard." +
                          gboard_ccs[i].second + " usano lo stesso CC " +
                          std::to_string(gboard_ccs[i].first);
                    return false;
                }
            }
        }

        std::vector<std::pair<int, std::string>> gboard_leds;
        for (std::size_t i = 0; i < cfg.gboard_mod_led.size(); ++i) {
            if (cfg.gboard_mod_led[i] >= 0) {
                gboard_leds.emplace_back(cfg.gboard_mod_led[i],
                                         "mod_led[" + std::to_string(i) + "]");
            }
        }
        for (std::size_t i = 0; i < cfg.gboard_drive_led.size(); ++i) {
            if (cfg.gboard_drive_led[i] >= 0) {
                gboard_leds.emplace_back(cfg.gboard_drive_led[i],
                                         "drive_led[" + std::to_string(i) + "]");
            }
        }
        if (cfg.gboard_boost_led >= 0) {
            gboard_leds.emplace_back(cfg.gboard_boost_led, "boost_led");
        }
        for (std::size_t i = 0; i < gboard_leds.size(); ++i) {
            for (std::size_t k = 0; k < i; ++k) {
                if (gboard_leds[i].first == gboard_leds[k].first) {
                    err = "midi.json: gboard." + gboard_leds[k].second + " e gboard." +
                          gboard_leds[i].second + " usano la stessa nota LED " +
                          std::to_string(gboard_leds[i].first);
                    return false;
                }
            }
        }

        if (!cfg.gboard_allow_cc_conflicts) {
            std::vector<std::pair<int, std::string>> occupied;
            for (int c : cfg.expression_ccs) {
                occupied.emplace_back(c, "expression.cc");
            }
            for (std::size_t i = 0; i < kMidiActionCount; ++i) {
                if (cfg.slot_cc[i] >= 0) {
                    occupied.emplace_back(cfg.slot_cc[i],
                                          std::string("cc.") +
                                              midi_action_name(static_cast<MidiAction>(i)));
                }
            }
            for (std::size_t i = 0; i < kModTypeCount; ++i) {
                if (cfg.mod_select_cc[i] >= 0) {
                    occupied.emplace_back(cfg.mod_select_cc[i],
                                          std::string("mod_select.") + kModTypeNames[i]);
                }
            }
            for (std::size_t i = 0; i < kDriveTypeCount; ++i) {
                if (cfg.drive_select_cc[i] >= 0) {
                    occupied.emplace_back(cfg.drive_select_cc[i],
                                          std::string("drive_select.") + kDriveTypeNames[i]);
                }
            }
            for (std::size_t i = 0; i < kDelayModeCount; ++i) {
                if (cfg.delay_select_cc[i] >= 0) {
                    occupied.emplace_back(cfg.delay_select_cc[i],
                                          std::string("delay_select.") + kDelayModeNames[i]);
                }
            }
            if (cfg.boost_cc >= 0) occupied.emplace_back(cfg.boost_cc, "boost.cc");
            if (cfg.looper_toggle_cc >= 0) {
                occupied.emplace_back(cfg.looper_toggle_cc, "looper.toggle");
            }
            if (cfg.looper_stop_cc >= 0) {
                occupied.emplace_back(cfg.looper_stop_cc, "looper.stop");
            }
            if (cfg.looper_clear_cc >= 0) {
                occupied.emplace_back(cfg.looper_clear_cc, "looper.clear");
            }
            // La navigazione preset (22/24/25) e' cablata in main.cpp e non
            // configurabile: va trattata come occupata.
            static const int kNavCcs[] = {22, 24, 25};
            for (int c : kNavCcs) occupied.emplace_back(c, "navigazione preset");

            for (const auto& g : gboard_ccs) {
                for (const auto& o : occupied) {
                    if (g.first == o.first) {
                        err = "midi.json: gboard." + g.second + "=" +
                              std::to_string(g.first) + " usa lo stesso CC di " + o.second +
                              " (se e' voluto: gboard.allow_cc_conflicts = true)";
                        return false;
                    }
                }
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
