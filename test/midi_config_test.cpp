#include <cstdio>
#include <fstream>

#include "control/midi_config.h"

static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_fail;                                                          \
        }                                                                      \
    } while (0)

namespace {

bool rejects(const char* text) {
    namzero::MidiConfig cfg;
    std::string err;
    const auto j = nlohmann::json::parse(text);
    return !namzero::parse_midi_config(j, cfg, err) && !err.empty();
}

bool rejects_with(const char* text, const char* needle) {
    namzero::MidiConfig cfg;
    std::string err;
    const auto j = nlohmann::json::parse(text);
    if (namzero::parse_midi_config(j, cfg, err)) return false;
    return err.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    // Senza file restano legati solo i tre storici: il comportamento non cambia.
    {
        namzero::MidiConfig c;
        CHECK(c.expression_cc == 7);
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kMod)] == 10);
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kDrive)] == 20);
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kDelay)] == 11);
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kGate)] == -1);
        // G-Board: di default nessun ingresso CC (si configura in midi.json),
        // ma le note LED restano quelle storiche 4-7 / 0-2 / 3.
        CHECK(c.gboard_mod_cc[0] == -1);
        CHECK(c.gboard_drive_cc[0] == -1);
        CHECK(c.gboard_boost_cc == -1);
        CHECK(c.gboard_mod_led[0] == 0);
        CHECK(c.gboard_mod_led[3] == 3);
        CHECK(c.gboard_drive_led[0] == 4);
        CHECK(c.gboard_drive_led[2] == 6);
        CHECK(c.gboard_boost_led == 7);
        // Default: PC-effetto spenti, preset spenti (canale 1 libero).
        CHECK(!c.gboard_preset_pc);
        CHECK(!c.gboard_pc_effects);
        CHECK(c.gboard_pc_effects_channel == 9);
        CHECK(c.gboard_pc_preset_channel == 1);
    }

    // File completo: tutti gli slot mappati, parametri del pedale compresi.
    {
        const auto j = nlohmann::json::parse(R"({
            "enabled": true,
            "device": "hw:2,0,0",
            "switches": "edge",
            "expression": { "cc": 4, "min_raw": 10, "max_raw": 120, "invert": true },
            "wah_autoengage": { "threshold": 0.2, "release_threshold": 0.1,
                                "engage_ms": 50, "release_ms": 400, "invert": true },
            "cc": { "gate": 25, "comp": 22, "drive": 20, "mod": 10,
                    "amp": 23, "ir": 30, "delay": 11, "reverb": 24 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.device == "hw:2,0,0");
        CHECK(c.switches == "edge");
        CHECK(c.expression_cc == 4);
        CHECK(c.expression_min_raw == 10);
        CHECK(c.expression_max_raw == 120);
        CHECK(c.expression_invert);
        CHECK(c.wah_threshold == 0.2);
        CHECK(c.wah_release_threshold == 0.1);
        CHECK(c.wah_engage_ms == 50);
        CHECK(c.wah_release_ms == 400);
        CHECK(c.wah_invert);
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kGate)] == 25);
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kReverb)] == 24);
    }

    // drive_select: un CC per modello.
    {
        const auto j = nlohmann::json::parse(R"({
            "drive_select": { "fuzz": 36, "klon": 37, "tubescreamer": 38 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.drive_select_cc[0] == 36);
        CHECK(c.drive_select_cc[1] == 37);
        CHECK(c.drive_select_cc[2] == 38);
    }

    // mod_select: quattro tipi, indici {tremolo, univibe, flanger, phaser}.
    // Il wah non e' piu' selezionabile: vive nel blocco filter.
    {
        const auto j = nlohmann::json::parse(R"({
            "mod_select": { "tremolo": 32, "univibe": 33, "flanger": 34, "phaser": 35 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.mod_select_cc[0] == 32);
        CHECK(c.mod_select_cc[1] == 33);
        CHECK(c.mod_select_cc[2] == 34);
        CHECK(c.mod_select_cc[3] == 35);
        CHECK(namzero::kModTypeCount == 4);
    }

    // delay_select: un CC per modo.
    {
        const auto j = nlohmann::json::parse(R"({
            "delay_select": { "stereo": 45, "ping_pong": 46, "ping_pong_stereo": 47 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.delay_select_cc[0] == 45);
        CHECK(c.delay_select_cc[1] == 46);
        CHECK(c.delay_select_cc[2] == 47);
    }

    // Boost di fine catena.
    {
        const auto j = nlohmann::json::parse(R"({ "boost": { "cc": 44, "db": 20 } })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.boost_cc == 44);
        CHECK(c.boost_db == 20.0);
    }

    // -1 toglie un binding.
    {
        const auto j = nlohmann::json::parse(R"({ "cc": { "mod": -1, "drive": -1, "delay": -1 } })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kMod)] == -1);
    }

    // G-Board riconfigurata: CC in ingresso e note LED separate.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": {
                "device": "hw:V103",
                "mod_cc": [54, 55, 56, 57],
                "drive_cc": [50, 51, 52],
                "boost_cc": 53,
                "mod_led": [4, 5, 6, 7],
                "drive_led": [0, 1, 2],
                "boost_led": 3,
                "mod_pc": [4, 5, 6, 7],
                "drive_pc": [0, 1, 2],
                "boost_pc": 3,
                "flash_ms": 150
            }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_mod_cc[0] == 54);
        CHECK(c.gboard_mod_cc[3] == 57);
        CHECK(c.gboard_drive_cc[0] == 50);
        CHECK(c.gboard_drive_cc[2] == 52);
        CHECK(c.gboard_boost_cc == 53);
        CHECK(c.gboard_mod_led[0] == 4);
        CHECK(c.gboard_mod_led[3] == 7);
        CHECK(c.gboard_drive_led[0] == 0);
        CHECK(c.gboard_boost_led == 3);
        // I PC storici restano accettati in parallelo.
        CHECK(c.gboard_mod_pc[0] == 4);
        CHECK(c.gboard_boost_pc == 3);
        CHECK(!c.gboard_allow_cc_conflicts);
    }

    // Nuovi flag G-Board: default e parse esplicito.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "preset_pc": false, "pc_effects": true }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(!c.gboard_preset_pc);
        CHECK(c.gboard_pc_effects);
    }
    {
        // Entrambi true e' ammesso: a runtime vince il preset (con avviso).
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "preset_pc": true, "pc_effects": true }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_preset_pc);
        CHECK(c.gboard_pc_effects);
    }

    // Routing dei PC: preset_pc=true -> slot 0-7.
    {
        namzero::MidiConfig c;
        c.gboard_preset_pc = true;
        for (int pc = 0; pc <= 7; ++pc) {
            const auto r = namzero::route_gboard_pc(c, pc);
            CHECK(r.action == namzero::GboardPcAction::kPreset);
            CHECK(r.index == pc);
        }
        const auto r8 = namzero::route_gboard_pc(c, 8);
        CHECK(r8.action == namzero::GboardPcAction::kNone);
        const auto rneg = namzero::route_gboard_pc(c, -1);
        CHECK(rneg.action == namzero::GboardPcAction::kNone);
        const auto rbig = namzero::route_gboard_pc(c, 128);
        CHECK(rbig.action == namzero::GboardPcAction::kNone);
    }

    // Routing dei PC: preset_pc=false e pc_effects=false -> tutto ignorato.
    {
        namzero::MidiConfig c;
        c.gboard_preset_pc = false;
        c.gboard_pc_effects = false;
        c.gboard_mod_pc = {4, 5, 6, 7};
        c.gboard_drive_pc = {0, 1, 2};
        c.gboard_boost_pc = 3;
        for (int pc = 0; pc <= 7; ++pc) {
            CHECK(namzero::route_gboard_pc(c, pc).action ==
                  namzero::GboardPcAction::kNone);
        }
    }

    // Routing dei PC: pc_effects=true -> azioni legacy one-shot.
    {
        namzero::MidiConfig c;
        c.gboard_preset_pc = false;
        c.gboard_pc_effects = true;
        c.gboard_mod_pc = {4, 5, 6, 7};
        c.gboard_drive_pc = {0, 1, 2};
        c.gboard_boost_pc = 3;
        auto r = namzero::route_gboard_pc(c, 5);
        CHECK(r.action == namzero::GboardPcAction::kMod);
        CHECK(r.index == 1);
        r = namzero::route_gboard_pc(c, 2);
        CHECK(r.action == namzero::GboardPcAction::kDrive);
        CHECK(r.index == 2);
        r = namzero::route_gboard_pc(c, 3);
        CHECK(r.action == namzero::GboardPcAction::kBoost);
        CHECK(namzero::route_gboard_pc(c, 8).action == namzero::GboardPcAction::kNone);
    }

    // Routing dei PC: con entrambi true i PC 0-7 hanno la priorita' preset;
    // i PC fuori 0-7 possono ancora usare le azioni legacy configurate.
    {
        namzero::MidiConfig c;
        c.gboard_preset_pc = true;
        c.gboard_pc_effects = true;
        c.gboard_mod_pc = {9, 5, 6, 7};
        c.gboard_drive_pc = {0, 1, 2};
        c.gboard_boost_pc = 3;
        auto r = namzero::route_gboard_pc(c, 3);
        CHECK(r.action == namzero::GboardPcAction::kPreset);
        CHECK(r.index == 3);
        r = namzero::route_gboard_pc(c, 9);
        CHECK(r.action == namzero::GboardPcAction::kMod);
        CHECK(r.index == 0);
    }

    // Blocco gboard minimale: LED di default invariati, nessun ingresso CC.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "device": "hw:V103", "flash_ms": 100 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_mod_led[0] == 4);
        CHECK(c.gboard_boost_led == 3);
        CHECK(c.gboard_mod_cc[0] == -1);
        CHECK(c.gboard_boost_cc == -1);
    }

    // File vecchio senza *_led: i PC valgono anche come note LED, come prima.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "mod_pc": [10, 11, 12, 13],
                        "drive_pc": [14, 15, 16], "boost_pc": 17 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_mod_led[0] == 10);
        CHECK(c.gboard_mod_led[3] == 13);
        CHECK(c.gboard_drive_led[2] == 16);
        CHECK(c.gboard_boost_led == 17);
        CHECK(c.gboard_mod_cc[0] == -1);
    }

    // Se *_led c'e', vince su *_pc: PC e note LED sono indipendenti.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "mod_pc": [10, 11, 12, 13], "mod_led": [4, 5, 6, 7] }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_mod_led[0] == 4);
        CHECK(c.gboard_mod_pc[0] == 10);
    }

    // Il looper si legge anche senza gboard.flash_ms (prima era annidato).
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "device": "hw:V103" },
            "looper": { "toggle": 30, "stop": 43, "clear": 42 }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.looper_toggle_cc == 30);
        CHECK(c.looper_stop_cc == 43);
        CHECK(c.looper_clear_cc == 42);
    }

    // -1 disattiva una singola voce di un array.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "mod_cc": [54, 55, 56, -1], "mod_led": [-1, 5, 6, 7] }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_mod_cc[3] == -1);
        CHECK(c.gboard_mod_led[0] == -1);
    }

    // Conflitto voluto: si puo' autorizzare esplicitamente.
    {
        const auto j = nlohmann::json::parse(R"({
            "gboard": { "mod_cc": [10, 55, 56, 57], "allow_cc_conflicts": true }
        })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.gboard_mod_cc[0] == 10);
        CHECK(c.gboard_allow_cc_conflicts);
    }

    // Errori: un file sbagliato deve dirlo, non essere ignorato.
    CHECK(rejects(R"({ "cc": { "drieve": 12 } })"));                       // azione ignota
    CHECK(rejects(R"({ "cc": { "drive": 200 } })"));                       // fuori range
    CHECK(rejects(R"({ "cc": { "drive": 10 } })"));                        // collide col mod di default
    CHECK(rejects(R"({ "cc": { "gate": 20, "comp": 20 } })"));             // doppio
    CHECK(rejects(R"({ "expression": { "cc": 10 } })"));                   // collide col mod
    CHECK(rejects(R"({ "switches": "boh" })"));                            // modo sconosciuto
    CHECK(rejects(R"({ "drive_select": { "drieve": 36 } })"));             // tipo drive ignoto
    CHECK(rejects(R"({ "drive_select": { "fuzz": 10 } })"));               // collide col mod di default
    CHECK(rejects(R"({ "drive_select": { "fuzz": 36, "klon": 36 } })"));   // doppio
    CHECK(rejects(R"({ "drive_select": { "fuzz": 32 }, "mod_select": { "tremolo": 32 } })"));
    CHECK(rejects(R"({ "boost": { "cc": 200 } })"));                        // fuori range
    CHECK(rejects(R"({ "boost": { "cc": 44, "db": 500 } })"));              // boost assurdo
    CHECK(rejects(R"({ "boost": { "cc": 10 } })"));                         // collide col mod
    CHECK(rejects(R"({ "delay_select": { "boh": 45 } })"));                 // modo sconosciuto
    CHECK(rejects(R"({ "mod_select": { "wah": 36 } })"));                   // il wah non e' un tipo di mod
    CHECK(rejects(R"({ "delay_select": { "stereo": 200 } })"));             // fuori range
    CHECK(rejects(R"({ "delay_select": { "stereo": 32 }, "mod_select": { "tremolo": 32 } })"));
    CHECK(rejects(R"({ "expression": { "cc": -1 } })"));                   // espressione fuori range
    CHECK(rejects(R"([1, 2, 3])"));                                        // radice non oggetto

    // G-Board: collisioni con gli altri CC, doppioni interni, LED doppi.
    CHECK(rejects_with(R"({ "gboard": { "mod_cc": [10, 55, 56, 57] } })",
                       "stesso CC"));
    CHECK(rejects_with(R"({ "gboard": { "drive_cc": [50, 51, 7] } })",
                       "expression"));
    CHECK(rejects_with(R"({ "gboard": { "boost_cc": 44 },
                            "boost": { "cc": 44 } })",
                       "boost.cc"));
    CHECK(rejects_with(R"({ "gboard": { "mod_cc": [54, 55, 56, 22] } })",
                       "navigazione"));
    CHECK(rejects_with(R"({ "gboard": { "mod_cc": [54, 54, 56, 57] } })",
                       "stesso CC"));
    CHECK(rejects_with(R"({ "gboard": { "drive_cc": [50, 51, 54],
                                        "mod_cc": [54, 55, 56, 57] } })",
                       "stesso CC"));
    CHECK(rejects_with(R"({ "gboard": { "mod_led": [4, 5, 6, 7],
                                        "drive_led": [4, 1, 2] } })",
                       "nota LED"));
    CHECK(rejects_with(R"({ "gboard": { "mod_cc": [54, 55, 56, 200] } })",
                       "fuori range"));
    CHECK(rejects_with(R"({ "gboard": { "mod_cc": [54] } })", "4 elementi"));
    CHECK(rejects_with(R"({ "gboard": { "mod_pc": [4, 5, 6] } })", "4 elementi"));
    CHECK(rejects_with(R"({ "gboard": { "boost_led": 200 } })", "fuori range"));
    CHECK(rejects_with(R"({ "gboard": { "flash_ms": 5000 } })", "flash_ms"));
    CHECK(rejects_with(R"({ "gboard": { "preset_pc": "true" } })", "preset_pc"));
    CHECK(rejects_with(R"({ "gboard": { "pc_effects": 1 } })", "pc_effects"));

    // Lettura da file: il wrapper deve riportare l'errore di parsing.
    {
        const char* path = "/tmp/namzero_midi_config_test.json";
        {
            std::ofstream out(path);
            out << R"({ "cc": { "drive": 20, "mod": 10, "delay": 11 } })";
        }
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::load_midi_config(path, c, err));
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kMod)] == 10);

        {
            std::ofstream out(path);
            out << "{ non json";
        }
        CHECK(!namzero::load_midi_config(path, c, err));
        CHECK(!err.empty());
        std::remove(path);
    }

    if (g_fail != 0) {
        std::fprintf(stderr, "midi_config: %d controlli falliti\n", g_fail);
        return 1;
    }
    std::printf("midi_config: default, file completo, disattivazione ed errori ok\n");
    return 0;
}
