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
#include "dsp/looper_slot.h"
#include "dsp/reverb_slot.h"
#include "control/midi.h"
#include "control/midi_config.h"
#include "control/delay_modes.h"
#include "control/drive_types.h"
#include "control/mod_types.h"
#include "control/presets_config.h"
#include "control/control_socket.h"
#include "control/midi_output.h"
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



namzero::ModSlot::Type mod_type_enum(int idx) {
    switch (idx) {
        case 1: return namzero::ModSlot::Type::kWah;
        case 2: return namzero::ModSlot::Type::kUnivibe;
        case 3: return namzero::ModSlot::Type::kFlanger;
        case 4: return namzero::ModSlot::Type::kPhaser;
        default: return namzero::ModSlot::Type::kTremolo;
    }
}


namzero::DelaySlot::Mode delay_mode_enum(int idx) {
    switch (idx) {
        case 1: return namzero::DelaySlot::Mode::kPingPong;
        case 2: return namzero::DelaySlot::Mode::kPingPongStereo;
        default: return namzero::DelaySlot::Mode::kStereo;
    }
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

// Riscrive il preset aggiornando gli stati on/off e il volume dell'ampli (trim),
// preservando nome e campi che non conosciamo: un salvataggio non deve
// distruggere il file.
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

    // Salvataggio completo: tutto lo stato live, non solo gli on/off. La UI
    // modifica i parametri dal vivo (socket di controllo) e poi salva qui.
    auto& slots = j["slots"];
    slots["gate"]["on"] = cfg.gate_on;
    slots["comp"]["on"] = cfg.comp_on;
    slots["comp"]["threshold_db"] = cfg.comp_threshold_db;
    slots["comp"]["ratio"] = cfg.comp_ratio;
    slots["comp"]["attack_ms"] = cfg.comp_attack_ms;
    slots["comp"]["release_ms"] = cfg.comp_release_ms;
    slots["comp"]["makeup_db"] = cfg.comp_makeup_db;
    slots["drive"]["on"] = cfg.drive_on;
    slots["drive"]["type"] = cfg.drive_type;
    slots["drive"]["blend"] = cfg.drive_blend;
    slots["drive"]["amount"] = cfg.drive_amount;
    slots["drive"]["tone"] = cfg.drive_tone;
    slots["drive"]["level_db"] = cfg.drive_level_db;
    slots["drive"]["mid_db"] = cfg.drive_mid_db;
    slots["drive"]["low_db"] = cfg.drive_low_db;
    slots["mod"]["on"] = cfg.mod_on;
    slots["mod"]["type"] = cfg.mod_type;
    slots["mod"]["rate_hz"] = cfg.mod_rate_hz;
    slots["mod"]["depth"] = cfg.mod_depth;
    slots["mod"]["position"] = cfg.mod_position;
    slots["mod"]["q"] = cfg.mod_q;
    slots["mod"]["mix"] = cfg.mod_mix;
    slots["mod"]["low_hz"] = cfg.mod_low_hz;
    slots["mod"]["high_hz"] = cfg.mod_high_hz;
    slots["mod"]["fx_mix"] = cfg.mod_fx_mix;
    slots["mod"]["feedback"] = cfg.mod_feedback;
    slots["amp"]["on"] = cfg.amp_on;
    slots["amp"]["file"] = cfg.amp_file;
    slots["amp"]["slim"] = cfg.amp_slim;
    slots["amp"]["trim_db"] = cfg.amp_trim_db;
    slots["ir"]["on"] = cfg.ir_on;
    slots["ir"]["file"] = cfg.ir_file;
    slots["delay"]["on"] = cfg.delay_on;
    slots["delay"]["time_ms"] = cfg.delay_time_ms;
    slots["delay"]["feedback"] = cfg.delay_feedback;
    slots["delay"]["mix"] = cfg.delay_mix;
    slots["delay"]["tone"] = cfg.delay_tone;
    slots["delay"]["mode"] = cfg.delay_mode;
    slots["reverb"]["on"] = cfg.reverb_on;
    slots["reverb"]["mix"] = cfg.reverb_mix;
    slots["reverb"]["size"] = cfg.reverb_size;
    slots["reverb"]["damp"] = cfg.reverb_damp;

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
    bool from_slot = false;
    // Slot in uso: serve alla navigazione (prec/succ) e viene tramandato al
    // processo ri-eseguito con NAMZERO_SLOT.
    std::atomic<int> current_slot{0};

    if (!parse_args(argc, argv, cfg, midi_cli, preset_path, saw_flag, err)) {
        std::fprintf(stderr, "errore: %s\n\n", err.c_str());
        usage();
        return 2;
    }
    if (cfg.help) {
        usage();
        return 0;
    }

    // --- presets.json: gate globale e slot 0-9 -----------------------------
    namzero::PresetsConfig presets_cfg;
    bool have_presets = false;
    {
        std::ifstream in("config/presets.json");
        if (in) {
            if (!namzero::load_presets_config("config/presets.json", presets_cfg, err)) {
                std::fprintf(stderr, "errore: %s\n", err.c_str());
                return 2;
            }
            have_presets = true;
        }
    }
    auto slot_file = [&](int slot) -> std::string {
        if (!have_presets || slot < 0 || slot >= 10) return {};
        const std::string& f = presets_cfg.slots[static_cast<std::size_t>(slot)];
        if (f.empty()) return {};
        return f[0] == '/' ? f : "config/" + f;
    };
    auto read_last_slot = [&]() -> int {
        std::ifstream in("config/.last_slot");
        int slot = 0;
        if (in) in >> slot;
        if (!have_presets || slot < 0 || slot >= 10) return 0;
        return slot;
    };
    auto write_last_slot = [](int slot) {
        std::ofstream out("config/.last_slot");
        if (out) out << slot << "\n";
    };


    {
        const char* env_slot = std::getenv("NAMZERO_SLOT");
        if (env_slot != nullptr) current_slot.store(std::atoi(env_slot));
    }
    if (preset_path.empty() && have_presets) {
        const int slot = presets_cfg.start_slot >= 0 ? presets_cfg.start_slot : read_last_slot();
        const std::string file = slot_file(slot);
        if (!file.empty()) {
            preset_path = file;
            current_slot.store(slot);
            from_slot = true;
        }
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
        if (have_presets) {
            cfg.gate_threshold_db = presets_cfg.gate.threshold_db;
            cfg.gate_attack_ms = presets_cfg.gate.attack_ms;
            cfg.gate_release_ms = presets_cfg.gate.release_ms;
        }
        // Banner del preset: numero (se da slot), file e modello NAM.
        std::string base = preset_path;
        const std::size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        std::string amp_name = cfg.amp_file;
        const std::size_t aslash = amp_name.find_last_of('/');
        if (aslash != std::string::npos) amp_name = amp_name.substr(aslash + 1);
        if (amp_name.empty()) amp_name = "nessun NAM";
        if (from_slot) {
            std::printf("\n[%d] - %s (%s)\n\n", current_slot.load(), base.c_str(),
                        amp_name.c_str());
        } else {
            std::printf("\n[-] - %s (%s)\n\n", base.c_str(), amp_name.c_str());
        }
    }

    // Un tipo scritto male non deve diventare un modello a caso: si torna al
    // default e lo si dice, invece di far suonare qualcosa di inatteso.
    if (namzero::drive_type_index(cfg.drive_type) < 0) {
        std::fprintf(stderr, "drive: tipo \"%s\" sconosciuto, uso fuzz\n",
                     cfg.drive_type.c_str());
        cfg.drive_type = "fuzz";
    }
    if (namzero::delay_mode_index(cfg.delay_mode) < 0) {
        std::fprintf(stderr, "delay: modo \"%s\" sconosciuto, uso stereo\n",
                     cfg.delay_mode.c_str());
        cfg.delay_mode = "stereo";
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
    if (midi_cli.device_set) {
        midi_cfg.device = midi_cli.device;
        midi_cfg.devices = {midi_cli.device};
    }
    if (midi_cli.no_midi) midi_cfg.enabled = false;
    if (midi_cli.expr_invert) midi_cfg.expression_invert = true;
    if (midi_cli.wah_invert) midi_cfg.wah_invert = true;

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Boost di fine catena: il gain master vive in un atomico perche' il thread
    // MIDI lo cambia e il thread audio lo legge a ogni blocco.
    const float base_master = static_cast<float>(std::pow(10.0, cfg.gain_db / 20.0));
    std::atomic<float> master_gain{base_master};
    bool boost_on = false;
    // Slot richiesto da tasto o Program Change: il loop audio esce e main
    // ri-esegue il processo con il nuovo preset (buco ~300 ms).
    std::atomic<int> switch_slot{-1};

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
        dr->set_type(drive_type_enum(namzero::drive_type_index(cfg.drive_type)));
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

    // Looper a fine catena: buffer preallocato all'avvio (60 s stereo).
    namzero::LooperSlot* looper_ptr = nullptr;
    {
        auto lp = std::make_unique<namzero::LooperSlot>(60.0);
        looper_ptr = lp.get();
        chain.add(std::move(lp));
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
    const int lock_fd = ::open("/tmp/namzero-lite.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0666);
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
    std::vector<std::string> midi_dev_names;
    std::vector<std::string> midi_open_names;
    std::vector<std::unique_ptr<namzero::MidiInput>> midis;
    if (midi_cfg.enabled) {
        std::vector<std::string> devs = midi_cfg.devices;
        if (devs.empty()) devs.push_back(midi_cfg.device);
        midi_dev_names = devs;
        for (const auto& dev : devs) {
            auto in = std::make_unique<namzero::MidiInput>();
            std::string merr;
            if (in->open(dev, merr)) {
                std::printf("midi: %s\n", dev.c_str());
                midis.push_back(std::move(in));
                midi_open_names.push_back(dev);
            } else {
                std::fprintf(stderr, "midi: %s: %s (salto)\n", dev.c_str(), merr.c_str());
            }
        }
        if (!midis.empty()) {
            std::printf("midi: espressione CC (multipli) | switch %s\n",
                        midi_cfg.switches.c_str());
            for (std::size_t i = 0; i < namzero::kMidiActionCount; ++i) {
                const int cc = midi_cfg.slot_cc[i];
                if (cc >= 0) {
                    std::printf("  %-6s CC%d\n",
                                namzero::midi_action_name(static_cast<namzero::MidiAction>(i)),
                                cc);
                }
            }
            auto print_select = [](const char* label, const int* ccs, std::size_t count) {
                bool any = false;
                std::string line;
                for (std::size_t i = 0; i < count; ++i) {
                    if (ccs[i] < 0) continue;
                    if (any) line += "/";
                    line += std::to_string(ccs[i]);
                    any = true;
                }
                if (any) std::printf("  %-6s CC%s\n", label, line.c_str());
            };
            print_select("mod", midi_cfg.mod_select_cc.data(), namzero::kModTypeCount);
            print_select("drive", midi_cfg.drive_select_cc.data(), namzero::kDriveTypeCount);
            print_select("delay", midi_cfg.delay_select_cc.data(), namzero::kDelayModeCount);
            if (midi_cfg.boost_cc >= 0) {
                std::printf("  %-6s CC%d (+%.1f dB)\n", "boost", midi_cfg.boost_cc,
                            midi_cfg.boost_db);
            }
        }
    }
    // LED del G-Board: Note On ch1, una nota per LED (0-7), 127 acceso.
    namzero::MidiOutput led_out;
    if (!midi_cfg.gboard_device.empty()) {
        std::string lerr;
        if (led_out.open(midi_cfg.gboard_device, lerr)) {
            std::printf("led: %s (Note On ch1, note 0-7)\n", midi_cfg.gboard_device.c_str());
        } else {
            std::fprintf(stderr, "led: %s (si prosegue senza)\n", lerr.c_str());
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

    namzero::ControlSocket ctl;
    {
        std::string cerr_;
        if (ctl.listen("/tmp/namzero-lite-ctl.sock", cerr_)) {
            std::printf("ctl: %s (JSON per la UI)\n", "/tmp/namzero-lite-ctl.sock");
        } else {
            std::fprintf(stderr, "ctl: %s (si prosegue senza)\n", cerr_.c_str());
        }
    }

    const bool wah_armed = !midis.empty();

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
    std::array<int8_t, 128> cc_drive_select{};
    cc_drive_select.fill(-1);
    for (std::size_t i = 0; i < namzero::kDriveTypeCount; ++i) {
        const int cc = midi_cfg.drive_select_cc[i];
        if (cc >= 0) cc_drive_select[static_cast<std::size_t>(cc)] = static_cast<int8_t>(i);
    }

    std::array<int8_t, 128> cc_delay_select{};
    cc_delay_select.fill(-1);
    for (std::size_t i = 0; i < namzero::kDelayModeCount; ++i) {
        const int cc = midi_cfg.delay_select_cc[i];
        if (cc >= 0) cc_delay_select[static_cast<std::size_t>(cc)] = static_cast<int8_t>(i);
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
    // Stato dei LED: riga 1 = mod (note 0-3), riga 2 = drive (note 4-6),
    // nota 7 libera. Il LED acceso e' quello dell'effetto selezionato.
    auto apply_leds = [&]() {
        if (!led_out.is_open()) return;
        static const int mod_led_type[4] = {0, 2, 3, 4};  // tremolo/univibe/flanger/phaser
        const int mt = namzero::mod_type_index(cfg.mod_type);
        for (int i = 0; i < 4; ++i) {
            led_out.send_note(0, static_cast<uint8_t>(4 + i),
                              (cfg.mod_on && mt == mod_led_type[i]) ? 127 : 0);
        }
        const int dt = namzero::drive_type_index(cfg.drive_type);
        for (int i = 0; i < 3; ++i) {
            led_out.send_note(0, static_cast<uint8_t>(i),
                              (cfg.drive_on && dt == i) ? 127 : 0);
        }
        led_out.send_note(0, 3, boost_on ? 127 : 0);
    };
    apply_leds();

    // Snapshot dello stato live per la UI: si manda intero a ogni cambiamento.
    auto build_state = [&]() {
        nlohmann::json s;
        s["slot"] = current_slot.load();
        s["preset"] = preset_path;
        s["boost"] = boost_on;
        s["boost_db"] = midi_cfg.boost_db;
        if (looper_ptr != nullptr) {
            s["looper"]["state"] = namzero::LooperSlot::state_name(looper_ptr->state());
            s["looper"]["pos"] = looper_ptr->position_seconds();
            s["looper"]["len"] = looper_ptr->length_seconds();
            s["looper"]["max"] = looper_ptr->max_seconds();
        }
        auto& sl = s["slots"];
        sl["gate"]["on"] = cfg.gate_on;
        sl["gate"]["threshold"] = cfg.gate_threshold_db;
        sl["gate"]["attack"] = cfg.gate_attack_ms;
        sl["gate"]["release"] = cfg.gate_release_ms;
        sl["comp"]["on"] = cfg.comp_on;
        sl["comp"]["threshold"] = cfg.comp_threshold_db;
        sl["comp"]["ratio"] = cfg.comp_ratio;
        sl["comp"]["makeup"] = cfg.comp_makeup_db;
        sl["comp"]["attack"] = cfg.comp_attack_ms;
        sl["comp"]["release"] = cfg.comp_release_ms;
        sl["drive"]["on"] = cfg.drive_on;
        sl["drive"]["type"] = cfg.drive_type;
        sl["drive"]["amount"] = cfg.drive_amount;
        sl["drive"]["tone"] = cfg.drive_tone;
        sl["drive"]["blend"] = cfg.drive_blend;
        sl["drive"]["level"] = cfg.drive_level_db;
        sl["drive"]["mid"] = cfg.drive_mid_db;
        sl["drive"]["low"] = cfg.drive_low_db;
        sl["mod"]["on"] = cfg.mod_on;
        sl["mod"]["type"] = cfg.mod_type;
        sl["mod"]["rate"] = cfg.mod_rate_hz;
        sl["mod"]["depth"] = cfg.mod_depth;
        sl["mod"]["feedback"] = cfg.mod_feedback;
        sl["mod"]["fx_mix"] = cfg.mod_fx_mix;
        sl["mod"]["q"] = cfg.mod_q;
        sl["mod"]["mix"] = cfg.mod_mix;
        sl["mod"]["low_hz"] = cfg.mod_low_hz;
        sl["mod"]["high_hz"] = cfg.mod_high_hz;
        sl["mod"]["position"] = cfg.mod_position;
        sl["amp"]["on"] = cfg.amp_on;
        sl["amp"]["file"] = cfg.amp_file;
        sl["amp"]["trim"] = cfg.amp_trim_db;
        sl["ir"]["on"] = cfg.ir_on;
        sl["ir"]["file"] = cfg.ir_file;
        sl["delay"]["on"] = cfg.delay_on;
        sl["delay"]["mode"] = cfg.delay_mode;
        sl["delay"]["time"] = cfg.delay_time_ms;
        sl["delay"]["feedback"] = cfg.delay_feedback;
        sl["delay"]["mix"] = cfg.delay_mix;
        sl["delay"]["tone"] = cfg.delay_tone;
        sl["reverb"]["on"] = cfg.reverb_on;
        sl["reverb"]["mix"] = cfg.reverb_mix;
        sl["reverb"]["size"] = cfg.reverb_size;
        sl["reverb"]["damp"] = cfg.reverb_damp;
        return s.dump();
    };

    auto handle_ctl = [&](const std::string& line) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(line);
        } catch (...) {
            return;
        }
        const std::string cmd = j.value("cmd", "");
        if (cmd == "state") {
            ctl.send(build_state());
        } else if (cmd == "slot") {
            const int n = j.value("n", -1);
            if (n >= 0 && n < 10) {
                switch_slot.store(n);
                current_slot.store(n);
            }
        } else if (cmd == "reload") {
            switch_slot.store(current_slot.load());
        } else if (cmd == "save") {
            std::string e;
            if (save_preset(preset_path, cfg, e)) {
                std::printf("  preset salvato: %s\\n", preset_path.c_str());
            } else {
                std::printf("  save: %s\\n", e.c_str());
            }
            std::fflush(stdout);
        } else if (cmd == "file") {
            const std::string fslot = j.value("slot", "");
            std::string file = j.value("file", "");
            if (file.empty()) return;
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            std::string e;
            if (fslot == "amp") {
                if (file.find('/') == std::string::npos) file = "models/" + file;
                namzero::NamSlot probe;  // valida PRIMA di salvare
                if (!probe.load(file, cfg.amp_slim, e)) {
                    std::printf("  file amp: %s\n", e.c_str());
                    std::fflush(stdout);
                    return;
                }
                cfg.amp_file = file;
            } else if (fslot == "ir") {
                if (file.find('/') == std::string::npos) file = "irs/" + file;
                namzero::IrSlot probe;
                if (!probe.load(file, cfg.ir_taps, e)) {
                    std::printf("  file ir: %s\n", e.c_str());
                    std::fflush(stdout);
                    return;
                }
                cfg.ir_file = file;
            } else {
                return;
            }
            save_preset(preset_path, cfg, e);  // persiste la scelta solo se valida
            switch_slot.store(current_slot.load());  // ricarica con il nuovo file
        } else if (cmd == "defaults") {
            const std::string slot = j.value("slot", "");
            Config d;  // default compilati
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            if (slot == "gate") {
                cfg.gate_on = d.gate_on;
                cfg.gate_threshold_db = d.gate_threshold_db;
                cfg.gate_attack_ms = d.gate_attack_ms;
                cfg.gate_release_ms = d.gate_release_ms;
                gate_ptr->set_threshold_db(static_cast<float>(cfg.gate_threshold_db));
                gate_ptr->set_attack_ms(static_cast<float>(cfg.gate_attack_ms));
                gate_ptr->set_release_ms(static_cast<float>(cfg.gate_release_ms));
                if (i_gate != kNoSlot) chain.set_enabled(i_gate, cfg.gate_on);
            } else if (slot == "comp") {
                cfg.comp_on = d.comp_on;
                cfg.comp_threshold_db = d.comp_threshold_db;
                cfg.comp_ratio = d.comp_ratio;
                cfg.comp_attack_ms = d.comp_attack_ms;
                cfg.comp_release_ms = d.comp_release_ms;
                cfg.comp_makeup_db = d.comp_makeup_db;
                comp_ptr->set_threshold_db(static_cast<float>(cfg.comp_threshold_db));
                comp_ptr->set_ratio(static_cast<float>(cfg.comp_ratio));
                comp_ptr->set_attack_ms(static_cast<float>(cfg.comp_attack_ms));
                comp_ptr->set_release_ms(static_cast<float>(cfg.comp_release_ms));
                comp_ptr->set_makeup_db(static_cast<float>(cfg.comp_makeup_db));
                if (i_comp != kNoSlot) chain.set_enabled(i_comp, cfg.comp_on);
            } else if (slot == "drive") {
                cfg.drive_on = d.drive_on;
                cfg.drive_type = d.drive_type;
                cfg.drive_amount = d.drive_amount;
                cfg.drive_tone = d.drive_tone;
                cfg.drive_blend = d.drive_blend;
                cfg.drive_level_db = d.drive_level_db;
                cfg.drive_mid_db = d.drive_mid_db;
                cfg.drive_low_db = d.drive_low_db;
                drive_ptr->set_type(drive_type_enum(namzero::drive_type_index(cfg.drive_type)));
                drive_ptr->set_drive(static_cast<float>(cfg.drive_amount));
                drive_ptr->set_tone(static_cast<float>(cfg.drive_tone));
                drive_ptr->set_blend(static_cast<float>(cfg.drive_blend));
                drive_ptr->set_level_db(static_cast<float>(cfg.drive_level_db));
                drive_ptr->set_mid_db(static_cast<float>(cfg.drive_mid_db));
                drive_ptr->set_low_db(static_cast<float>(cfg.drive_low_db));
                if (i_drive != kNoSlot) chain.set_enabled(i_drive, cfg.drive_on);
                apply_leds();
            } else if (slot == "mod") {
                cfg.mod_on = d.mod_on;
                cfg.mod_type = d.mod_type;
                cfg.mod_rate_hz = d.mod_rate_hz;
                cfg.mod_depth = d.mod_depth;
                cfg.mod_feedback = d.mod_feedback;
                cfg.mod_fx_mix = d.mod_fx_mix;
                cfg.mod_q = d.mod_q;
                cfg.mod_mix = d.mod_mix;
                cfg.mod_low_hz = d.mod_low_hz;
                cfg.mod_high_hz = d.mod_high_hz;
                cfg.mod_position = d.mod_position;
                mod_ptr->set_type(mod_type_enum(namzero::mod_type_index(cfg.mod_type)));
                mod_ptr->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
                mod_ptr->set_depth(static_cast<float>(cfg.mod_depth));
                mod_ptr->set_feedback(static_cast<float>(cfg.mod_feedback));
                mod_ptr->set_fx_mix(static_cast<float>(cfg.mod_fx_mix));
                mod_ptr->set_wah_q(static_cast<float>(cfg.mod_q));
                mod_ptr->set_wah_mix(static_cast<float>(cfg.mod_mix));
                mod_ptr->set_wah_low_hz(static_cast<float>(cfg.mod_low_hz));
                mod_ptr->set_wah_high_hz(static_cast<float>(cfg.mod_high_hz));
                mod_ptr->set_position(static_cast<float>(cfg.mod_position));
                if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                apply_leds();
            } else if (slot == "amp") {
                cfg.amp_on = d.amp_on;
                cfg.amp_trim_db = d.amp_trim_db;
                if (amp_ptr != nullptr) amp_ptr->set_trim_db(cfg.amp_trim_db);
                if (i_amp != kNoSlot) chain.set_enabled(i_amp, cfg.amp_on);
            } else if (slot == "ir") {
                cfg.ir_on = d.ir_on;
                if (i_ir != kNoSlot) chain.set_enabled(i_ir, cfg.ir_on);
            } else if (slot == "delay") {
                cfg.delay_on = d.delay_on;
                cfg.delay_time_ms = d.delay_time_ms;
                cfg.delay_feedback = d.delay_feedback;
                cfg.delay_mix = d.delay_mix;
                cfg.delay_tone = d.delay_tone;
                cfg.delay_mode = d.delay_mode;
                delay_slot->set_time_ms(static_cast<float>(cfg.delay_time_ms));
                delay_slot->set_feedback(static_cast<float>(cfg.delay_feedback));
                delay_slot->set_mix(static_cast<float>(cfg.delay_mix));
                delay_slot->set_tone(static_cast<float>(cfg.delay_tone));
                delay_slot->set_mode(delay_mode_enum(namzero::delay_mode_index(cfg.delay_mode)));
                if (i_delay != kNoSlot) chain.set_enabled(i_delay, cfg.delay_on);
            } else if (slot == "reverb") {
                cfg.reverb_on = d.reverb_on;
                cfg.reverb_mix = d.reverb_mix;
                cfg.reverb_size = d.reverb_size;
                cfg.reverb_damp = d.reverb_damp;
                reverb_ptr->set_wet(static_cast<float>(cfg.reverb_mix));
                reverb_ptr->set_room_size(static_cast<float>(cfg.reverb_size));
                reverb_ptr->set_damp(static_cast<float>(cfg.reverb_damp));
                if (i_reverb != kNoSlot) chain.set_enabled(i_reverb, cfg.reverb_on);
            } else if (slot == "boost") {
                namzero::MidiConfig dm;
                midi_cfg.boost_db = dm.boost_db;
                if (boost_on) {
                    master_gain.store(
                        base_master * static_cast<float>(std::pow(10.0, midi_cfg.boost_db / 20.0)),
                        std::memory_order_relaxed);
                }
            }
        } else if (cmd == "looper") {
            const std::string action = j.value("action", "");
            if (looper_ptr != nullptr) {
                if (action == "toggle") looper_ptr->command(namzero::LooperSlot::Command::kToggle);
                else if (action == "stop") looper_ptr->command(namzero::LooperSlot::Command::kStop);
                else if (action == "clear") looper_ptr->command(namzero::LooperSlot::Command::kClear);
            }
        } else if (cmd == "boost") {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            boost_on = j.value("on", boost_on);
            const float b = boost_on
                                ? static_cast<float>(std::pow(10.0, midi_cfg.boost_db / 20.0))
                                : 1.0f;
            master_gain.store(base_master * b, std::memory_order_relaxed);
            apply_leds();
        } else if (cmd == "set") {
            const std::string slot = j.value("slot", "");
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            if (slot == "mod") {
                if (j.contains("type")) {
                    cfg.mod_type = j["type"].get<std::string>();
                    mod_ptr->set_type(mod_type_enum(namzero::mod_type_index(cfg.mod_type)));
                }
                cfg.mod_on = j.value("on", cfg.mod_on);
                if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                apply_leds();
            } else if (slot == "drive") {
                if (j.contains("type")) {
                    cfg.drive_type = j["type"].get<std::string>();
                    drive_ptr->set_type(drive_type_enum(namzero::drive_type_index(cfg.drive_type)));
                }
                cfg.drive_on = j.value("on", cfg.drive_on);
                if (i_drive != kNoSlot) chain.set_enabled(i_drive, cfg.drive_on);
                apply_leds();
            } else if (slot == "gate") {
                cfg.gate_on = j.value("on", cfg.gate_on);
                if (i_gate != kNoSlot) chain.set_enabled(i_gate, cfg.gate_on);
            } else if (slot == "comp") {
                cfg.comp_on = j.value("on", cfg.comp_on);
                if (i_comp != kNoSlot) chain.set_enabled(i_comp, cfg.comp_on);
            } else if (slot == "amp") {
                cfg.amp_on = j.value("on", cfg.amp_on);
                if (i_amp != kNoSlot) chain.set_enabled(i_amp, cfg.amp_on);
            } else if (slot == "ir") {
                cfg.ir_on = j.value("on", cfg.ir_on);
                if (i_ir != kNoSlot) chain.set_enabled(i_ir, cfg.ir_on);
            } else if (slot == "reverb") {
                cfg.reverb_on = j.value("on", cfg.reverb_on);
                if (i_reverb != kNoSlot) chain.set_enabled(i_reverb, cfg.reverb_on);
            } else if (slot == "delay") {
                if (j.contains("mode")) {
                    cfg.delay_mode = j["mode"].get<std::string>();
                    delay_slot->set_mode(delay_mode_enum(namzero::delay_mode_index(cfg.delay_mode)));
                }
                cfg.delay_on = j.value("on", cfg.delay_on);
                if (i_delay != kNoSlot) chain.set_enabled(i_delay, cfg.delay_on);
            }
        } else if (cmd == "param") {
            const std::string slot = j.value("slot", "");
            const std::string key = j.value("key", "");
            const double v = j.value("value", 0.0);
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            if (slot == "gate") {
                if (key == "threshold") { cfg.gate_threshold_db = v; gate_ptr->set_threshold_db(static_cast<float>(v)); }
                else if (key == "attack") { cfg.gate_attack_ms = v; gate_ptr->set_attack_ms(static_cast<float>(v)); }
                else if (key == "release") { cfg.gate_release_ms = v; gate_ptr->set_release_ms(static_cast<float>(v)); }
            } else if (slot == "comp") {
                if (key == "threshold") { cfg.comp_threshold_db = v; comp_ptr->set_threshold_db(static_cast<float>(v)); }
                else if (key == "ratio") { cfg.comp_ratio = v; comp_ptr->set_ratio(static_cast<float>(v)); }
                else if (key == "makeup") { cfg.comp_makeup_db = v; comp_ptr->set_makeup_db(static_cast<float>(v)); }
                else if (key == "attack") { cfg.comp_attack_ms = v; comp_ptr->set_attack_ms(static_cast<float>(v)); }
                else if (key == "release") { cfg.comp_release_ms = v; comp_ptr->set_release_ms(static_cast<float>(v)); }
            } else if (slot == "drive") {
                if (key == "amount") { cfg.drive_amount = v; drive_ptr->set_drive(static_cast<float>(v)); }
                else if (key == "tone") { cfg.drive_tone = v; drive_ptr->set_tone(static_cast<float>(v)); }
                else if (key == "blend") { cfg.drive_blend = v; drive_ptr->set_blend(static_cast<float>(v)); }
                else if (key == "level") { cfg.drive_level_db = v; drive_ptr->set_level_db(static_cast<float>(v)); }
                else if (key == "mid") { cfg.drive_mid_db = v; drive_ptr->set_mid_db(static_cast<float>(v)); }
                else if (key == "low") { cfg.drive_low_db = v; drive_ptr->set_low_db(static_cast<float>(v)); }
            } else if (slot == "mod") {
                if (key == "rate") { cfg.mod_rate_hz = v; mod_ptr->set_rate_hz(static_cast<float>(v)); }
                else if (key == "depth") { cfg.mod_depth = v; mod_ptr->set_depth(static_cast<float>(v)); }
                else if (key == "feedback") { cfg.mod_feedback = v; mod_ptr->set_feedback(static_cast<float>(v)); }
                else if (key == "fx_mix") { cfg.mod_fx_mix = v; mod_ptr->set_fx_mix(static_cast<float>(v)); }
                else if (key == "q") { cfg.mod_q = v; mod_ptr->set_wah_q(static_cast<float>(v)); }
                else if (key == "mix") { cfg.mod_mix = v; mod_ptr->set_wah_mix(static_cast<float>(v)); }
                else if (key == "low_hz") { cfg.mod_low_hz = v; mod_ptr->set_wah_low_hz(static_cast<float>(v)); }
                else if (key == "high_hz") { cfg.mod_high_hz = v; mod_ptr->set_wah_high_hz(static_cast<float>(v)); }
                else if (key == "position") { cfg.mod_position = v; mod_ptr->set_position(static_cast<float>(v)); }
            } else if (slot == "amp") {
                if (key == "trim") { cfg.amp_trim_db = v; amp_ptr->set_trim_db(v); }
            } else if (slot == "delay") {
                if (key == "time") { cfg.delay_time_ms = v; delay_slot->set_time_ms(static_cast<float>(v)); }
                else if (key == "feedback") { cfg.delay_feedback = v; delay_slot->set_feedback(static_cast<float>(v)); }
                else if (key == "mix") { cfg.delay_mix = v; delay_slot->set_mix(static_cast<float>(v)); }
                else if (key == "tone") { cfg.delay_tone = v; delay_slot->set_tone(static_cast<float>(v)); }
            } else if (slot == "reverb") {
                if (key == "mix") { cfg.reverb_mix = v; reverb_ptr->set_wet(static_cast<float>(v)); }
                else if (key == "size") { cfg.reverb_size = v; reverb_ptr->set_room_size(static_cast<float>(v)); }
                else if (key == "damp") { cfg.reverb_damp = v; reverb_ptr->set_damp(static_cast<float>(v)); }
            }
        }
    };
    // true mentre il pedale tiene acceso il wah: la tastiera non deve toccare
    // lo slot mod in quel momento, o il ripristino al rilascio sarebbe sbagliato.
    std::atomic<bool> wah_engaged{false};
    std::thread kb_thread([&chain, &cfg, &preset_path, &ctrl_mtx, &wah_engaged, &switch_slot, &current_slot, &apply_leds, looper_ptr, wah_armed,
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
            apply_leds();
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
                case '0': case '1': case '2': case '3': case '4':
                case '5': case '6': case '7': case '8': case '9':
                    switch_slot.store(ch - '0');
                    current_slot.store(ch - '0');
                    std::printf("  preset: slot %c\n", ch);
                    std::fflush(stdout);
                    break;
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
                    int idx = namzero::drive_type_index(cfg.drive_type);
                    idx = (idx + 1) % static_cast<int>(namzero::kDriveTypeCount);
                    cfg.drive_type = namzero::kDriveTypeNames[idx];
                    drive_ptr->set_type(drive_type_enum(idx));
                    apply_leds();
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
                case 'l':
                    if (looper_ptr != nullptr) looper_ptr->command(namzero::LooperSlot::Command::kToggle);
                    break;
                case ';':
                    if (looper_ptr != nullptr) looper_ptr->command(namzero::LooperSlot::Command::kStop);
                    break;
                case 'L':
                    if (looper_ptr != nullptr) looper_ptr->command(namzero::LooperSlot::Command::kClear);
                    break;
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
                    apply_leds();
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

    std::thread ctl_thread([&]() {
        std::string last;
        bool was = false;
        while (g_running.load()) {
            std::vector<std::string> lines;
            ctl.poll(lines);
            for (const auto& line : lines) {
                try {
                    handle_ctl(line);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "ctl: %s\n", e.what());
                }
            }
            std::string snap;
            {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                snap = build_state();
            }
            const bool now = ctl.connected();
            if (now && (!was || snap != last)) {
                ctl.send(snap);
                last = snap;
            }
            was = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    std::thread midi_thread([&]() {
        if (midis.empty() && !ble.is_open()) return;

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
            // Hotplug: un pedale USB collegato dopo l'avvio viene aperto qui.
            static std::int64_t last_retry = 0;
            const auto now_retry = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count();
            if (now_retry - last_retry > 2000) {
                last_retry = now_retry;
                for (const auto& dev : midi_dev_names) {
                    bool already = false;
                    for (const auto& n : midi_open_names) {
                        if (n == dev) already = true;
                    }
                    if (already) continue;
                    auto in = std::make_unique<namzero::MidiInput>();
                    std::string e;
                    if (in->open(dev, e)) {
                        std::printf("midi: %s (collegato a caldo)\n", dev.c_str());
                        std::fflush(stdout);
                        midis.push_back(std::move(in));
                        midi_open_names.push_back(dev);
                    }
                }
                if (!led_out.is_open() && !midi_cfg.gboard_device.empty()) {
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    std::string e;
                    if (led_out.open(midi_cfg.gboard_device, e)) {
                        apply_leds();
                    }
                }
            }

            events.clear();
            if (!midis.empty()) {
                midis[0]->poll(2, events);
                for (std::size_t i = 1; i < midis.size(); ++i) {
                    midis[i]->poll(0, events);
                }
            }
            if (ble.is_open()) {
                ble.poll(events);
            }
            if (midis.empty() && events.empty()) {
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
                bool is_expr = false;
                if (is_cc) {
                    for (int c : midi_cfg.expression_ccs) {
                        if (c == e.cc) is_expr = true;
                    }
                }
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

                if ((e.status & 0xF0) == 0xC0) {
                    // G-Board: i pulsanti mandano Program Change.
                    // Riga 1 (PC 4-7) = mod, riga 2 (PC 0-2) = drive.
                    int mod_led = -1;
                    int drive_led = -1;
                    for (int i = 0; i < 4; ++i) {
                        if (midi_cfg.gboard_mod_pc[i] == e.cc) mod_led = i;
                    }
                    for (int i = 0; i < 3; ++i) {
                        if (midi_cfg.gboard_drive_pc[i] == e.cc) drive_led = i;
                    }
                    static const int mod_led_type[4] = {0, 2, 3, 4};
                    if (mod_led >= 0) {
                        std::lock_guard<std::mutex> lk(ctrl_mtx);
                        const int type_idx = mod_led_type[mod_led];
                        const bool same =
                            (cfg.mod_type == namzero::kModTypeNames[type_idx] && cfg.mod_on);
                        cfg.mod_type = namzero::kModTypeNames[type_idx];
                        cfg.mod_rate_hz = kModDefaultRate[type_idx];
                        cfg.mod_feedback = kModDefaultFeedback[type_idx];
                        mod_ptr->set_type(mod_type_enum(type_idx));
                        mod_ptr->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
                        mod_ptr->set_feedback(static_cast<float>(cfg.mod_feedback));
                        cfg.mod_on = !same;
                        if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                        apply_leds();
                        std::printf("  mod: %s %s (PC%d)\n", cfg.mod_type.c_str(),
                                    cfg.mod_on ? "on" : "off", e.cc);
                        std::fflush(stdout);
                    } else if (midi_cfg.gboard_boost_pc >= 0 &&
                               e.cc == midi_cfg.gboard_boost_pc) {
                        std::lock_guard<std::mutex> lk(ctrl_mtx);
                        boost_on = !boost_on;
                        const float boost =
                            boost_on ? static_cast<float>(std::pow(10.0, midi_cfg.boost_db / 20.0))
                                     : 1.0f;
                        master_gain.store(base_master * boost, std::memory_order_relaxed);
                        apply_leds();
                        std::printf("  boost: %s (PC%d)\n", boost_on ? "on" : "off", e.cc);
                        std::fflush(stdout);
                    } else if (drive_led >= 0) {
                        std::lock_guard<std::mutex> lk(ctrl_mtx);
                        const bool same =
                            (cfg.drive_type == namzero::kDriveTypeNames[drive_led] &&
                             cfg.drive_on);
                        cfg.drive_type = namzero::kDriveTypeNames[drive_led];
                        drive_ptr->set_type(drive_type_enum(drive_led));
                        cfg.drive_on = !same;
                        if (i_drive != kNoSlot) chain.set_enabled(i_drive, cfg.drive_on);
                        apply_leds();
                        std::printf("  drive: %s %s (PC%d)\n", cfg.drive_type.c_str(),
                                    cfg.drive_on ? "on" : "off", e.cc);
                        std::fflush(stdout);
                    }
                    continue;
                }

                if (!is_cc || is_expr) continue;

                // Gruppo blu dell'XTONE: navigazione preset. 22 = indietro,
                // 24 = avanti, 25 = primo slot. I CC sono presi prima della
                // mappa degli slot, cosi' non vengono interpretati due volte.
                if (is_cc) {
                    int target = -1;
                    const int cur = current_slot.load();
                    // 8 slot: la navigazione gira su 0-7.
                    if (e.cc == 22) target = (cur + 7) % 8;
                    else if (e.cc == 24) target = (cur + 1) % 8;
                    else if (e.cc == 25) target = 0;
                    if (target >= 0) {
                        switch_slot.store(target);
                        current_slot.store(target);
                        std::printf("  preset: slot %d (nav CC%d)\n", target, e.cc);
                        std::fflush(stdout);
                        continue;
                    }
                }



                // Solo il fronte di salita e' una pressione: il rilascio non
                // deve contare, altrimenti tenere premuto spegne subito il blocco.
                if (!switches.pressed(e.cc, e.value, now_ms)) continue;

                if (midi_cfg.boost_cc >= 0 && e.cc == midi_cfg.boost_cc) {
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    boost_on = !boost_on;
                    const float boost =
                        boost_on ? static_cast<float>(std::pow(10.0, midi_cfg.boost_db / 20.0))
                                 : 1.0f;
                    master_gain.store(base_master * boost, std::memory_order_relaxed);
                    apply_leds();
                    if (boost_on) {
                        std::printf("  boost: on (+%.1f dB)\n", midi_cfg.boost_db);
                    } else {
                        std::printf("  boost: off\n");
                    }
                    std::fflush(stdout);
                    continue;
                }

                const int dsel = cc_drive_select[e.cc];
                if (dsel >= 0) {
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    const bool same =
                        (cfg.drive_type == namzero::kDriveTypeNames[dsel] && cfg.drive_on);
                    cfg.drive_type = namzero::kDriveTypeNames[dsel];
                    drive_ptr->set_type(drive_type_enum(dsel));
                    cfg.drive_on = !same;
                    if (i_drive != kNoSlot) chain.set_enabled(i_drive, cfg.drive_on);
                    apply_leds();
                    std::printf("  drive: %s %s (CC%d)\n", cfg.drive_type.c_str(),
                                cfg.drive_on ? "on" : "off", e.cc);
                    std::fflush(stdout);
                    continue;
                }

                const int lsel = cc_delay_select[e.cc];
                if (lsel >= 0) {
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    const char* mode = namzero::kDelayModeNames[lsel];
                    const bool same = (cfg.delay_mode == mode && cfg.delay_on);
                    cfg.delay_mode = mode;
                    if (delay_slot != nullptr) delay_slot->set_mode(delay_mode_enum(lsel));
                    cfg.delay_on = !same;
                    if (i_delay != kNoSlot) chain.set_enabled(i_delay, cfg.delay_on);
                    std::printf("  delay: %s %s (CC%d)\n", mode,
                                cfg.delay_on ? "on" : "off", e.cc);
                    std::fflush(stdout);
                    continue;
                }

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
                    apply_leds();
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
                    apply_leds();
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
                    apply_leds();
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

    while (g_running.load()) {
        const int pending = switch_slot.load();
        if (pending >= 0) {
            if (slot_file(pending).empty()) {
                std::printf("  slot %d vuoto, resta il preset attuale\n", pending);
                std::fflush(stdout);
                switch_slot.store(-1);
            } else {
                break;
            }
        }
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
            buf[i] = soft_clip(buf[i] * master_gain.load(std::memory_order_relaxed));
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

    // Cambio preset: si ri-esegue il processo con -p. Il device viene chiuso
    // prima, cosi' il nuovo processo lo trova libero.
    const int wanted = switch_slot.load();
    if (wanted >= 0 && g_running.load()) {
        const std::string file = slot_file(wanted);
        if (!file.empty()) {
            if (led_out.is_open()) {
                led_out.send_note(0, static_cast<uint8_t>(wanted), 127);
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(midi_cfg.gboard_flash_ms));
            }
            write_last_slot(wanted);
            setenv("NAMZERO_SLOT", std::to_string(wanted).c_str(), 1);
            std::printf("slot %d: %s\n", wanted, file.c_str());
            std::fflush(stdout);
            io.close();
            char* args[] = {argv[0], const_cast<char*>("-p"), const_cast<char*>(file.c_str()),
                            nullptr};
            // Eseguire "/proc/self/exe" cambierebbe il nome del processo in
            // "exe": pgrep/pkill del servizio non lo troverebbero piu'.
            char real[4096];
            const ssize_t n = ::readlink("/proc/self/exe", real, sizeof(real) - 1);
            if (n > 0) {
                real[n] = '\0';
                execv(real, args);
            }
            execv(argv[0], args);
            std::perror("execv");
        }
    }

    g_running.store(false);
    stats_stop.store(true);
    if (stats_thread.joinable()) stats_thread.join();
    if (kb_thread.joinable()) kb_thread.join();
    if (midi_thread.joinable()) midi_thread.join();
    if (ctl_thread.joinable()) ctl_thread.join();
    for (auto& m : midis) m->close();

    io.close();

    std::printf("\nriepilogo: %" PRIu64 " blocchi, %" PRIu64 " xrun\n",
                stats.blocks.load(), stats.xruns.load());
    return 0;
}
