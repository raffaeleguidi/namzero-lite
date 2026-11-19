#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <array>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <termios.h>
#include <unistd.h>

#include "audio/alsa_io.h"
#include "audio/rt.h"
#include "dsp/chain.h"
#include "dsp/comp_slot.h"
#include "dsp/drive_slot.h"
#include "dsp/gain_slot.h"
#include "dsp/gate_slot.h"
#include "dsp/ir_slot.h"
#include "dsp/nam_slot.h"
#include "dsp/delay_slot.h"
#include "dsp/reverb_slot.h"
#include "control/midi.h"
#include "control/midi_config.h"
#include "control/mod_types.h"
#include "control/midi_socket.h"
#include "control/switch_debounce.h"
#include "control/wah_autoengage.h"
#include "dsp/mod_slot.h"
#include "json.hpp"

namespace {

std::atomic<bool> g_running{true};

// Indice che significa "slot assente": 0 non va bene, perche' il gate e' l'indice 0.
constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

void on_signal(int /*sig*/) { g_running.store(false); }

// Tutta la configurazione del motore in un posto solo: i flag da riga di
// comando e il file preset sono due modi di riempire questa struttura.
struct Config {
    // audio
    std::string device = "hw:XTONE";
    unsigned rate = 48000;
    unsigned period = 64;
    unsigned periods = 3;
    double gain_db = 0.0;

    // slot
    bool gate_on = false;
    double gate_threshold_db = -55.0;
    double gate_attack_ms = 2.0;
    double gate_release_ms = 120.0;

    bool comp_on = false;
    double comp_threshold_db = -18.0;
    double comp_ratio = 3.0;
    double comp_attack_ms = 10.0;
    double comp_release_ms = 150.0;
    double comp_makeup_db = 3.0;

    bool drive_on = false;
    std::string drive_type = "fuzz";  // fuzz | klon
    double drive_amount = 0.15;
    double drive_tone = 0.5;
    double drive_level_db = 0.0;
    double drive_blend = 0.40;  // solo klon: quota di segnale saturato
    double drive_mid_db = 4.0;  // solo klon: gobba sulle medie
    double drive_low_db = 5.0;  // solo fuzz: peso delle basse

    bool mod_on = false;
    std::string mod_type = "tremolo";  // tremolo | univibe | flanger | phaser | wah
    double mod_rate_hz = 4.5;
    double mod_depth = 0.6;
    double mod_fx_mix = 0.5;   // univibe/flanger/phaser: quota di segnale trattato
    double mod_feedback = 0.35;  // flanger/phaser
    double mod_position = 0.5;
    double mod_q = 3.0;
    double mod_mix = 1.0;
    double mod_low_hz = 350.0;
    double mod_high_hz = 2200.0;

    bool amp_on = false;
    std::string amp_file;
    double amp_slim = 0.49;
    double amp_trim_db = 0.0;

    bool ir_on = false;
    std::string ir_file;
    unsigned ir_taps = 2048;

    bool delay_on = false;
    double delay_time_ms = 350.0;
    double delay_feedback = 0.35;
    double delay_mix = 0.25;
    double delay_tone = 0.5;
    std::string delay_mode = "stereo";  // stereo | ping_pong | ping_pong_stereo

    bool reverb_on = false;
    double reverb_mix = 0.25;
    double reverb_size = 0.55;
    double reverb_damp = 0.45;

    // comportamento
    // La mappa MIDI non sta qui ne' nei preset: vive in midi.json (vedi
    // MidiConfig), cosi' si cambia senza ricompilare e vale per tutti i preset.
    bool midi_log = false;  // stampa ogni messaggio: serve a misurare il pedale
    bool print_config = false;  // stampa i valori effettivi letti dagli slot ed esce

    bool stats = false;
    bool help = false;
};

// Tipi di mod e drive in ordine di ciclo: la stessa tabella serve a validare
// il preset, al tasto che li scorre e al ripristino dello stato del mod.

// Rate e feedback suggeriti per tipo: quando si scorre con 'w' i parametri si
// adeguano, altrimenti un flanger a 4.5 Hz suonerebbe come un vibrato nervoso.
constexpr float kModDefaultRate[] = {4.5f, 0.0f, 1.0f, 0.5f, 0.8f};
constexpr float kModDefaultFeedback[] = {0.0f, 0.0f, 0.0f, 0.35f, 0.20f};

// Il wah non e' nella turnazione: si usa col pedale (o da preset / --wah).
// Restare in elenco significava solo poterlo selezionare spento.
constexpr int kModCycle[] = {0, 2, 3, 4};  // tremolo, univibe, flanger, phaser
constexpr int kModCycleCount = static_cast<int>(sizeof(kModCycle) / sizeof(kModCycle[0]));

constexpr const char* kDriveTypeNames[] = {"fuzz", "klon", "tubescreamer"};
constexpr std::size_t kDriveTypeCount = sizeof(kDriveTypeNames) / sizeof(kDriveTypeNames[0]);


namzero::ModSlot::Type mod_type_enum(int idx) {
    switch (idx) {
        case 1: return namzero::ModSlot::Type::kWah;
        case 2: return namzero::ModSlot::Type::kUnivibe;
        case 3: return namzero::ModSlot::Type::kFlanger;
        case 4: return namzero::ModSlot::Type::kPhaser;
        default: return namzero::ModSlot::Type::kTremolo;
    }
}

int drive_type_index(const std::string& name) {
    for (std::size_t i = 0; i < kDriveTypeCount; ++i) {
        if (name == kDriveTypeNames[i]) return static_cast<int>(i);
    }
    return -1;
}

namzero::DriveSlot::Type drive_type_enum(int idx) {
    switch (idx) {
        case 1: return namzero::DriveSlot::Type::kKlon;
        case 2: return namzero::DriveSlot::Type::kTubescreamer;
        default: return namzero::DriveSlot::Type::kFuzz;
    }
}

struct Stats {
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> xruns{0};
    std::atomic<uint64_t> max_us{0};
};

void usage() {
    std::puts(
        "namzero-lite - motore audio realtime per chitarra\n"
        "\n"
        "Uso: namzero-lite [--preset file.json] | [opzioni]\n"
        "\n"
        "  -p, --preset <file>  carica un preset JSON (alternativo agli altri flag)\n"
        "  --device <hw:ID>     PCM ALSA (default hw:XTONE)\n"
        "  --rate <hz>          sample rate (default 48000)\n"
        "  --buffer <frame>     frame per periodo (default 64)\n"
        "  --periods <n>        periodi nel buffer (default 3)\n"
        "  --gain <db>          gain master (default 0)\n"
        "  --nam <file.nam>     modello NAM nello slot amp\n"
        "  --nam-slim <0..1>    sottomodello (default 0.49 = lite)\n"
        "  --amp-gain <db>      trim sullo slot amp (default 0)\n"
        "  --ir <file.wav>      cabinet IR\n"
        "  --taps <n>           limite di tap della IR (default 2048)\n"
        "  --gate               noise gate\n"
        "  --gate-threshold <db> soglia (default -55)\n"
        "  --gate-attack <ms>   attacco (default 2)\n"
        "  --gate-release <ms>  rilascio: lungo = tiene le code (default 120)\n"
        "  --comp               compressore\n"
        "  --drive              boost/overdrive\n"
        "  --drive-type <t>     fuzz (default) | klon | tubescreamer\n"
        "  --drive-amount <0..1> quantita' di drive (default 0.15)\n"
        "  --drive-tone <0..1>  tono del drive (default 0.5)\n"
        "  --drive-blend <0..1> solo klon: quota di segnale saturato (default 0.40)\n"
        "  --drive-mid <db>     solo klon: gobba sulle medie (default +4)\n"
        "  --drive-low <db>     solo fuzz: peso delle basse (default +5)\n"
        "  --tremolo            tremolo nello slot mod\n"
        "  --wah                wah nello slot mod\n"
        "  --wah-position <0..1> posizione del pedale (default 0.5)\n"
        "  --wah-q <0.5..12>    risonanza (default 3)\n"
        "  --wah-mix <0..1>     quantita' di wah (default 1)\n"
        "  --tremolo-rate <hz>  frequenza (default 4.5)\n"
        "  --tremolo-depth <0..1> profondita' (default 0.6)\n"
        "  --delay              delay\n"
        "  --delay-time <ms>    tempo (default 350)\n"
        "  --delay-feedback <0..0.95> retroazione (default 0.35)\n"
        "  --delay-mix <0..1>   quantita' (default 0.25)\n"
        "  --delay-ping-pong        eco alternato (ingresso mono)\n"
        "  --delay-ping-pong-stereo eco alternato, ingresso stereo preservato\n"
        "  --reverb             riverbero\n"
        "  --reverb-mix <0..1>  quantita' (default 0.25)\n"
        "  --reverb-size <0..1> stanza (default 0.55)\n"
        "  --midi-config <f>    file della mappa MIDI (default config/midi.json)\n"
        "  --midi-device <dev>  rawmidi (default hw:XTONE)\n"
        "  --no-midi            non aprire l'ingresso MIDI\n"
        "  --expr-invert        pedale di espressione con corsa invertita\n"
        "  --wah-invert         wah acceso verso il tacco: spento con la punta a fondo\n"
        "  --midi-log           stampa ogni messaggio MIDI ricevuto (cc, note, pc)\n"
        "  --print-config       stampa i valori effettivi letti dagli slot ed esce\n"
        "  --stats              statistiche ogni 5 s (default: solo problemi)\n"
        "  -h, --help           questo messaggio\n"
        "\n"
        "Tasti durante l'esecuzione:\n"
        "  g gate | c comp | o overdrive | k modello drive | p modo delay\n"
        "  + - volume ampli\n"
        "  m mod | w tipo mod | , . wah\n"
        "  d delay | t tap tempo | i ir | r reverb\n"
        "  s salva lo stato nel preset | q o Ctrl+C esce\n");
}

bool parse_uint(const char* s, unsigned& out) {
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = static_cast<unsigned>(v);
    return true;
}

bool parse_float(const char* s, double& out) {
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = v;
    return true;
}

// --- preset ---------------------------------------------------------------

double get_num(const nlohmann::json& j, const char* key, double fallback) {
    return (j.contains(key) && j[key].is_number()) ? j[key].get<double>() : fallback;
}

bool get_str(const nlohmann::json& j, const char* key, std::string& out) {
    if (!j.contains(key) || !j[key].is_string()) return false;
    out = j[key].get<std::string>();
    return true;
}

bool get_bool(const nlohmann::json& j, const char* key, bool fallback) {
    return (j.contains(key) && j[key].is_boolean()) ? j[key].get<bool>() : fallback;
}

nlohmann::json slot_or_empty(const nlohmann::json& root, const char* name) {
    if (root.contains("slots") && root["slots"].is_object() &&
        root["slots"].contains(name) && root["slots"][name].is_object()) {
        return root["slots"][name];
    }
    return nlohmann::json::object();
}

bool load_preset(const std::string& path, Config& cfg, std::string& err) {
    nlohmann::json j;
    try {
        std::ifstream in(path);
        if (!in) {
            err = "impossibile aprire " + path;
            return false;
        }
        in >> j;
    } catch (const std::exception& e) {
        err = std::string("JSON non valido: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        err = "il preset deve essere un oggetto JSON";
        return false;
    }

    if (j.contains("audio") && j["audio"].is_object()) {
        const auto& a = j["audio"];
        get_str(a, "device", cfg.device);
        if (a.contains("rate") && a["rate"].is_number_integer()) {
            cfg.rate = a["rate"].get<unsigned>();
        }
        if (a.contains("period_frames") && a["period_frames"].is_number_integer()) {
            cfg.period = a["period_frames"].get<unsigned>();
        }
        if (a.contains("periods") && a["periods"].is_number_integer()) {
            cfg.periods = a["periods"].get<unsigned>();
        }
        cfg.gain_db = get_num(a, "gain_db", cfg.gain_db);
    }

    auto g = slot_or_empty(j, "gate");
    if (!g.empty()) {
        cfg.gate_on = get_bool(g, "on", cfg.gate_on);
        cfg.gate_threshold_db = get_num(g, "threshold_db", cfg.gate_threshold_db);
        cfg.gate_attack_ms = get_num(g, "attack_ms", cfg.gate_attack_ms);
        cfg.gate_release_ms = get_num(g, "release_ms", cfg.gate_release_ms);
    }

    auto c = slot_or_empty(j, "comp");
    if (!c.empty()) {
        cfg.comp_on = get_bool(c, "on", cfg.comp_on);
        cfg.comp_threshold_db = get_num(c, "threshold_db", cfg.comp_threshold_db);
        cfg.comp_ratio = get_num(c, "ratio", cfg.comp_ratio);
        cfg.comp_attack_ms = get_num(c, "attack_ms", cfg.comp_attack_ms);
        cfg.comp_release_ms = get_num(c, "release_ms", cfg.comp_release_ms);
        cfg.comp_makeup_db = get_num(c, "makeup_db", cfg.comp_makeup_db);
    }

    auto d = slot_or_empty(j, "drive");
    if (!d.empty()) {
        cfg.drive_on = get_bool(d, "on", cfg.drive_on);
        get_str(d, "type", cfg.drive_type);
        cfg.drive_amount = get_num(d, "amount", cfg.drive_amount);
        cfg.drive_tone = get_num(d, "tone", cfg.drive_tone);
        cfg.drive_level_db = get_num(d, "level_db", cfg.drive_level_db);
        cfg.drive_blend = get_num(d, "blend", cfg.drive_blend);
        cfg.drive_mid_db = get_num(d, "mid_db", cfg.drive_mid_db);
        cfg.drive_low_db = get_num(d, "low_db", cfg.drive_low_db);
    }

    auto md = slot_or_empty(j, "mod");
    if (!md.empty()) {
        cfg.mod_on = get_bool(md, "on", cfg.mod_on);
        if (md.contains("type") && md["type"].is_string()) {
            cfg.mod_type = md["type"].get<std::string>();
        }
        cfg.mod_rate_hz = get_num(md, "rate_hz", cfg.mod_rate_hz);
        cfg.mod_depth = get_num(md, "depth", cfg.mod_depth);
        cfg.mod_fx_mix = get_num(md, "fx_mix", cfg.mod_fx_mix);
        cfg.mod_feedback = get_num(md, "feedback", cfg.mod_feedback);
        cfg.mod_position = get_num(md, "position", cfg.mod_position);
        cfg.mod_q = get_num(md, "q", cfg.mod_q);
        cfg.mod_mix = get_num(md, "mix", cfg.mod_mix);
        cfg.mod_low_hz = get_num(md, "low_hz", cfg.mod_low_hz);
        cfg.mod_high_hz = get_num(md, "high_hz", cfg.mod_high_hz);
    }

    auto amp = slot_or_empty(j, "amp");
    if (!amp.empty()) {
        cfg.amp_on = get_bool(amp, "on", cfg.amp_on);
        if (get_str(amp, "file", cfg.amp_file)) cfg.amp_on = true;
        cfg.amp_slim = get_num(amp, "slim", cfg.amp_slim);
        cfg.amp_trim_db = get_num(amp, "trim_db", cfg.amp_trim_db);
    }

    auto ir = slot_or_empty(j, "ir");
    if (!ir.empty()) {
        cfg.ir_on = get_bool(ir, "on", cfg.ir_on);
        if (get_str(ir, "file", cfg.ir_file)) cfg.ir_on = true;
        if (ir.contains("taps") && ir["taps"].is_number_integer()) {
            cfg.ir_taps = ir["taps"].get<unsigned>();
        }
    }

    auto dl = slot_or_empty(j, "delay");
    if (!dl.empty()) {
        cfg.delay_on = get_bool(dl, "on", cfg.delay_on);
        cfg.delay_time_ms = get_num(dl, "time_ms", cfg.delay_time_ms);
        cfg.delay_feedback = get_num(dl, "feedback", cfg.delay_feedback);
        cfg.delay_mix = get_num(dl, "mix", cfg.delay_mix);
        cfg.delay_tone = get_num(dl, "tone", cfg.delay_tone);
        if (dl.contains("mode") && dl["mode"].is_string()) {
            cfg.delay_mode = dl["mode"].get<std::string>();
        } else if (get_bool(dl, "ping_pong", false)) {
            cfg.delay_mode = "ping_pong";  // compatibilita' con la prima versione
        }
    }

    auto rv = slot_or_empty(j, "reverb");
    if (!rv.empty()) {
        cfg.reverb_on = get_bool(rv, "on", cfg.reverb_on);
        cfg.reverb_mix = get_num(rv, "mix", cfg.reverb_mix);
        cfg.reverb_size = get_num(rv, "size", cfg.reverb_size);
        cfg.reverb_damp = get_num(rv, "damp", cfg.reverb_damp);
    }

    // La mappa MIDI non si legge dai preset: vive in midi.json. Se un preset
    // vecchio la contiene lo diciamo, invece di ignorarla in silenzio.
    if (j.contains("midi")) {
        std::fprintf(stderr,
                     "preset: blocco \"midi\" ignorato (la mappa sta in config/midi.json)\n");
    }

    if (j.contains("stats") && j["stats"].is_boolean()) {
        cfg.stats = j["stats"].get<bool>();
    }
    return true;
}

// Riscrive il preset aggiornando solo gli stati on/off, preservando nome e
// campi che non conosciamo: un salvataggio non deve distruggere il file.
bool save_preset(const std::string& path, const Config& cfg, std::string& err) {
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(path);
        if (in) {
            try {
                in >> j;
            } catch (...) {
                j = nlohmann::json::object();
            }
        }
    }
    if (!j.is_object()) j = nlohmann::json::object();
    j["version"] = 1;
    if (!j.contains("slots") || !j["slots"].is_object()) j["slots"] = nlohmann::json::object();

    auto& slots = j["slots"];
    slots["gate"]["on"] = cfg.gate_on;
    slots["comp"]["on"] = cfg.comp_on;
    slots["drive"]["on"] = cfg.drive_on;
    slots["mod"]["on"] = cfg.mod_on;
    slots["amp"]["on"] = cfg.amp_on;
    slots["ir"]["on"] = cfg.ir_on;
    slots["delay"]["on"] = cfg.delay_on;
    slots["reverb"]["on"] = cfg.reverb_on;

    std::ofstream out(path);
    if (!out) {
        err = "impossibile scrivere " + path;
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

// --- riga di comando ------------------------------------------------------

// Override MIDI da riga di comando. Stanno a parte perche' midi.json si carica
// dopo il parsing e i flag, se presenti, devono vincere sul file.
struct MidiCli {
    bool device_set = false;
    std::string device;
    bool no_midi = false;
    bool expr_invert = false;
    bool wah_invert = false;
    bool config_set = false;
    std::string config_path;
};

bool parse_args(int argc, char** argv, Config& cfg, MidiCli& mcli, std::string& preset_path,
                bool& saw_flag, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];

        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                err = std::string("manca il valore per ") + name;
                return nullptr;
            }
            return argv[++i];
        };

        if (a == "-h" || a == "--help") {
            cfg.help = true;
            continue;
        }
        if (a == "-p" || a == "--preset") {
            const char* v = next("--preset");
            if (v == nullptr) return false;
            preset_path = v;
            continue;
        }
        if (a == "--stats") {
            cfg.stats = true;
            continue;
        }
        // Diagnostica: non e' una configurazione, quindi convive con -p.
        if (a == "--midi-log") {
            cfg.midi_log = true;
            continue;
        }
        if (a == "--print-config") {
            cfg.print_config = true;
            continue;
        }
        // La mappa MIDI vive in midi.json, non nei preset: questi flag non
        // rendono ambiguo un -p, quindi possono convivere.
        if (a == "--no-midi") {
            mcli.no_midi = true;
            continue;
        }
        if (a == "--expr-invert") {
            mcli.expr_invert = true;
            continue;
        }
        if (a == "--wah-invert") {
            mcli.wah_invert = true;
            continue;
        }
        if (a == "--midi-device") {
            const char* v = next("--midi-device");
            if (v == nullptr) return false;
            mcli.device = v;
            mcli.device_set = true;
            continue;
        }
        if (a == "--midi-config") {
            const char* v = next("--midi-config");
            if (v == nullptr) return false;
            mcli.config_path = v;
            mcli.config_set = true;
            continue;
        }

        saw_flag = true;

        if (a == "--device") {
            const char* v = next("--device");
            if (v == nullptr) return false;
            cfg.device = v;
        } else if (a == "--rate") {
            const char* v = next("--rate");
            if (v == nullptr || !parse_uint(v, cfg.rate)) {
                err = "valore non valido per --rate";
                return false;
            }
        } else if (a == "--buffer") {
            const char* v = next("--buffer");
            if (v == nullptr || !parse_uint(v, cfg.period)) {
                err = "valore non valido per --buffer";
                return false;
            }
        } else if (a == "--periods") {
            const char* v = next("--periods");
            if (v == nullptr || !parse_uint(v, cfg.periods)) {
                err = "valore non valido per --periods";
                return false;
            }
        } else if (a == "--gain") {
            const char* v = next("--gain");
            if (v == nullptr || !parse_float(v, cfg.gain_db)) {
                err = "valore non valido per --gain";
                return false;
            }
        } else if (a == "--nam") {
            const char* v = next("--nam");
            if (v == nullptr) return false;
            cfg.amp_file = v;
            cfg.amp_on = true;
        } else if (a == "--nam-slim") {
            const char* v = next("--nam-slim");
            if (v == nullptr || !parse_float(v, cfg.amp_slim)) {
                err = "valore non valido per --nam-slim";
                return false;
            }
        } else if (a == "--amp-gain") {
            const char* v = next("--amp-gain");
            if (v == nullptr || !parse_float(v, cfg.amp_trim_db)) {
                err = "valore non valido per --amp-gain";
                return false;
            }
        } else if (a == "--ir") {
            const char* v = next("--ir");
            if (v == nullptr) return false;
            cfg.ir_file = v;
            cfg.ir_on = true;
        } else if (a == "--taps") {
            const char* v = next("--taps");
            if (v == nullptr || !parse_uint(v, cfg.ir_taps)) {
                err = "valore non valido per --taps";
                return false;
            }
        } else if (a == "--tremolo") {
            cfg.mod_on = true;
            cfg.mod_type = "tremolo";
        } else if (a == "--wah") {
            cfg.mod_on = true;
            cfg.mod_type = "wah";
        } else if (a == "--wah-position") {
            const char* v = next("--wah-position");
            if (v == nullptr || !parse_float(v, cfg.mod_position)) {
                err = "valore non valido per --wah-position";
                return false;
            }
        } else if (a == "--wah-q") {
            const char* v = next("--wah-q");
            if (v == nullptr || !parse_float(v, cfg.mod_q)) {
                err = "valore non valido per --wah-q";
                return false;
            }
        } else if (a == "--wah-mix") {
            const char* v = next("--wah-mix");
            if (v == nullptr || !parse_float(v, cfg.mod_mix)) {
                err = "valore non valido per --wah-mix";
                return false;
            }
        } else if (a == "--tremolo-rate") {
            const char* v = next("--tremolo-rate");
            if (v == nullptr || !parse_float(v, cfg.mod_rate_hz)) {
                err = "valore non valido per --tremolo-rate";
                return false;
            }
        } else if (a == "--tremolo-depth") {
            const char* v = next("--tremolo-depth");
            if (v == nullptr || !parse_float(v, cfg.mod_depth)) {
                err = "valore non valido per --tremolo-depth";
                return false;
            }
        } else if (a == "--gate") {
            cfg.gate_on = true;
        } else if (a == "--gate-threshold") {
            const char* v = next("--gate-threshold");
            if (v == nullptr || !parse_float(v, cfg.gate_threshold_db)) {
                err = "valore non valido per --gate-threshold";
                return false;
            }
        } else if (a == "--gate-attack") {
            const char* v = next("--gate-attack");
            if (v == nullptr || !parse_float(v, cfg.gate_attack_ms)) {
                err = "valore non valido per --gate-attack";
                return false;
            }
        } else if (a == "--gate-release") {
            const char* v = next("--gate-release");
            if (v == nullptr || !parse_float(v, cfg.gate_release_ms)) {
                err = "valore non valido per --gate-release";
                return false;
            }
        } else if (a == "--comp") {
            cfg.comp_on = true;
        } else if (a == "--drive") {
            cfg.drive_on = true;
        } else if (a == "--drive-type") {
            const char* v = next("--drive-type");
            if (v == nullptr) return false;
            cfg.drive_type = v;
        } else if (a == "--drive-mid") {
            const char* v = next("--drive-mid");
            if (v == nullptr || !parse_float(v, cfg.drive_mid_db)) {
                err = "valore non valido per --drive-mid";
                return false;
            }
        } else if (a == "--drive-low") {
            const char* v = next("--drive-low");
            if (v == nullptr || !parse_float(v, cfg.drive_low_db)) {
                err = "valore non valido per --drive-low";
                return false;
            }
        } else if (a == "--drive-blend") {
            const char* v = next("--drive-blend");
            if (v == nullptr || !parse_float(v, cfg.drive_blend)) {
                err = "valore non valido per --drive-blend";
                return false;
            }
        } else if (a == "--drive-amount") {
            const char* v = next("--drive-amount");
            if (v == nullptr || !parse_float(v, cfg.drive_amount)) {
                err = "valore non valido per --drive-amount";
                return false;
            }
        } else if (a == "--drive-tone") {
            const char* v = next("--drive-tone");
            if (v == nullptr || !parse_float(v, cfg.drive_tone)) {
                err = "valore non valido per --drive-tone";
                return false;
            }
        } else if (a == "--delay") {
            cfg.delay_on = true;
        } else if (a == "--delay-time") {
            const char* v = next("--delay-time");
            if (v == nullptr || !parse_float(v, cfg.delay_time_ms)) {
                err = "valore non valido per --delay-time";
                return false;
            }
        } else if (a == "--delay-feedback") {
            const char* v = next("--delay-feedback");
            if (v == nullptr || !parse_float(v, cfg.delay_feedback)) {
                err = "valore non valido per --delay-feedback";
                return false;
            }
        } else if (a == "--delay-mix") {
            const char* v = next("--delay-mix");
            if (v == nullptr || !parse_float(v, cfg.delay_mix)) {
                err = "valore non valido per --delay-mix";
                return false;
            }
        } else if (a == "--delay-ping-pong") {
            cfg.delay_mode = "ping_pong";
        } else if (a == "--delay-ping-pong-stereo") {
            cfg.delay_mode = "ping_pong_stereo";
        } else if (a == "--reverb") {
            cfg.reverb_on = true;
        } else if (a == "--reverb-mix") {
            const char* v = next("--reverb-mix");
            if (v == nullptr || !parse_float(v, cfg.reverb_mix)) {
                err = "valore non valido per --reverb-mix";
                return false;
            }
        } else if (a == "--reverb-size") {
            const char* v = next("--reverb-size");
            if (v == nullptr || !parse_float(v, cfg.reverb_size)) {
                err = "valore non valido per --reverb-size";
                return false;
            }
        } else {
            err = "opzione sconosciuta: " + a;
            return false;
        }
    }
    return true;
}

inline float to_float(int32_t x) {
    return static_cast<float>(x) * (1.0f / 2147483648.0f);
}

inline int32_t to_i32(float x) {
    if (x > 1.0f) x = 1.0f;
    if (x < -1.0f) x = -1.0f;
    return static_cast<int32_t>(x * 2147483520.0f);
}

inline float soft_clip(float x) {
    constexpr float t = 0.8f;
    if (x > t) return t + (1.0f - t) * std::tanh((x - t) / (1.0f - t));
    if (x < -t) return -t - (1.0f - t) * std::tanh((-x - t) / (1.0f - t));
    return x;
}

uint64_t elapsed_us(const timespec& a, const timespec& b) {
    const int64_t s = static_cast<int64_t>(b.tv_sec - a.tv_sec);
    const int64_t ns = static_cast<int64_t>(b.tv_nsec - a.tv_nsec);
    return static_cast<uint64_t>(s * 1000000 + ns / 1000);
}

void store_max(std::atomic<uint64_t>& target, uint64_t value) {
    uint64_t prev = target.load(std::memory_order_relaxed);
    while (value > prev &&
           !target.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    MidiCli midi_cli;
    std::string preset_path;
    std::string err;
    bool saw_flag = false;

    if (!parse_args(argc, argv, cfg, midi_cli, preset_path, saw_flag, err)) {
        std::fprintf(stderr, "errore: %s\n\n", err.c_str());
        usage();
        return 2;
    }
    if (cfg.help) {
        usage();
        return 0;
    }

    if (!preset_path.empty()) {
        // Preset e flag sono alternativi: mescolarli renderebbe ambiguo chi
        // vince, e i due percorsi divergerebbero in silenzio.
        if (saw_flag) {
            std::fprintf(stderr, "errore: --preset non si combina con gli altri flag\n");
            return 2;
        }
        Config from_file;
        if (!load_preset(preset_path, from_file, err)) {
            std::fprintf(stderr, "preset: %s\n", err.c_str());
            return 1;
        }
        from_file.stats = cfg.stats;
        from_file.midi_log = cfg.midi_log;
        from_file.print_config = cfg.print_config;
        cfg = from_file;
        std::printf("preset: %s\n", preset_path.c_str());
    }

    // Un tipo scritto male non deve diventare un modello a caso: si torna al
    // default e lo si dice, invece di far suonare qualcosa di inatteso.
    if (drive_type_index(cfg.drive_type) < 0) {
        std::fprintf(stderr, "drive: tipo \"%s\" sconosciuto, uso fuzz\n",
                     cfg.drive_type.c_str());
        cfg.drive_type = "fuzz";
    }
    if (namzero::mod_type_index(cfg.mod_type) < 0) {
        std::fprintf(stderr, "mod: tipo \"%s\" sconosciuto, uso tremolo\n",
                     cfg.mod_type.c_str());
        cfg.mod_type = "tremolo";
    }

    // --- mappa MIDI: file globale, non preset ------------------------------
    // Precedenza: default compilati < midi.json < flag da riga di comando.
    namzero::MidiConfig midi_cfg;
    {
        std::string midi_path;
        if (midi_cli.config_set) {
            midi_path = midi_cli.config_path;
        } else {
            // Si prova prima la posizione naturale dalla radice del progetto,
            // poi quella accanto all'eseguibile: nessuna magia oltre a questa.
            std::ifstream a("config/midi.json");
            std::ifstream b("midi.json");
            if (a) midi_path = "config/midi.json";
            else if (b) midi_path = "midi.json";
        }

        if (!midi_path.empty()) {
            std::ifstream exists(midi_path);
            if (exists) {
                if (!namzero::load_midi_config(midi_path, midi_cfg, err)) {
                    std::fprintf(stderr, "errore: %s\n", err.c_str());
                    return 2;
                }
                std::printf("midi: config %s\n", midi_path.c_str());
            } else if (midi_cli.config_set) {
                std::fprintf(stderr, "errore: midi.json non leggibile: %s\n", midi_path.c_str());
                return 2;
            }
        }
    }
    if (midi_cli.device_set) midi_cfg.device = midi_cli.device;
    if (midi_cli.no_midi) midi_cfg.enabled = false;
    if (midi_cli.expr_invert) midi_cfg.expression_invert = true;
    if (midi_cli.wah_invert) midi_cfg.wah_invert = true;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Ordine della catena: gate -> comp -> drive -> amp -> ir -> reverb.
    // Gli indici si memorizzano man mano: gli slot opzionali sposterebbero gli altri.
    namzero::Chain chain;

    // I parametri vanno applicati allo slot: senza queste chiamate il gate (e
    // il comp) restavano sui default compilati, qualunque cosa dicesse il preset.
    const std::size_t i_gate = chain.size();
    namzero::GateSlot* gate_ptr = nullptr;
    {
        auto gt = std::make_unique<namzero::GateSlot>();
        gate_ptr = gt.get();
        gt->set_threshold_db(static_cast<float>(cfg.gate_threshold_db));
        gt->set_attack_ms(static_cast<float>(cfg.gate_attack_ms));
        gt->set_release_ms(static_cast<float>(cfg.gate_release_ms));
        chain.add(std::move(gt));
    }
    const std::size_t i_comp = chain.size();
    namzero::CompSlot* comp_ptr = nullptr;
    {
        auto cp = std::make_unique<namzero::CompSlot>();
        comp_ptr = cp.get();
        cp->set_threshold_db(static_cast<float>(cfg.comp_threshold_db));
        cp->set_ratio(static_cast<float>(cfg.comp_ratio));
        cp->set_attack_ms(static_cast<float>(cfg.comp_attack_ms));
        cp->set_release_ms(static_cast<float>(cfg.comp_release_ms));
        cp->set_makeup_db(static_cast<float>(cfg.comp_makeup_db));
        chain.add(std::move(cp));
    }
    const std::size_t i_drive = chain.size();
    namzero::DriveSlot* drive_ptr = nullptr;
    {
        auto dr = std::make_unique<namzero::DriveSlot>();
        drive_ptr = dr.get();
        dr->set_type(drive_type_enum(drive_type_index(cfg.drive_type)));
        dr->set_drive(static_cast<float>(cfg.drive_amount));
        dr->set_tone(static_cast<float>(cfg.drive_tone));
        dr->set_level_db(static_cast<float>(cfg.drive_level_db));
        dr->set_blend(static_cast<float>(cfg.drive_blend));
        dr->set_mid_db(static_cast<float>(cfg.drive_mid_db));
        dr->set_low_db(static_cast<float>(cfg.drive_low_db));
        chain.add(std::move(dr));
    }

    const std::size_t i_mod = chain.size();
    auto mod_owner = std::make_unique<namzero::ModSlot>();
    namzero::ModSlot* mod_ptr = mod_owner.get();
    auto mod_slot = std::move(mod_owner);
    mod_ptr->set_type(mod_type_enum(namzero::mod_type_index(cfg.mod_type)));
    mod_slot->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
    mod_slot->set_depth(static_cast<float>(cfg.mod_depth));
    mod_ptr->set_position(static_cast<float>(cfg.mod_position));
    mod_slot->set_wah_q(static_cast<float>(cfg.mod_q));
    mod_slot->set_wah_mix(static_cast<float>(cfg.mod_mix));
    mod_slot->set_wah_low_hz(static_cast<float>(cfg.mod_low_hz));
    mod_slot->set_wah_high_hz(static_cast<float>(cfg.mod_high_hz));
    mod_slot->set_fx_mix(static_cast<float>(cfg.mod_fx_mix));
    mod_slot->set_feedback(static_cast<float>(cfg.mod_feedback));
    chain.add(std::move(mod_slot));

    const std::size_t i_amp = chain.size();
    namzero::NamSlot* amp_ptr = nullptr;
    const bool has_amp = cfg.amp_on && !cfg.amp_file.empty();
    if (has_amp) {
        auto amp = std::make_unique<namzero::NamSlot>();
        amp_ptr = amp.get();
        if (!amp->load(cfg.amp_file, cfg.amp_slim, err)) {
            std::fprintf(stderr, "amp: %s\n", err.c_str());
            return 1;
        }
        amp->set_trim_db(cfg.amp_trim_db);
        std::printf("amp: %s | slim %.2f%s | gain modello %.2f | trim %+.1f dB\n",
                    cfg.amp_file.c_str(), cfg.amp_slim,
                    amp->slimmable() ? "" : " (non slimmable)", amp->model_gain(),
                    cfg.amp_trim_db);
        chain.add(std::move(amp));
    } else {
        chain.add(std::make_unique<namzero::GainSlot>(0.0f));
    }

    std::size_t i_ir = kNoSlot;
    namzero::IrSlot* ir_ptr = nullptr;
    const bool has_ir = cfg.ir_on && !cfg.ir_file.empty();
    if (has_ir) {
        i_ir = chain.size();
        auto ir = std::make_unique<namzero::IrSlot>();
        ir_ptr = ir.get();
        if (!ir->load(cfg.ir_file, cfg.ir_taps, err)) {
            std::fprintf(stderr, "IR: %s\n", err.c_str());
            return 1;
        }
        std::printf("IR: %s | %zu tap (%.1f ms @ 48 kHz)\n", cfg.ir_file.c_str(),
                    ir->taps(), static_cast<double>(ir->taps()) * 1000.0 / 48000.0);
        chain.add(std::move(ir));
    }

    // Delay e riverbero esistono sempre, anche se spenti: cosi' il tasto di
    // bypass funziona con qualunque preset, invece di non fare nulla in silenzio.
    const std::size_t i_delay = chain.size();
    namzero::DelaySlot* delay_slot = nullptr;
    {
        auto dl = std::make_unique<namzero::DelaySlot>();
        delay_slot = dl.get();
        dl->set_time_ms(static_cast<float>(cfg.delay_time_ms));
        dl->set_feedback(static_cast<float>(cfg.delay_feedback));
        dl->set_mix(static_cast<float>(cfg.delay_mix));
        dl->set_tone(static_cast<float>(cfg.delay_tone));
        if (cfg.delay_mode == "ping_pong") {
            dl->set_mode(namzero::DelaySlot::Mode::kPingPong);
        } else if (cfg.delay_mode == "ping_pong_stereo") {
            dl->set_mode(namzero::DelaySlot::Mode::kPingPongStereo);
        } else {
            dl->set_mode(namzero::DelaySlot::Mode::kStereo);
        }
        chain.add(std::move(dl));
    }

    const std::size_t i_reverb = chain.size();
    namzero::ReverbSlot* reverb_ptr = nullptr;
    {
        auto rv = std::make_unique<namzero::ReverbSlot>();
        reverb_ptr = rv.get();
        rv->set_wet(static_cast<float>(cfg.reverb_mix));
        rv->set_room_size(static_cast<float>(cfg.reverb_size));
        rv->set_damp(static_cast<float>(cfg.reverb_damp));
        chain.add(std::move(rv));
    }


    // --print-config: valori letti DAGLI SLOT, non dal Config. E' la verifica
    // che il preset sia arrivato fino in fondo (lettura + applicazione); il
    // confronto con i JSON lo fa scripts/verify_presets.py.
    if (cfg.print_config) {
        auto mod_name = [](namzero::ModSlot::Type t) {
            switch (t) {
                case namzero::ModSlot::Type::kWah: return "wah";
                case namzero::ModSlot::Type::kUnivibe: return "univibe";
                case namzero::ModSlot::Type::kFlanger: return "flanger";
                case namzero::ModSlot::Type::kPhaser: return "phaser";
                default: return "tremolo";
            }
        };
        auto drive_name = [](namzero::DriveSlot::Type t) {
            switch (t) {
                case namzero::DriveSlot::Type::kKlon: return "klon";
                case namzero::DriveSlot::Type::kTubescreamer: return "tubescreamer";
                default: return "fuzz";
            }
        };
        auto delay_mode_name = [](namzero::DelaySlot::Mode m) {
            switch (m) {
                case namzero::DelaySlot::Mode::kPingPong: return "ping_pong";
                case namzero::DelaySlot::Mode::kPingPongStereo: return "ping_pong_stereo";
                default: return "stereo";
            }
        };

        std::printf("cfg.gate.threshold_db=%.4f\n", gate_ptr->threshold_db());
        std::printf("cfg.gate.attack_ms=%.4f\n", gate_ptr->attack_ms());
        std::printf("cfg.gate.release_ms=%.4f\n", gate_ptr->release_ms());

        std::printf("cfg.comp.threshold_db=%.4f\n", comp_ptr->threshold_db());
        std::printf("cfg.comp.ratio=%.4f\n", comp_ptr->ratio());
        std::printf("cfg.comp.attack_ms=%.4f\n", comp_ptr->attack_ms());
        std::printf("cfg.comp.release_ms=%.4f\n", comp_ptr->release_ms());
        std::printf("cfg.comp.makeup_db=%.4f\n", comp_ptr->makeup_db());

        std::printf("cfg.drive.type=%s\n", drive_name(drive_ptr->type()));
        std::printf("cfg.drive.amount=%.4f\n", drive_ptr->drive());
        std::printf("cfg.drive.tone=%.4f\n", drive_ptr->tone());
        std::printf("cfg.drive.level_db=%.4f\n", drive_ptr->level_db());
        std::printf("cfg.drive.blend=%.4f\n", drive_ptr->blend());
        std::printf("cfg.drive.mid_db=%.4f\n", drive_ptr->mid_db());
        std::printf("cfg.drive.low_db=%.4f\n", drive_ptr->low_db());

        std::printf("cfg.mod.type=%s\n", mod_name(mod_ptr->type()));
        std::printf("cfg.mod.rate_hz=%.4f\n", mod_ptr->rate_hz());
        std::printf("cfg.mod.depth=%.4f\n", mod_ptr->depth());
        std::printf("cfg.mod.position=%.4f\n", mod_ptr->position());
        std::printf("cfg.mod.q=%.4f\n", mod_ptr->wah_q());
        std::printf("cfg.mod.mix=%.4f\n", mod_ptr->wah_mix());
        std::printf("cfg.mod.low_hz=%.4f\n", mod_ptr->wah_low_hz());
        std::printf("cfg.mod.high_hz=%.4f\n", mod_ptr->wah_high_hz());
        std::printf("cfg.mod.fx_mix=%.4f\n", mod_ptr->fx_mix());
        std::printf("cfg.mod.feedback=%.4f\n", mod_ptr->feedback());

        std::printf("cfg.amp.file=%s\n", cfg.amp_file.c_str());
        std::printf("cfg.amp.slim=%.4f\n", cfg.amp_slim);
        std::printf("cfg.amp.trim_db=%.4f\n", cfg.amp_trim_db);

        std::printf("cfg.ir.file=%s\n", cfg.ir_file.c_str());
        std::printf("cfg.ir.taps=%zu\n", ir_ptr != nullptr ? ir_ptr->taps() : 0u);

        std::printf("cfg.delay.time_ms=%.4f\n", delay_slot->time_ms());
        std::printf("cfg.delay.feedback=%.4f\n", delay_slot->feedback());
        std::printf("cfg.delay.mix=%.4f\n", delay_slot->mix());
        std::printf("cfg.delay.tone=%.4f\n", delay_slot->tone());
        std::printf("cfg.delay.mode=%s\n", delay_mode_name(delay_slot->mode()));

        std::printf("cfg.reverb.mix=%.4f\n", reverb_ptr->wet());
        std::printf("cfg.reverb.size=%.4f\n", reverb_ptr->room_size());
        std::printf("cfg.reverb.damp=%.4f\n", reverb_ptr->damp());
        return 0;
    }


    // Istanza singola: il device audio e' esclusivo, e un residuo in background
    // fa fallire l'avvio con un "device busy" che sembra un problema di config.
    const int lock_fd = ::open("/tmp/namzero-lite.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd >= 0 && ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        // Il PID nel file dice chi tiene il device: senza, l'unica via e' pkill.
        char buf[32] = {0};
        const ssize_t got = ::pread(lock_fd, buf, sizeof(buf) - 1, 0);
        if (got > 0) {
            std::fprintf(stderr,
                         "errore: gia' in esecuzione il processo %s "
                         "(fermalo con: kill %s)\n",
                         buf, buf);
        } else {
            std::fprintf(stderr, "errore: un'altra istanza e' gia' in esecuzione\n");
        }
        return 1;
    }
    if (lock_fd >= 0) {
        const std::string pid = std::to_string(::getpid());
        ::ftruncate(lock_fd, 0);
        ::pwrite(lock_fd, pid.c_str(), pid.size(), 0);
    }

    namzero::AudioConfig acfg;
    acfg.device = cfg.device;
    acfg.rate = cfg.rate;
    acfg.period_frames = cfg.period;
    acfg.periods = cfg.periods;

    namzero::AlsaDuplex io;
    if (!io.open(acfg, err)) {
        std::fprintf(stderr, "audio: %s\n", err.c_str());
        return 1;
    }
    std::printf("audio: %s | %u Hz | periodo %u frame | buffer %u frame | %u periodi\n",
                io.device().c_str(), io.rate(), io.period_frames(),
                io.buffer_frames(), io.periods());
    chain.prepare(io.rate(), io.period_frames());

    // MIDI: si apre prima di costruire la catena, perche' se il pedale c'e'
    // l'auto-engage prende il comando dello slot mod fin dall'avvio.
    namzero::MidiInput midi;
    if (midi_cfg.enabled) {
        std::string merr;
        if (midi.open(midi_cfg.device, merr)) {
            std::printf("midi: %s | espressione CC%d | switch %s\n", midi_cfg.device.c_str(),
                        midi_cfg.expression_cc, midi_cfg.switches.c_str());
            for (std::size_t i = 0; i < namzero::kMidiActionCount; ++i) {
                const int cc = midi_cfg.slot_cc[i];
                if (cc >= 0) {
                    std::printf("  %-6s CC%d\n",
                                namzero::midi_action_name(static_cast<namzero::MidiAction>(i)),
                                cc);
                }
            }
        } else {
            std::fprintf(stderr, "midi: %s (si prosegue senza)\n", merr.c_str());
        }
    }
    // Bridge BLE: il motore fa da server, il bridge si connette e scrive byte
    // MIDI. Se la socket non si apre si prosegue: il pedale USB funziona lo stesso.
    namzero::MidiSocket ble;
    if (!midi_cfg.ble_socket.empty()) {
        std::string berr;
        if (ble.listen(midi_cfg.ble_socket, berr)) {
            std::printf("midi: bridge BLE in ascolto su %s\n", midi_cfg.ble_socket.c_str());
        } else {
            std::fprintf(stderr, "midi: bridge BLE: %s (si prosegue senza)\n", berr.c_str());
        }
    }

    const bool wah_armed = midi.is_open();

    // Regola: il wah non parte mai acceso, lo accende solo il pedale. Se il
    // MIDI non c'e' (o e' disattivato) resta il controllo manuale, altrimenti
    // un preset col wah dentro non avrebbe modo di suonare.
    if (wah_armed && cfg.mod_type == "wah" && cfg.mod_on) {
        cfg.mod_on = false;
        std::printf("mod: wah spento all'avvio (si accende col pedale)\n");
    }

    chain.set_enabled(i_gate, cfg.gate_on);
    chain.set_enabled(i_comp, cfg.comp_on);
    chain.set_enabled(i_drive, cfg.drive_on);
    chain.set_enabled(i_mod, cfg.mod_on);
    chain.set_enabled(i_amp, has_amp);
    if (i_ir != kNoSlot) chain.set_enabled(i_ir, true);
    chain.set_enabled(i_delay, cfg.delay_on);
    chain.set_enabled(i_reverb, cfg.reverb_on);
    chain.reset();

    std::printf("gate: soglia %.1f dB | attack %.1f ms | release %.0f ms\n",
                cfg.gate_threshold_db, cfg.gate_attack_ms, cfg.gate_release_ms);
    std::printf("comp: soglia %.1f dB | ratio %.1f | attack %.1f ms | release %.0f ms | makeup %.1f dB\n",
                cfg.comp_threshold_db, cfg.comp_ratio, cfg.comp_attack_ms, cfg.comp_release_ms,
                cfg.comp_makeup_db);
    std::printf("slot: gate %s | comp %s | drive %s (%s) | mod %s (%s) | amp %s | ir %s | delay %s | reverb %s\n",
                cfg.gate_on ? "on" : "off", cfg.comp_on ? "on" : "off",
                cfg.drive_on ? "on" : "off", cfg.drive_type.c_str(),
                cfg.mod_on ? "on" : "off", cfg.mod_type.c_str(),
                has_amp ? "on" : "off", has_ir ? "on" : "off",
                cfg.delay_on ? "on" : "off", cfg.reverb_on ? "on" : "off");
    const std::size_t latency = chain.latency_frames();
    std::printf("latenza catena: %zu campioni (%.2f ms)\n", latency,
                static_cast<double>(latency) * 1000.0 / 48000.0);
    std::printf("tasti: g gate | c comp | o overdrive | k modello drive | p modo delay | + - volume amp\n"
                "        m mod | w tipo mod | , . wah\n"
                "        d delay | t tap tempo | i ir | r reverb | s salva | q o Ctrl+C esci\n");
    std::fflush(stdout);

    // CC -> azione, costruita una volta sola: il thread MIDI non deve fare una
    // catena di confronti. midi.json ha gia' rifiutato i doppioni.
    std::array<int8_t, 128> cc_action{};
    cc_action.fill(-1);
    for (std::size_t i = 0; i < namzero::kMidiActionCount; ++i) {
        const int cc = midi_cfg.slot_cc[i];
        if (cc >= 0) cc_action[static_cast<std::size_t>(cc)] = static_cast<int8_t>(i);
    }
    std::array<int8_t, 128> cc_mod_select{};
    cc_mod_select.fill(-1);
    for (std::size_t i = 0; i < namzero::kModTypeCount; ++i) {
        const int cc = midi_cfg.mod_select_cc[i];
        if (cc >= 0) cc_mod_select[static_cast<std::size_t>(cc)] = static_cast<int8_t>(i);
    }

    const std::array<std::size_t, namzero::kMidiActionCount> slot_index = {
        i_gate, i_comp, i_drive, i_mod, i_amp, i_ir, i_delay, i_reverb};
    std::array<bool*, namzero::kMidiActionCount> slot_flag = {
        &cfg.gate_on, &cfg.comp_on, &cfg.drive_on, &cfg.mod_on,
        &cfg.amp_on,  &cfg.ir_on,  &cfg.delay_on, &cfg.reverb_on};

    const std::size_t frames = io.period_frames();
    const std::size_t samples = frames * 2;

    std::vector<int32_t> in_i32(samples, 0);
    std::vector<int32_t> out_i32(samples, 0);
    std::vector<float> buf(samples, 0.0f);

    if (!io.prime_playback(2 * io.period_frames())) {
        std::fprintf(stderr,
                     "attenzione: priming del playback fallito, "
                     "possibile underrun iniziale\n");
    }

    if (!namzero::rt::install(err)) {
        // Non e' un errore fatale: senza priorita' FIFO si suona lo stesso, ma
        // con buffer piccoli gli xrun diventano probabili. Il rimedio va detto,
        // altrimenti sembra che il programma sia rotto.
        std::fprintf(stderr,
                     "nota: priorita' realtime non disponibile (%s)\n"
                     "      si prosegue a priorita' normale: possibili xrun con buffer piccoli.\n"
                     "      per abilitarla, dopo ogni build: scripts/build.sh\n"
                     "      (oppure: sudo setcap cap_sys_nice,cap_ipc_lock+ep ./build/namzero-lite)\n",
                     err.c_str());
    }

    Stats stats;
    std::atomic<bool> stats_stop{false};

    // In modalita' normale il thread tace: parla solo se qualcosa non va.
    const double period_us =
        static_cast<double>(io.period_frames()) * 1e6 / static_cast<double>(io.rate());
    const uint64_t warn_us = static_cast<uint64_t>(period_us * 0.85);
    const bool verbose = cfg.stats;

    std::thread stats_thread([&stats, &stats_stop, verbose, period_us, warn_us]() {
        uint64_t prev_blocks = 0;
        uint64_t prev_xruns = 0;
        bool first = true;

        while (g_running.load() && !stats_stop.load()) {
            for (int i = 0; i < 50 && g_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const uint64_t blocks = stats.blocks.load();
            const uint64_t xruns = stats.xruns.load();
            const uint64_t max_us = stats.max_us.exchange(0);
            const uint64_t new_xruns = xruns - prev_xruns;

            if (verbose) {
                std::printf("blocchi %" PRIu64 " (+%" PRIu64 ") | xrun %" PRIu64
                            " (+%" PRIu64 ") | max blocco %.2f ms\n",
                            blocks, blocks - prev_blocks, xruns, new_xruns,
                            static_cast<double>(max_us) / 1000.0);
                std::fflush(stdout);
            } else {
                // Il primo intervallo contiene gli xrun di priming: non sono un
                // problema e non meritano un avviso.
                const bool real_xruns = new_xruns > 0 && !first;
                const bool tight = max_us > warn_us;
                if (real_xruns || tight) {
                    std::fprintf(stderr,
                                 "attenzione: +%" PRIu64 " xrun | max blocco %.2f ms "
                                 "(%.0f%% del periodo)\n",
                                 new_xruns, static_cast<double>(max_us) / 1000.0,
                                 100.0 * static_cast<double>(max_us) / period_us);
                    std::fflush(stderr);
                }
            }
            first = false;
            prev_blocks = blocks;
            prev_xruns = xruns;
        }
    });

    // Tastiera: toggle degli slot mentre suona, senza toccare i file.
    // stdin e' in modalita' raw, quindi i tasti arrivano senza Invio.
    // Il mutex serializza tastiera e MIDI: scrivono entrambi lo stesso stato.
    std::mutex ctrl_mtx;
    // true mentre il pedale tiene acceso il wah: la tastiera non deve toccare
    // lo slot mod in quel momento, o il ripristino al rilascio sarebbe sbagliato.
    std::atomic<bool> wah_engaged{false};
    std::thread kb_thread([&chain, &cfg, &preset_path, &ctrl_mtx, &wah_engaged, wah_armed,
                           drive_ptr, amp_ptr, i_gate, i_comp, i_drive, i_mod,
                           i_delay, i_ir, i_reverb, mod_ptr, delay_slot]() {
        termios oldt{};
        const bool is_tty = (tcgetattr(STDIN_FILENO, &oldt) == 0);
        if (is_tty) {
            termios raw = oldt;
            cfmakeraw(&raw);
            // cfmakeraw spegne ISIG: senza, ^C non arriva come SIGINT ma come
            // byte 0x03, che il ciclo dei tasti non riconosce. Il programma si
            // fermava solo con 'q'.
            raw.c_lflag |= ISIG;
            // ^Z resta disattivato: sospendere un processo che tiene il device
            // audio aperto lascia il terminale in raw mode e il device occupato.
            raw.c_cc[VSUSP] = _POSIX_VDISABLE;
            // cfmakeraw toglie anche OPOST: senza di esso '\n' scende di una
            // riga senza tornare a capo, e le stampe si sfalsano a scaletta.
            raw.c_oflag |= (OPOST | ONLCR);
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }

        auto toggle = [&](const char* label, std::size_t idx, bool& flag) {
            if (idx == kNoSlot) {
                std::printf("  %s: non presente in questo preset\n", label);
                std::fflush(stdout);
                return;
            }
            flag = !flag;
            chain.set_enabled(idx, flag);
            std::printf("  %s: %s%s\n", label, flag ? "on" : "off",
                        preset_path.empty() ? "" : "   (s per salvare)");
            std::fflush(stdout);
        };

        // Tap tempo: la media degli ultimi intervalli, non l'ultimo.
        auto last_tap = std::chrono::steady_clock::now();
        bool have_tap = false;
        double tap_ms = 0.0;

        while (g_running.load()) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            const int r = poll(&p, 1, 100);
            if (r <= 0) continue;

            char ch = 0;
            if (::read(STDIN_FILENO, &ch, 1) != 1) {
                if (!is_tty) break;
                continue;
            }
            switch (ch) {
                case 'g': toggle("gate", i_gate, cfg.gate_on); break;
                case 'c': toggle("comp", i_comp, cfg.comp_on); break;
                case 'o': {
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    toggle("overdrive", i_drive, cfg.drive_on);
                    break;
                }
                case 'k': {
                    if (drive_ptr == nullptr) break;
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    int idx = drive_type_index(cfg.drive_type);
                    idx = (idx + 1) % static_cast<int>(kDriveTypeCount);
                    cfg.drive_type = kDriveTypeNames[idx];
                    drive_ptr->set_type(drive_type_enum(idx));
                    std::printf("  drive: %s\n", cfg.drive_type.c_str());
                    std::fflush(stdout);
                    break;
                }
                case 'p': {
                    if (delay_slot == nullptr) break;
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    if (cfg.delay_mode == "stereo") {
                        cfg.delay_mode = "ping_pong";
                        delay_slot->set_mode(namzero::DelaySlot::Mode::kPingPong);
                    } else if (cfg.delay_mode == "ping_pong") {
                        cfg.delay_mode = "ping_pong_stereo";
                        delay_slot->set_mode(namzero::DelaySlot::Mode::kPingPongStereo);
                    } else {
                        cfg.delay_mode = "stereo";
                        delay_slot->set_mode(namzero::DelaySlot::Mode::kStereo);
                    }
                    std::printf("  delay: %s\n", cfg.delay_mode.c_str());
                    std::fflush(stdout);
                    break;
                }
                case '+':
                case '=': {
                    if (amp_ptr == nullptr) {
                        std::printf("  amp: non presente in questo preset\n");
                        std::fflush(stdout);
                        break;
                    }
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    cfg.amp_trim_db = cfg.amp_trim_db >= 24.0 ? 24.0 : cfg.amp_trim_db + 1.0;
                    amp_ptr->set_trim_db(cfg.amp_trim_db);
                    std::printf("  amp: %+.1f dB\n", cfg.amp_trim_db);
                    std::fflush(stdout);
                    break;
                }
                case '-': {
                    if (amp_ptr == nullptr) {
                        std::printf("  amp: non presente in questo preset\n");
                        std::fflush(stdout);
                        break;
                    }
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    cfg.amp_trim_db = cfg.amp_trim_db <= -24.0 ? -24.0 : cfg.amp_trim_db - 1.0;
                    amp_ptr->set_trim_db(cfg.amp_trim_db);
                    std::printf("  amp: %+.1f dB\n", cfg.amp_trim_db);
                    std::fflush(stdout);
                    break;
                }
                case 'd': toggle("delay", i_delay, cfg.delay_on); break;
                case 't': {
                    if (delay_slot == nullptr) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (have_tap) {
                        const double dt = std::chrono::duration<double, std::milli>(
                                              now - last_tap).count();
                        if (dt > 120.0 && dt < 2000.0) {
                            // Media pesata: il primo intervallo vale 1, poi 0,3.
                            tap_ms = (tap_ms > 0.0) ? (0.7 * tap_ms + 0.3 * dt) : dt;
                            cfg.delay_time_ms = tap_ms;
                            delay_slot->set_time_ms(static_cast<float>(tap_ms));
                            std::printf("  delay: %.0f ms (tap)\n", tap_ms);
                            std::fflush(stdout);
                        } else {
                            tap_ms = 0.0;  // pausa lunga: riparte
                        }
                    }
                    have_tap = true;
                    last_tap = now;
                    break;
                }
                case 'm': {
                    if (wah_engaged.load()) {
                        std::printf("  (il wah e' in mano al pedale)\n");
                        std::fflush(stdout);
                        break;
                    }
                    if (wah_armed && cfg.mod_type == "wah") {
                        std::printf("  (il wah si accende col pedale, non da qui)\n");
                        std::fflush(stdout);
                        break;
                    }
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    toggle("mod", i_mod, cfg.mod_on);
                    break;
                }
                case 'w': {
                    if (mod_ptr == nullptr) break;
                    if (wah_engaged.load()) {
                        std::printf("  (il wah e' in mano al pedale)\n");
                        std::fflush(stdout);
                        break;
                    }
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    // Si scorre solo fra gli effetti: il wah si usa dal pedale.
                    const int cur = namzero::mod_type_index(cfg.mod_type);
                    int next = kModCycle[0];
                    for (int i = 0; i < kModCycleCount; ++i) {
                        if (kModCycle[i] == cur) {
                            next = kModCycle[(i + 1) % kModCycleCount];
                            break;
                        }
                    }
                    cfg.mod_type = namzero::kModTypeNames[next];
                    cfg.mod_rate_hz = kModDefaultRate[next];
                    cfg.mod_feedback = kModDefaultFeedback[next];
                    mod_ptr->set_type(mod_type_enum(next));
                    mod_ptr->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
                    mod_ptr->set_feedback(static_cast<float>(cfg.mod_feedback));
                    std::printf("  mod: %s (rate %.1f Hz)\n", cfg.mod_type.c_str(),
                                cfg.mod_rate_hz);
                    std::fflush(stdout);
                    break;
                }
                case ',':
                case '.': {
                    if (mod_ptr == nullptr) break;
                    if (cfg.mod_type != "wah") {
                        std::printf("  (il wah e' spento: premi w per attivarlo)\n");
                        std::fflush(stdout);
                        break;
                    }
                    double p = cfg.mod_position + (ch == '.' ? 0.1 : -0.1);
                    p = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                    cfg.mod_position = p;
                    mod_ptr->set_position(static_cast<float>(p));
                    std::printf("  wah: %.1f\n", p);
                    std::fflush(stdout);
                    break;
                }
                case 'i': toggle("ir", i_ir, cfg.ir_on); break;
                case 'r': toggle("reverb", i_reverb, cfg.reverb_on); break;
                case 's': {
                    if (preset_path.empty()) {
                        std::printf("  preset: nessun file da salvare (avviato con i flag)\n");
                    } else {
                        std::string e;
                        if (save_preset(preset_path, cfg, e)) {
                            std::printf("  preset salvato: %s\n", preset_path.c_str());
                        } else {
                            std::printf("  errore: %s\n", e.c_str());
                        }
                    }
                    std::fflush(stdout);
                    break;
                }
                case 'q':
                case '\x03':  // ^C arrivato come byte (stdin non TTY, o ISIG off)
                    g_running.store(false);
                    break;
                default: break;
            }
        }
        if (is_tty) tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    });

    // MIDI: assegnazioni fisse, uguali in ogni preset.
    std::thread midi_thread([&]() {
        if (!midi.is_open() && !ble.is_open()) return;

        std::vector<namzero::MidiEvent> events;
        // I footswitch mandano 127 alla pressione e 0 al rilascio: conta il
        // fronte, non il singolo evento (vedi SwitchDebounce).
        namzero::SwitchDebounce switches;
        switches.set_mode(midi_cfg.switches == "edge" ? namzero::SwitchDebounce::Mode::kEdge
                                                        : namzero::SwitchDebounce::Mode::kAny);

        namzero::WahAutoEngage ae;
        ae.configure(static_cast<float>(midi_cfg.wah_threshold), midi_cfg.wah_engage_ms,
                     midi_cfg.wah_release_ms,
                     static_cast<float>(midi_cfg.wah_release_threshold));
        const auto t0 = std::chrono::steady_clock::now();
        ae.reset(0.0);
        float pos = 0.0f;
        // Finche' il pedale non ha parlato non si conosce la sua posizione:
        // meglio restare spenti che accendersi su un valore inventato.
        bool have_expr = false;

        // Proprieta' dello slot mod: il wah accende solo dal pedale, e al
        // rilascio si torna esattamente allo stato di prima (tremolo compreso).
        namzero::ModArbiter arbiter;

        // Applica uno stato allo slot: usata sia per accendere sia per
        // ripristinare, cosi' le due strade non possono divergere.
        auto apply_mod = [&](const namzero::ModArbiter::State& st) {
            cfg.mod_on = st.on;
            cfg.mod_type = namzero::kModTypeNames[static_cast<std::size_t>(st.type)];
            if (mod_ptr != nullptr) mod_ptr->set_type(mod_type_enum(st.type));
            if (i_mod != kNoSlot) chain.set_enabled(i_mod, st.on);
        };

        while (g_running.load()) {
            events.clear();
            if (midi.is_open()) {
                midi.poll(2, events);
            }
            if (ble.is_open()) {
                ble.poll(events);
            }
            if (!midi.is_open() && events.empty()) {
                // Solo BLE: senza il poll del rawmidi non c'e' attesa, quindi
                // si dorme qui per non bruciare la CPU.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            const auto now = std::chrono::steady_clock::now();
            const double now_ms =
                std::chrono::duration<double, std::milli>(now - t0).count();

            for (const auto& e : events) {
                const uint8_t type = e.status & 0xF0;
                const bool is_cc = (type == 0xB0);
                const bool is_expr = is_cc && (e.cc == midi_cfg.expression_cc);
                if (is_expr) {
                    const float lo = static_cast<float>(midi_cfg.expression_min_raw);
                    const float hi = static_cast<float>(midi_cfg.expression_max_raw);
                    const float span = (hi > lo) ? (hi - lo) : 1.0f;
                    float p = (static_cast<float>(e.value) - lo) / span;
                    p = p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
                    pos = midi_cfg.expression_invert ? 1.0f - p : p;
                    have_expr = true;
                    if (mod_ptr != nullptr) mod_ptr->set_position(pos);
                }

                if (cfg.midi_log) {
                    const unsigned ch = static_cast<unsigned>(e.status & 0x0F) + 1;
                    if (is_expr) {
                        const float act = namzero::wah_activation(pos, midi_cfg.wah_invert);
                        std::printf(
                            "midi %7.1f ms  %-12s ch%u  %3u %3u  pos %.2f  gate %.2f%s%s\n",
                            now_ms, namzero::midi_kind_name(e.status), ch, e.cc, e.value, pos,
                            act, midi_cfg.expression_invert ? " (corsa invertita)" : "",
                            midi_cfg.wah_invert ? " (punta = spento)" : "");
                    } else {
                        std::printf("midi %7.1f ms  %-12s ch%u  %3u %3u\n", now_ms,
                                    namzero::midi_kind_name(e.status), ch, e.cc, e.value);
                    }
                    std::fflush(stdout);
                }

                if (!is_cc || is_expr) continue;

                // Solo il fronte di salita e' una pressione: il rilascio non
                // deve contare, altrimenti tenere premuto spegne subito il blocco.
                if (!switches.pressed(e.cc, e.value, now_ms)) continue;

                const int sel = cc_mod_select[e.cc];
                if (sel >= 0) {
                    // Un tasto = un effetto: seleziona il tipo e accende lo slot;
                    // ripremere lo stesso tasto lo spegne.
                    if (arbiter.engaged()) continue;  // il pedale tiene il wah
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    const bool same = (cfg.mod_type == namzero::kModTypeNames[sel] && cfg.mod_on);
                    cfg.mod_type = namzero::kModTypeNames[sel];
                    cfg.mod_rate_hz = kModDefaultRate[sel];
                    cfg.mod_feedback = kModDefaultFeedback[sel];
                    mod_ptr->set_type(mod_type_enum(sel));
                    mod_ptr->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
                    mod_ptr->set_feedback(static_cast<float>(cfg.mod_feedback));
                    cfg.mod_on = !same;
                    if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                    std::printf("  mod: %s %s (CC%d)\n", cfg.mod_type.c_str(),
                                cfg.mod_on ? "on" : "off", e.cc);
                    std::fflush(stdout);
                    continue;
                }

                const int action = cc_action[e.cc];
                if (action < 0) continue;  // CC non mappato in midi.json

                const auto act = static_cast<namzero::MidiAction>(action);
                const char* name = namzero::midi_action_name(act);

                if (act == namzero::MidiAction::kMod) {
                    // Il mod condivide lo slot col wah: se il pedale lo tiene,
                    // non si tocca (comanda il pedale).
                    if (arbiter.engaged()) continue;
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    // Se lo slot e' sul wah questo comando lo riporta al tremolo;
                    // per gli altri effetti si cambia solo on/off, senza
                    // sovrascrivere il tipo scelto dall'utente.
                    if (cfg.mod_type == "wah") {
                        cfg.mod_type = "tremolo";
                        if (mod_ptr != nullptr) {
                            mod_ptr->set_type(namzero::ModSlot::Type::kTremolo);
                        }
                    }
                    cfg.mod_on = !cfg.mod_on;
                    if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                    std::printf("  mod: %s (%s, CC%d)\n", cfg.mod_on ? "on" : "off",
                                cfg.mod_type.c_str(), e.cc);
                    std::fflush(stdout);
                    continue;
                }

                // amp e ir possono non esistere nel preset corrente.
                if (act == namzero::MidiAction::kAmp && !has_amp) {
                    std::printf("  amp: non presente in questo preset (CC%d)\n", e.cc);
                    std::fflush(stdout);
                    continue;
                }
                const std::size_t idx = slot_index[action];
                if (idx == kNoSlot) {
                    std::printf("  %s: non presente in questo preset (CC%d)\n", name, e.cc);
                    std::fflush(stdout);
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    bool& flag = *slot_flag[action];
                    flag = !flag;
                    chain.set_enabled(idx, flag);
                    if (act == namzero::MidiAction::kDrive) {
                        std::printf("  drive: %s (CC%d, %s)\n", flag ? "on" : "off", e.cc,
                                    cfg.drive_type.c_str());
                    } else {
                        std::printf("  %s: %s (CC%d)\n", name, flag ? "on" : "off", e.cc);
                    }
                    std::fflush(stdout);
                }
            }

            const float activation =
                have_expr ? namzero::wah_activation(pos, midi_cfg.wah_invert) : 0.0f;
            const int transition = ae.update(activation, now_ms);

            if (transition == 1) {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                arbiter.engage({cfg.mod_on, std::max(0, namzero::mod_type_index(cfg.mod_type))});
                apply_mod({true, 1});  // 1 = wah
                wah_engaged.store(true);
                std::printf("  wah: acceso dal pedale\n");
                std::fflush(stdout);
            } else if (transition == -1) {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                apply_mod(arbiter.release());
                wah_engaged.store(false);
                std::printf("  wah: spento dal pedale\n");
                std::fflush(stdout);
            }
        }
    });

    const float master = static_cast<float>(std::pow(10.0, cfg.gain_db / 20.0));

    while (g_running.load()) {
        const long n = io.read_i32(in_i32.data(), frames);
        if (n < 0) {
            if (io.recover(static_cast<int>(n), /*capture_side=*/true)) {
                stats.xruns.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::fprintf(stderr, "read: %s\n", snd_strerror(static_cast<int>(n)));
            break;
        }
        if (static_cast<std::size_t>(n) != frames) continue;

        for (std::size_t i = 0; i < samples; ++i) buf[i] = to_float(in_i32[i]);

        timespec t0{};
        timespec t1{};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        chain.process(buf.data(), frames);
        for (std::size_t i = 0; i < samples; ++i) {
            buf[i] = soft_clip(buf[i] * master);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        store_max(stats.max_us, elapsed_us(t0, t1));

        for (std::size_t i = 0; i < samples; ++i) out_i32[i] = to_i32(buf[i]);

        const long w = io.write_i32(out_i32.data(), frames);
        if (w < 0) {
            if (io.recover(static_cast<int>(w), /*capture_side=*/false)) {
                stats.xruns.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::fprintf(stderr, "write: %s\n", snd_strerror(static_cast<int>(w)));
            break;
        }

        stats.blocks.fetch_add(1, std::memory_order_relaxed);
    }

    g_running.store(false);
    stats_stop.store(true);
    if (stats_thread.joinable()) stats_thread.join();
    if (kb_thread.joinable()) kb_thread.join();
    if (midi_thread.joinable()) midi_thread.join();
    midi.close();

    io.close();

    std::printf("\nriepilogo: %" PRIu64 " blocchi, %" PRIu64 " xrun\n",
                stats.blocks.load(), stats.xruns.load());
    return 0;
}
