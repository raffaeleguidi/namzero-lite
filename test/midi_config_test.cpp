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

    // -1 toglie un binding.
    {
        const auto j = nlohmann::json::parse(R"({ "cc": { "mod": -1, "drive": -1, "delay": -1 } })");
        namzero::MidiConfig c;
        std::string err;
        CHECK(namzero::parse_midi_config(j, c, err));
        CHECK(c.slot_cc[static_cast<std::size_t>(namzero::MidiAction::kMod)] == -1);
    }

    // Errori: un file sbagliato deve dirlo, non essere ignorato.
    CHECK(rejects(R"({ "cc": { "drieve": 12 } })"));                       // azione ignota
    CHECK(rejects(R"({ "cc": { "drive": 200 } })"));                       // fuori range
    CHECK(rejects(R"({ "cc": { "drive": 10 } })"));                        // collide col mod di default
    CHECK(rejects(R"({ "cc": { "gate": 20, "comp": 20 } })"));             // doppio
    CHECK(rejects(R"({ "expression": { "cc": 10 } })"));                   // collide col mod
    CHECK(rejects(R"({ "switches": "boh" })"));                            // modo sconosciuto
    CHECK(rejects(R"({ "expression": { "cc": -1 } })"));                   // espressione fuori range
    CHECK(rejects(R"([1, 2, 3])"));                                        // radice non oggetto

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
