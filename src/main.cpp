#include <algorithm>
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
#include "dsp/eq_slot.h"
#include "dsp/looper_slot.h"
#include "dsp/reverb_slot.h"
#include "dsp/tuner.h"
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
    double drive_octave = 0.0;  // solo fuzz: quota di ottava (0..1, stile Octavia)
    bool drive_octave_on = false;  // solo fuzz: interruttore dell'ottava

    bool mod_on = false;
    std::string mod_type = "tremolo";  // tremolo | univibe | flanger | phaser
    double mod_rate_hz = 4.5;
    double mod_depth = 0.6;
    double mod_fx_mix = 0.5;   // univibe/flanger/phaser: quota di segnale trattato
    double mod_feedback = 0.35;  // flanger/phaser
    // Dove sta il mod nella catena: prima dell'amplificatore (default, come un
    // pedale vero) oppure dopo la IR. Si applica al caricamento del preset.
    std::string mod_position = "pre_amp";  // pre_amp | post_ir

    // FILTER (wah/whammy) fisso prima del drive. Nei preset e' sempre spento:
    // lo accende solo l'auto-engage dell'espressione. rate/depth non sono usati
    // dal wah ma restano configurabili (e nello stato/param della UI).
    bool filter_on = false;
    std::string filter_type = "wah";  // wah | whammy
    double filter_range = 12.0;       // whammy: semitoni a pedale a fondo
    double filter_rate_hz = 1.0;
    double filter_depth = 0.5;
    double filter_q = 3.0;
    double filter_mix = 1.0;          // wah e whammy: quota di wet
    double filter_low_hz = 350.0;
    double filter_high_hz = 2200.0;
    double filter_position = 0.5;

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

    // EQ grafico globale a 5 bande: vive in presets.json (come gate, boost e
    // auto-engage), non nel preset del singolo slot. Sempre in catena.
    bool eq_on = true;
    std::array<double, 5> eq_gains{0.0, 0.0, 0.0, 0.0, 0.0};
    // Volume globale di fine catena, in dB: vive in presets.json accanto all'EQ
    // ma si applica SEMPRE, anche a EQ spento. Default +6 dB.
    double eq_volume_db = 6.0;

    // comportamento
    // La mappa MIDI non sta qui ne' nei preset: vive in midi.json (vedi
    // MidiConfig), cosi' si cambia senza ricompilare e vale per tutti i preset.
    bool midi_log = false;  // stampa ogni messaggio: serve a misurare il pedale
    bool print_config = false;  // stampa i valori effettivi letti dagli slot ed esce

    bool stats = false;
    bool help = false;
};

// Limiti del volume globale (presets.json -> eq.volume_db), in dB.
constexpr double kVolumeMinDb = -24.0;
constexpr double kVolumeMaxDb = 24.0;

// Tipi di mod e drive in ordine di ciclo: la stessa tabella serve a validare
// il preset, al tasto che li scorre e al ripristino dello stato del mod.

// Rate e feedback suggeriti per tipo: quando si scorre con 'w' i parametri si
// adeguano, altrimenti un flanger a 4.5 Hz suonerebbe come un vibrato nervoso.
constexpr float kModDefaultRate[] = {4.5f, 1.0f, 0.5f, 0.8f};
constexpr float kModDefaultFeedback[] = {0.0f, 0.0f, 0.35f, 0.20f};



namzero::ModSlot::Type mod_type_enum(int idx) {
    switch (idx) {
        case 1: return namzero::ModSlot::Type::kUnivibe;
        case 2: return namzero::ModSlot::Type::kFlanger;
        case 3: return namzero::ModSlot::Type::kPhaser;
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
        "  --drive-octave <0..1> solo fuzz: quota di ottava, stile Octavia (default 0)\n"
        "  --drive-octave-on    solo fuzz: accende l'ottava (default spenta)\n"
        "  --tremolo            tremolo nello slot mod\n"
        "  --mod-position <p>   pre_amp (default) | post_ir: dove sta il mod\n"
        "  --wah-position <0..1> posizione del filter (default 0.5)\n"
        "  --wah-q <0.5..12>    risonanza del filter (default 3)\n"
        "  --wah-mix <0..1>     quantita' di filter (default 1)\n"
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
        "  m mod | w tipo mod | , . filter\n"
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

// Interruttori via socket: la UI manda true/false, ma un client esterno puo'
// mandare 1/0 o "on"/"off". Tutto cio' che non e' riconoscibile vale false,
// cosi' il valore resta comunque un bool (clamp).
bool json_truthy(const nlohmann::json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        return s == "1" || s == "true" || s == "on";
    }
    return false;
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
        cfg.drive_octave = get_num(d, "octave", cfg.drive_octave);
        // Interruttore separato dal mix. I preset vecchi non hanno la chiave:
        // partono spenti (default false), col mix salvato che resta invariato.
        cfg.drive_octave_on = get_bool(d, "octave_on", cfg.drive_octave_on);
        // Migrazione: un preset senza "octave" resta a 0 (solo fuzz). Un
        // valore fuori range non deve far partire il motore con una manopola
        // oltre i limiti: si limita come fa il setter.
        if (!std::isfinite(cfg.drive_octave)) {
            std::fprintf(stderr, "drive: octave non valido, uso 0\n");
            cfg.drive_octave = 0.0;
        } else if (cfg.drive_octave < 0.0 || cfg.drive_octave > 1.0) {
            std::fprintf(stderr, "drive: octave %.3f fuori da 0..1, lo limito\n",
                         cfg.drive_octave);
            cfg.drive_octave = std::clamp(cfg.drive_octave, 0.0, 1.0);
        }
    }

    auto md = slot_or_empty(j, "mod");
    if (!md.empty()) {
        cfg.mod_on = get_bool(md, "on", cfg.mod_on);
        if (md.contains("type") && md["type"].is_string()) {
            cfg.mod_type = md["type"].get<std::string>();
            // Compatibilita': il wah non e' piu' un tipo di mod. Un preset
            // vecchio con "wah" diventa tremolo spento, senza fallire; i
            // parametri del wah si leggono dal blocco "filter".
            if (cfg.mod_type == "wah") {
                std::fprintf(stderr,
                             "preset: mod.type \"wah\" non piu' supportato, "
                             "uso tremolo spento (il wah sta nel blocco filter)\n");
                cfg.mod_type = "tremolo";
                cfg.mod_on = false;
            }
        }
        cfg.mod_rate_hz = get_num(md, "rate_hz", cfg.mod_rate_hz);
        cfg.mod_depth = get_num(md, "depth", cfg.mod_depth);
        cfg.mod_fx_mix = get_num(md, "fx_mix", cfg.mod_fx_mix);
        cfg.mod_feedback = get_num(md, "feedback", cfg.mod_feedback);
        // mod.position e' la posizione nella catena. I preset vecchi avevano
        // qui un numero (la posizione del pedale wah): si ignora.
        if (md.contains("position") && md["position"].is_string()) {
            cfg.mod_position = md["position"].get<std::string>();
        }
    }

    // FILTER (wah/whammy) fisso prima del drive. Il flag on del file viene
    // ignorato: si accende solo l'auto-engage dell'espressione.
    auto fl = slot_or_empty(j, "filter");
    if (!fl.empty()) {
        if (fl.contains("type") && fl["type"].is_string()) {
            cfg.filter_type = fl["type"].get<std::string>();
        }
        cfg.filter_range = get_num(fl, "range", cfg.filter_range);
        cfg.filter_rate_hz = get_num(fl, "rate_hz", get_num(fl, "rate", cfg.filter_rate_hz));
        cfg.filter_depth = get_num(fl, "depth", cfg.filter_depth);
        cfg.filter_q = get_num(fl, "q", cfg.filter_q);
        cfg.filter_mix = get_num(fl, "mix", cfg.filter_mix);
        cfg.filter_low_hz = get_num(fl, "low_hz", cfg.filter_low_hz);
        cfg.filter_high_hz = get_num(fl, "high_hz", cfg.filter_high_hz);
        cfg.filter_position = get_num(fl, "position", cfg.filter_position);
    }
    cfg.filter_on = false;
    // Default e migrazione: i preset senza "type" restano wah. Un tipo scritto
    // male non deve diventare un effetto a caso.
    if (namzero::filter_type_index(cfg.filter_type) < 0) {
        std::fprintf(stderr, "filter: tipo \"%s\" sconosciuto, uso wah\n",
                     cfg.filter_type.c_str());
        cfg.filter_type = "wah";
    }
    if (cfg.filter_range < namzero::kFilterRangeMin ||
        cfg.filter_range > namzero::kFilterRangeMax) {
        std::fprintf(stderr, "filter: range %.1f fuori da %.0f..%.0f semitoni, lo limito\n",
                     cfg.filter_range, namzero::kFilterRangeMin,
                     namzero::kFilterRangeMax);
        cfg.filter_range = std::clamp(cfg.filter_range, namzero::kFilterRangeMin,
                                      namzero::kFilterRangeMax);
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
    slots["drive"]["octave"] = cfg.drive_octave;
    slots["drive"]["octave_on"] = cfg.drive_octave_on;
    slots["mod"]["on"] = cfg.mod_on;
    slots["mod"]["type"] = cfg.mod_type;
    slots["mod"]["rate_hz"] = cfg.mod_rate_hz;
    slots["mod"]["depth"] = cfg.mod_depth;
    slots["mod"]["fx_mix"] = cfg.mod_fx_mix;
    slots["mod"]["feedback"] = cfg.mod_feedback;
    slots["mod"]["position"] = cfg.mod_position;
    // Il filter si salva sempre spento: si accende solo col pedale.
    slots["filter"]["on"] = false;
    slots["filter"]["type"] = cfg.filter_type;
    slots["filter"]["range"] = cfg.filter_range;
    slots["filter"]["rate_hz"] = cfg.filter_rate_hz;
    slots["filter"]["depth"] = cfg.filter_depth;
    slots["filter"]["q"] = cfg.filter_q;
    slots["filter"]["mix"] = cfg.filter_mix;
    slots["filter"]["low_hz"] = cfg.filter_low_hz;
    slots["filter"]["high_hz"] = cfg.filter_high_hz;
    slots["filter"]["position"] = cfg.filter_position;
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
        } else if (a == "--mod-position") {
            const char* v = next("--mod-position");
            if (v == nullptr) return false;
            cfg.mod_position = v;
        } else if (a == "--wah-position") {
            const char* v = next("--wah-position");
            if (v == nullptr || !parse_float(v, cfg.filter_position)) {
                err = "valore non valido per --wah-position";
                return false;
            }
        } else if (a == "--wah-q") {
            const char* v = next("--wah-q");
            if (v == nullptr || !parse_float(v, cfg.filter_q)) {
                err = "valore non valido per --wah-q";
                return false;
            }
        } else if (a == "--wah-mix") {
            const char* v = next("--wah-mix");
            if (v == nullptr || !parse_float(v, cfg.filter_mix)) {
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
        } else if (a == "--drive-octave") {
            const char* v = next("--drive-octave");
            if (v == nullptr || !parse_float(v, cfg.drive_octave)) {
                err = "valore non valido per --drive-octave";
                return false;
            }
            cfg.drive_octave = std::isfinite(cfg.drive_octave)
                                   ? std::clamp(cfg.drive_octave, 0.0, 1.0)
                                   : 0.0;
        } else if (a == "--drive-octave-on") {
            cfg.drive_octave_on = true;
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

// Puntatore globale alla config MIDI: serve a lambda con capture list esplicita
// (es. save) per le opzioni globali (auto-engage).
static namzero::MidiConfig* g_midi = nullptr;
static bool g_boost_on = false;

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
    std::atomic<int> ae_rev{0};  // cambia quando la UI tocca l'auto-engage

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
    if (cfg.mod_position != "pre_amp" && cfg.mod_position != "post_ir") {
        std::fprintf(stderr, "mod: posizione \"%s\" sconosciuta, uso pre_amp\n",
                     cfg.mod_position.c_str());
        cfg.mod_position = "pre_amp";
    }

    // --- mappa MIDI: file globale, non preset ------------------------------
    // Precedenza: default compilati < midi.json < flag da riga di comando.
    namzero::MidiConfig midi_cfg;
    g_midi = &midi_cfg;
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
    // Opzioni globali salvate dalla web UI: l'auto-engage vive in presets.json
    // (che i deploy escludono), cosi' non viene sovrascritto.
    if (have_presets) {
        std::ifstream ain("config/presets.json");
        nlohmann::json aj;
        if (ain && (ain >> aj) && aj.contains("autoengage")) {
            const auto& a = aj["autoengage"];
            if (a.contains("threshold")) midi_cfg.wah_threshold = a["threshold"].get<double>();
            if (a.contains("release_threshold")) midi_cfg.wah_release_threshold = a["release_threshold"].get<double>();
            if (a.contains("engage_ms")) midi_cfg.wah_engage_ms = a["engage_ms"].get<int>();
            if (a.contains("release_ms")) midi_cfg.wah_release_ms = a["release_ms"].get<int>();
            if (a.contains("invert")) midi_cfg.wah_invert = a["invert"].get<bool>();
            std::printf("autoengage: soglia %.2f / release %.2f, %d ms / %d ms\n",
                        midi_cfg.wah_threshold, midi_cfg.wah_release_threshold,
                        midi_cfg.wah_engage_ms, midi_cfg.wah_release_ms);
        }
    }
    if (midi_cli.device_set) {
        midi_cfg.device = midi_cli.device;
        midi_cfg.devices = {midi_cli.device};
    }
    if (midi_cli.no_midi) midi_cfg.enabled = false;
    if (midi_cli.expr_invert) midi_cfg.expression_invert = true;
    if (midi_cli.wah_invert) midi_cfg.wah_invert = true;
    if (midi_cfg.gboard_preset_pc && midi_cfg.gboard_pc_effects) {
        std::fprintf(stderr,
                     "avviso: gboard.preset_pc e gboard.pc_effects sono entrambi true: "
                     "i PC 0-7 cambiano preset (priorita' preset); le azioni effetti "
                     "legacy restano possibili solo per PC diversi da 0-7\n");
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Boost e volume globale di fine catena: il gain master vive in un atomico
    // perche' il thread MIDI lo cambia e il thread audio lo legge a ogni blocco.
    const float base_master = static_cast<float>(std::pow(10.0, cfg.gain_db / 20.0));
    std::atomic<float> master_gain{base_master};
    // Volume globale (eq.volume_db): vive in presets.json accanto all'EQ ma si
    // somma SEMPRE al gain master, anche a EQ spento.
    std::atomic<float> volume_db{static_cast<float>(cfg.eq_volume_db)};
    // Il boost e' un'opzione generale: si ripristina da presets.json.
    {
        std::ifstream bst("config/presets.json");
        nlohmann::json bj;
        if (bst && (bst >> bj) && bj.contains("boost")) {
            g_boost_on = bj["boost"].value("on", false);
            midi_cfg.boost_db = bj["boost"].value("db", midi_cfg.boost_db);
        }
    }
    // EQ globale: come boost e auto-engage vive in presets.json, cosi' il
    // deploy non lo sovrascrive e vale per tutti i preset. Assente = default
    // (acceso, piatto, volume +6 dB).
    {
        std::ifstream ein("config/presets.json");
        nlohmann::json ej;
        if (ein && (ein >> ej) && ej.contains("eq") && ej["eq"].is_object()) {
            const auto& e = ej["eq"];
            cfg.eq_on = e.value("on", cfg.eq_on);
            cfg.eq_volume_db = std::clamp(e.value("volume_db", cfg.eq_volume_db),
                                          kVolumeMinDb, kVolumeMaxDb);
            if (e.contains("gains") && e["gains"].is_array()) {
                for (std::size_t i = 0; i < cfg.eq_gains.size() && i < e["gains"].size(); ++i) {
                    if (e["gains"][i].is_number()) {
                        cfg.eq_gains[i] =
                            std::clamp(e["gains"][i].get<double>(),
                                       static_cast<double>(namzero::EqSlot::kMinDb),
                                       static_cast<double>(namzero::EqSlot::kMaxDb));
                    }
                }
            }
        }
    }
    volume_db.store(static_cast<float>(cfg.eq_volume_db), std::memory_order_relaxed);

    // Accordatore: riferimento e transpose vivono in presets.json (blocco
    // "tuner") e non nel preset del singolo slot, come le altre opzioni
    // generali. on/mute NON si persistono: dopo un riavvio il motore riparte
    // spento e non muto (evita di restare in silenzio senza capire perche').
    double tuner_ref = namzero::kTunerRefDefault;
    int tuner_tr = 0;
    {
        std::ifstream tin("config/presets.json");
        nlohmann::json tj;
        if (tin && (tin >> tj) && tj.contains("tuner") && tj["tuner"].is_object()) {
            const auto& t = tj["tuner"];
            if (t.contains("reference_hz") && t["reference_hz"].is_number()) {
                tuner_ref = std::clamp(t["reference_hz"].get<double>(),
                                       namzero::kTunerRefMin, namzero::kTunerRefMax);
            }
            if (t.contains("transpose") && t["transpose"].is_number()) {
                tuner_tr = std::clamp(
                    static_cast<int>(std::lround(t["transpose"].get<double>())),
                    namzero::kTunerTransposeMin, namzero::kTunerTransposeMax);
            }
        }
    }
    std::atomic<double> tuner_reference_hz{tuner_ref};
    std::atomic<int> tuner_transpose{tuner_tr};
    std::atomic<bool> tuner_on{false};
    std::atomic<bool> tuner_muted{false};
    // Ultima lettura grezza del rilevatore: il thread di analisi la scrive, il
    // thread ctl la legge per costruire lo snapshot (nota e cent si calcolano
    // li' con il riferimento corrente, cosi' cambiarli ha effetto immediato).
    std::mutex tuner_mtx;
    namzero::TunerPitch tuner_reading;
    std::printf("tuner: riferimento %.1f Hz, transpose %+d\n", tuner_ref, tuner_tr);

    // Unico punto che scrive master_gain: base (gain_db del preset) * volume
    // globale * boost. Va usato da startup, comandi boost/volume e MIDI, cosi'
    // i due guadagni non si perdono a vicenda.
    const auto update_master = [&]() {
        const float vol = static_cast<float>(
            std::pow(10.0, static_cast<double>(volume_db.load(std::memory_order_relaxed)) / 20.0));
        const float boost = g_boost_on
                                ? static_cast<float>(std::pow(10.0, midi_cfg.boost_db / 20.0))
                                : 1.0f;
        master_gain.store(base_master * vol * boost, std::memory_order_relaxed);
    };
    update_master();
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
    // FILTER (wah/whammy) fisso: seconda istanza di ModSlot, sempre in catena.
    // Nei preset e' spento (lo accende solo l'auto-engage dell'espressione), ma
    // esiste da subito cosi' il pedale puo' comandarlo in qualunque momento.
    const std::size_t i_filter = chain.size();
    namzero::ModSlot* filter_ptr = nullptr;
    {
        auto fl = std::make_unique<namzero::ModSlot>();
        filter_ptr = fl.get();
        fl->set_type(cfg.filter_type == "whammy" ? namzero::ModSlot::Type::kWhammy
                                                 : namzero::ModSlot::Type::kWah);
        fl->set_whammy_range(static_cast<float>(cfg.filter_range));
        fl->set_rate_hz(static_cast<float>(cfg.filter_rate_hz));
        fl->set_depth(static_cast<float>(cfg.filter_depth));
        fl->set_wah_q(static_cast<float>(cfg.filter_q));
        fl->set_wah_mix(static_cast<float>(cfg.filter_mix));
        fl->set_wah_low_hz(static_cast<float>(cfg.filter_low_hz));
        fl->set_wah_high_hz(static_cast<float>(cfg.filter_high_hz));
        fl->set_position(static_cast<float>(cfg.filter_position));
        chain.add(std::move(fl));
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
        dr->set_octave(static_cast<float>(cfg.drive_octave));
        dr->set_octave_on(cfg.drive_octave_on);
        chain.add(std::move(dr));
    }

    // Il mod sta prima dell'amplificatore (default) o dopo la IR: la catena si
    // costruisce all'avvio, quindi la scelta si applica caricando il preset.
    const bool mod_pre_amp = cfg.mod_position == "pre_amp";
    std::size_t i_mod = kNoSlot;
    namzero::ModSlot* mod_ptr = nullptr;
    auto add_mod = [&]() {
        i_mod = chain.size();
        auto mod = std::make_unique<namzero::ModSlot>();
        mod_ptr = mod.get();
        mod->set_type(mod_type_enum(namzero::mod_type_index(cfg.mod_type)));
        mod->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
        mod->set_depth(static_cast<float>(cfg.mod_depth));
        mod->set_fx_mix(static_cast<float>(cfg.mod_fx_mix));
        mod->set_feedback(static_cast<float>(cfg.mod_feedback));
        chain.add(std::move(mod));
    };
    if (mod_pre_amp) add_mod();

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

    if (!mod_pre_amp) add_mod();

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

    // EQ globale a 5 bande: sempre presente, ultimo stadio della catena (dopo
    // riverbero e looper), prima del gain master/boost. L'on/off passa da
    // chain.set_enabled come gli altri slot.
    const std::size_t i_eq = chain.size();
    namzero::EqSlot* eq_ptr = nullptr;
    {
        auto eq = std::make_unique<namzero::EqSlot>();
        eq_ptr = eq.get();
        for (std::size_t i = 0; i < cfg.eq_gains.size(); ++i) {
            eq->set_gain(static_cast<int>(i), static_cast<float>(cfg.eq_gains[i]));
        }
        chain.add(std::move(eq));
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
        std::printf("cfg.drive.octave=%.4f\n", drive_ptr->octave());
        std::printf("cfg.drive.octave_on=%d\n", drive_ptr->octave_on() ? 1 : 0);

        std::printf("cfg.mod.type=%s\n", mod_name(mod_ptr->type()));
        std::printf("cfg.mod.rate_hz=%.4f\n", mod_ptr->rate_hz());
        std::printf("cfg.mod.depth=%.4f\n", mod_ptr->depth());
        std::printf("cfg.mod.fx_mix=%.4f\n", mod_ptr->fx_mix());
        std::printf("cfg.mod.feedback=%.4f\n", mod_ptr->feedback());
        std::printf("cfg.mod.position=%s\n", cfg.mod_position.c_str());

        std::printf("cfg.filter.on=%d\n", 0);
        std::printf("cfg.filter.type=%s\n", cfg.filter_type.c_str());
        std::printf("cfg.filter.range=%.4f\n", filter_ptr->whammy_range());
        std::printf("cfg.filter.rate_hz=%.4f\n", filter_ptr->rate_hz());
        std::printf("cfg.filter.depth=%.4f\n", filter_ptr->depth());
        std::printf("cfg.filter.q=%.4f\n", filter_ptr->wah_q());
        std::printf("cfg.filter.mix=%.4f\n", filter_ptr->wah_mix());
        std::printf("cfg.filter.low_hz=%.4f\n", filter_ptr->wah_low_hz());
        std::printf("cfg.filter.high_hz=%.4f\n", filter_ptr->wah_high_hz());
        std::printf("cfg.filter.position=%.4f\n", filter_ptr->position());

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

        std::printf("cfg.eq.on=%d\n", cfg.eq_on ? 1 : 0);
        std::printf("cfg.eq.volume_db=%.4f\n", cfg.eq_volume_db);
        for (std::size_t i = 0; i < cfg.eq_gains.size(); ++i) {
            std::printf("cfg.eq.gain%zu=%.4f\n", i, eq_ptr->gain_db(static_cast<int>(i)));
        }
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
            std::printf("led: %s (Note On ch1, note da midi.json: 4-7/0-2/3)\n",
                        midi_cfg.gboard_device.c_str());
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

    // Regola: il filter non parte mai acceso, lo accende solo l'auto-engage
    // dell'espressione. Vale anche senza MIDI: il filter non ha un interruttore.
    cfg.filter_on = false;
    if (cfg.mod_type == "wah") cfg.mod_type = "tremolo";  // non deve succedere, ma...

    chain.set_enabled(i_gate, cfg.gate_on);
    chain.set_enabled(i_comp, cfg.comp_on);
    chain.set_enabled(i_filter, false);
    chain.set_enabled(i_drive, cfg.drive_on);
    chain.set_enabled(i_mod, cfg.mod_on);
    chain.set_enabled(i_amp, has_amp);
    if (i_ir != kNoSlot) chain.set_enabled(i_ir, true);
    chain.set_enabled(i_delay, cfg.delay_on);
    chain.set_enabled(i_reverb, cfg.reverb_on);
    chain.set_enabled(i_eq, cfg.eq_on);
    chain.reset();

    std::printf("gate: soglia %.1f dB | attack %.1f ms | release %.0f ms\n",
                cfg.gate_threshold_db, cfg.gate_attack_ms, cfg.gate_release_ms);
    std::printf("comp: soglia %.1f dB | ratio %.1f | attack %.1f ms | release %.0f ms | makeup %.1f dB\n",
                cfg.comp_threshold_db, cfg.comp_ratio, cfg.comp_attack_ms, cfg.comp_release_ms,
                cfg.comp_makeup_db);
    if (cfg.filter_type == "whammy") {
        std::printf("filter: whammy spento (auto-engage) | range %+.0f st | mix %.2f | pos %.2f"
                    " | latenza %.1f ms\n",
                    cfg.filter_range, cfg.filter_mix, cfg.filter_position,
                    static_cast<double>(filter_ptr->latency_frames()) * 1000.0 /
                        static_cast<double>(io.rate()));
    } else {
        std::printf("filter: wah spento (auto-engage) | q %.1f | mix %.2f | %.0f..%.0f Hz | pos %.2f\n",
                    cfg.filter_q, cfg.filter_mix, cfg.filter_low_hz, cfg.filter_high_hz,
                    cfg.filter_position);
    }
    std::printf("slot: gate %s | comp %s | filter %s | drive %s (%s) | mod %s (%s, %s) | amp %s | ir %s | delay %s | reverb %s | eq %s\n",
                cfg.gate_on ? "on" : "off", cfg.comp_on ? "on" : "off",
                cfg.filter_on ? "on" : "off",
                cfg.drive_on ? "on" : "off", cfg.drive_type.c_str(),
                cfg.mod_on ? "on" : "off", cfg.mod_type.c_str(), cfg.mod_position.c_str(),
                has_amp ? "on" : "off", has_ir ? "on" : "off",
                cfg.delay_on ? "on" : "off", cfg.reverb_on ? "on" : "off",
                cfg.eq_on ? "on" : "off");
    std::printf("eq: %s | 80 %+.1f | 250 %+.1f | 800 %+.1f | 2.5k %+.1f | 8k %+.1f dB | volume %+.1f dB\n",
                cfg.eq_on ? "on" : "off", cfg.eq_gains[0], cfg.eq_gains[1],
                cfg.eq_gains[2], cfg.eq_gains[3], cfg.eq_gains[4], cfg.eq_volume_db);
    std::string chain_desc = "gate -> comp -> filter(" + cfg.filter_type + ") -> drive -> ";
    if (mod_pre_amp) chain_desc += "mod -> ";
    chain_desc += "amp";
    if (has_ir) chain_desc += " -> ir";
    if (!mod_pre_amp) chain_desc += " -> mod";
    chain_desc += " -> delay -> reverb -> looper -> eq";
    std::printf("catena: %s\n", chain_desc.c_str());
    const std::size_t latency = chain.latency_frames();
    std::printf("latenza catena: %zu campioni (%.2f ms)\n", latency,
                static_cast<double>(latency) * 1000.0 / 48000.0);
    std::printf("tasti: g gate | c comp | o overdrive | k modello drive | p modo delay | + - volume amp\n"
                "        m mod | w tipo mod | , . filter\n"
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

    // Accordatore: il thread audio copia qui il segnale DRY (input PRIMA della
    // catena). Il ring e' lock-free e non alloca: il thread di analisi legge
    // gli ultimi 4096 campioni ~15 volte al secondo.
    namzero::TunerTap tuner_tap;

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
    // Stato dei LED: 4 mod (note 4,5,6,7), 3 drive (0,1,2), 1 boost (3).
    // Le note vengono da midi.json e sono INDIPENDENTI dai CC/PC in ingresso.
    // Si tiene l'ultimo valore mandato anche quando il G-Board non c'e', cosi'
    // /api/state lo mostra e i test runtime possono verificarlo.
    struct GboardLed {
        int note = -1;
        bool on = false;
    };
    std::array<GboardLed, 8> gboard_led_state{};
    auto apply_leds = [&]() {
        static const int mod_led_type[4] = {0, 1, 2, 3};  // tremolo/univibe/flanger/phaser
        const int mt = namzero::mod_type_index(cfg.mod_type);
        for (int i = 0; i < 4; ++i) {
            const bool on = cfg.mod_on && mt == mod_led_type[i];
            const int note = midi_cfg.gboard_mod_led[static_cast<std::size_t>(i)];
            gboard_led_state[static_cast<std::size_t>(i)] = {note, on};
            if (note >= 0 && led_out.is_open()) {
                led_out.send_note(0, static_cast<uint8_t>(note), on ? 127 : 0);
            }
        }
        const int dt = namzero::drive_type_index(cfg.drive_type);
        for (int i = 0; i < 3; ++i) {
            const bool on = cfg.drive_on && dt == i;
            const int note = midi_cfg.gboard_drive_led[static_cast<std::size_t>(i)];
            gboard_led_state[static_cast<std::size_t>(4 + i)] = {note, on};
            if (note >= 0 && led_out.is_open()) {
                led_out.send_note(0, static_cast<uint8_t>(note), on ? 127 : 0);
            }
        }
        const int boost_note = midi_cfg.gboard_boost_led;
        gboard_led_state[7] = {boost_note, g_boost_on};
        if (boost_note >= 0 && led_out.is_open()) {
            led_out.send_note(0, static_cast<uint8_t>(boost_note), g_boost_on ? 127 : 0);
        }
    };
    apply_leds();

    // Snapshot dello stato live per la UI: si manda intero a ogni cambiamento.
    auto build_state = [&]() {
        nlohmann::json s;
        s["slot"] = current_slot.load();
        s["preset"] = preset_path;
        s["boost"] = g_boost_on;
        s["boost_db"] = midi_cfg.boost_db;
        // G-Board vista dalla UI/test: ingressi configurati e note dei LED.
        // "leds" e' l'ultimo stato mandato (note reali, non numeri di CC).
        {
            nlohmann::json& gb = s["gboard"];
            gb["mod_cc"] = midi_cfg.gboard_mod_cc;
            gb["drive_cc"] = midi_cfg.gboard_drive_cc;
            gb["boost_cc"] = midi_cfg.gboard_boost_cc;
            gb["preset_pc"] = midi_cfg.gboard_preset_pc;
            gb["pc_effects"] = midi_cfg.gboard_pc_effects;
            gb["mod_led"] = midi_cfg.gboard_mod_led;
            gb["drive_led"] = midi_cfg.gboard_drive_led;
            gb["boost_led"] = midi_cfg.gboard_boost_led;
            gb["leds"] = nlohmann::json::array();
            for (const auto& l : gboard_led_state) {
                gb["leds"].push_back({{"note", l.note}, {"on", l.on}});
            }
        }
        s["autoengage"] = {{"threshold", midi_cfg.wah_threshold},
                           {"release_threshold", midi_cfg.wah_release_threshold},
                           {"engage_ms", midi_cfg.wah_engage_ms},
                           {"release_ms", midi_cfg.wah_release_ms}};
        s["eq"]["on"] = cfg.eq_on;
        s["eq"]["gains"] = cfg.eq_gains;
        // volume = gain globale in dB (volume_db), master_gain = lineare
        // risultante (base * volume * boost): serve ai test e alla diagnostica.
        s["eq"]["volume"] = volume_db.load(std::memory_order_relaxed);
        s["master_gain"] = master_gain.load(std::memory_order_relaxed);
        if (looper_ptr != nullptr) {
            s["looper"]["state"] = namzero::LooperSlot::state_name(looper_ptr->state());
            s["looper"]["pos"] = looper_ptr->position_seconds();
            s["looper"]["len"] = looper_ptr->length_seconds();
            s["looper"]["max"] = looper_ptr->max_seconds();
            s["looper"]["armed"] = looper_ptr->armed();
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
        sl["drive"]["octave"] = cfg.drive_octave;
        sl["drive"]["octave_on"] = cfg.drive_octave_on;
        sl["mod"]["on"] = cfg.mod_on;
        sl["mod"]["type"] = cfg.mod_type;
        sl["mod"]["rate"] = cfg.mod_rate_hz;
        sl["mod"]["depth"] = cfg.mod_depth;
        sl["mod"]["feedback"] = cfg.mod_feedback;
        sl["mod"]["fx_mix"] = cfg.mod_fx_mix;
        sl["mod"]["position"] = cfg.mod_position;
        sl["filter"]["on"] = cfg.filter_on;
        sl["filter"]["type"] = cfg.filter_type;
        sl["filter"]["range"] = cfg.filter_range;
        // Rapporto effettivo del pitch shifter (1.0 per il wah): la UI non lo
        // usa, ma il test runtime verifica che il pedale a fondo alzi davvero.
        sl["filter"]["ratio"] = cfg.filter_type == "whammy" ? filter_ptr->whammy_ratio() : 1.0;
        sl["filter"]["rate"] = cfg.filter_rate_hz;
        sl["filter"]["depth"] = cfg.filter_depth;
        sl["filter"]["q"] = cfg.filter_q;
        sl["filter"]["mix"] = cfg.filter_mix;
        sl["filter"]["low_hz"] = cfg.filter_low_hz;
        sl["filter"]["high_hz"] = cfg.filter_high_hz;
        sl["filter"]["position"] = cfg.filter_position;
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
        // Accordatore: lo snapshot contiene il riferimento impostato e quello
        // effettivo (base * 2^(transpose/12)). Se e' spento o non c'e' una nota
        // stabile, note/freq/cents restano neutri: la UI mostra "—".
        {
            const bool on = tuner_on.load(std::memory_order_relaxed);
            const bool muted = tuner_muted.load(std::memory_order_relaxed);
            const double ref = tuner_reference_hz.load(std::memory_order_relaxed);
            const int tr = tuner_transpose.load(std::memory_order_relaxed);
            nlohmann::json& t = s["tuner"];
            t["on"] = on;
            t["muted"] = muted;
            t["reference_hz"] = ref;
            t["transpose"] = tr;
            t["effective_reference_hz"] = namzero::tuner_effective_reference(ref, tr);
            namzero::TunerPitch p;
            {
                std::lock_guard<std::mutex> lk(tuner_mtx);
                p = tuner_reading;
            }
            const auto note = namzero::tuner_note_from_freq(p.freq, ref, tr);
            if (on && p.voiced && note.valid) {
                t["note"] = note.name + std::to_string(note.octave);
                // 0.1 Hz / 0.1 cent: abbastanza fine per l'ago, abbastanza
                // grossolano da non far ballare lo snapshot a ogni campione.
                t["cents"] = std::round(note.cents * 10.0) / 10.0;
                t["freq"] = std::round(p.freq * 10.0) / 10.0;
                t["confidence"] = std::round(p.confidence * 100.0) / 100.0;
            } else {
                t["note"] = "";
                t["cents"] = 0.0;
                t["freq"] = 0.0;
                t["confidence"] = 0.0;
            }
        }
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
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            // Lo stato va a tutti: chi si e' appena collegato (o ha aperto la
            // connessione insieme a un cambio preset) non deve aspettare.
            ctl.broadcast(build_state());
        } else if (cmd == "ping") {
            // Keepalive della connessione SSE: nessuna azione, serve solo a
            // far vedere al motore che il client e' ancora vivo (idle 10 s).
        } else if (cmd == "tuner") {
            // Tre forme indipendenti, combinabili in un solo messaggio:
            // {"on":bool}, {"mute":bool}, {"reference_hz":float} (415..450),
            // {"transpose":int} (-6..+6). Nessun re-exec: l'analisi legge gli
            // atomici al giro successivo e lo snapshot cambia da solo.
            if (j.contains("on")) tuner_on.store(j.value("on", false));
            if (j.contains("mute")) tuner_muted.store(j.value("mute", false));
            if (j.contains("reference_hz")) {
                const double v = std::clamp(j.value("reference_hz", tuner_ref),
                                            namzero::kTunerRefMin,
                                            namzero::kTunerRefMax);
                tuner_reference_hz.store(v, std::memory_order_relaxed);
            }
            if (j.contains("transpose")) {
                const int v = std::clamp(
                    static_cast<int>(std::lround(j.value("transpose", 0.0))),
                    namzero::kTunerTransposeMin, namzero::kTunerTransposeMax);
                tuner_transpose.store(v, std::memory_order_relaxed);
            }
        } else if (cmd == "slot") {
            const int n = j.value("n", -1);
            if (n >= 0 && n < 10) {
                switch_slot.store(n);
                current_slot.store(n);
            }
        } else if (cmd == "reload") {
            switch_slot.store(current_slot.load());
        } else if (cmd == "save_options") {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            try {
                std::ifstream gin("config/presets.json");
                if (!gin) throw std::runtime_error("presets.json non leggibile");
                nlohmann::json gj;
                gin >> gj;
                gj["gate"]["threshold_db"] = cfg.gate_threshold_db;
                gj["gate"]["attack_ms"] = cfg.gate_attack_ms;
                gj["gate"]["release_ms"] = cfg.gate_release_ms;
                gj["autoengage"]["threshold"] = g_midi->wah_threshold;
                gj["autoengage"]["release_threshold"] = g_midi->wah_release_threshold;
                gj["autoengage"]["engage_ms"] = g_midi->wah_engage_ms;
                gj["autoengage"]["release_ms"] = g_midi->wah_release_ms;
                gj["boost"]["on"] = g_boost_on;
                gj["boost"]["db"] = g_midi->boost_db;
                gj["eq"]["on"] = cfg.eq_on;
                gj["eq"]["gains"] = cfg.eq_gains;
                gj["eq"]["volume_db"] = cfg.eq_volume_db;
                // Blocco tuner: solo riferimento e transpose (on/mute non si
                // persistono, vedi il commento all'avvio).
                nlohmann::json tuner_json;
                tuner_json["reference_hz"] =
                    tuner_reference_hz.load(std::memory_order_relaxed);
                tuner_json["transpose"] =
                    tuner_transpose.load(std::memory_order_relaxed);
                gj["tuner"] = tuner_json;
                std::ofstream gout("config/presets.json");
                if (!gout) throw std::runtime_error("presets.json non scrivibile");
                gout << gj.dump(2) << '\n';
                if (!gout) throw std::runtime_error("scrittura fallita");
                std::printf("  opzioni generali salvate\n");
            } catch (const std::exception& e) {
                std::printf("  opzioni: %s\n", e.what());
            }
            std::fflush(stdout);
        } else if (cmd == "save") {
            std::string e;
            if (save_preset(preset_path, cfg, e)) {
                std::printf("  preset salvato: %s\n", preset_path.c_str());
            } else {
                std::printf("  save: %s\n", e.c_str());
            }
            std::fflush(stdout);
        } else if (cmd == "rename") {
            std::string name = j.value("name", "");
            std::string clean;
            for (char ch : name) {
                if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
                    clean += ch;
                } else if (ch == ' ') {
                    clean += '_';
                }
            }
            if (clean.empty() || preset_path.empty()) {
                std::printf("  rename: nome non valido\n");
                std::fflush(stdout);
                return;
            }
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            const std::string old_path = preset_path;
            const std::string old_name = old_path.substr(old_path.find_last_of('/') + 1);
            const std::string new_path = "config/presets/" + clean + ".json";
            std::ifstream exists(new_path);
            if (exists.good()) {
                std::printf("  rename: %s esiste gia'\n", new_path.c_str());
                std::fflush(stdout);
                return;
            }
            if (std::rename(old_path.c_str(), new_path.c_str()) != 0) {
                std::printf("  rename: errore nel rinominare il file\n");
                std::fflush(stdout);
                return;
            }
            preset_path = new_path;
            try {
                std::ifstream gin("config/presets.json");
                nlohmann::json pj;
                gin >> pj;
                for (auto& slot : pj["slots"]) {
                    if (slot.is_string()) {
                        const std::string v = slot.get<std::string>();
                        if (v.size() >= old_name.size() &&
                            v.compare(v.size() - old_name.size(), old_name.size(), old_name) == 0) {
                            slot = "presets/" + clean + ".json";
                        }
                    }
                }
                std::ofstream gout("config/presets.json");
                gout << pj.dump(2) << '\n';
            } catch (...) {
                std::printf("  rename: presets.json non aggiornato\n");
            }
            std::printf("  rename: %s -> %s\n", old_path.c_str(), new_path.c_str());
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
                cfg.drive_octave = d.drive_octave;
                cfg.drive_octave_on = d.drive_octave_on;
                drive_ptr->set_type(drive_type_enum(namzero::drive_type_index(cfg.drive_type)));
                drive_ptr->set_drive(static_cast<float>(cfg.drive_amount));
                drive_ptr->set_tone(static_cast<float>(cfg.drive_tone));
                drive_ptr->set_blend(static_cast<float>(cfg.drive_blend));
                drive_ptr->set_level_db(static_cast<float>(cfg.drive_level_db));
                drive_ptr->set_mid_db(static_cast<float>(cfg.drive_mid_db));
                drive_ptr->set_low_db(static_cast<float>(cfg.drive_low_db));
                drive_ptr->set_octave(static_cast<float>(cfg.drive_octave));
                drive_ptr->set_octave_on(cfg.drive_octave_on);
                if (i_drive != kNoSlot) chain.set_enabled(i_drive, cfg.drive_on);
                apply_leds();
            } else if (slot == "mod") {
                cfg.mod_on = d.mod_on;
                cfg.mod_type = d.mod_type;
                cfg.mod_rate_hz = d.mod_rate_hz;
                cfg.mod_depth = d.mod_depth;
                cfg.mod_feedback = d.mod_feedback;
                cfg.mod_fx_mix = d.mod_fx_mix;
                // mod_position non si ripristina: e' la posizione nella catena,
                // non un parametro dell'effetto (e cambiarla richiede un reload).
                mod_ptr->set_type(mod_type_enum(namzero::mod_type_index(cfg.mod_type)));
                mod_ptr->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
                mod_ptr->set_depth(static_cast<float>(cfg.mod_depth));
                mod_ptr->set_feedback(static_cast<float>(cfg.mod_feedback));
                mod_ptr->set_fx_mix(static_cast<float>(cfg.mod_fx_mix));
                if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                apply_leds();
            } else if (slot == "filter") {
                cfg.filter_type = d.filter_type;
                cfg.filter_range = d.filter_range;
                cfg.filter_rate_hz = d.filter_rate_hz;
                cfg.filter_depth = d.filter_depth;
                cfg.filter_q = d.filter_q;
                cfg.filter_mix = d.filter_mix;
                cfg.filter_low_hz = d.filter_low_hz;
                cfg.filter_high_hz = d.filter_high_hz;
                cfg.filter_position = d.filter_position;
                filter_ptr->set_type(cfg.filter_type == "whammy"
                                         ? namzero::ModSlot::Type::kWhammy
                                         : namzero::ModSlot::Type::kWah);
                filter_ptr->set_whammy_range(static_cast<float>(cfg.filter_range));
                filter_ptr->set_rate_hz(static_cast<float>(cfg.filter_rate_hz));
                filter_ptr->set_depth(static_cast<float>(cfg.filter_depth));
                filter_ptr->set_wah_q(static_cast<float>(cfg.filter_q));
                filter_ptr->set_wah_mix(static_cast<float>(cfg.filter_mix));
                filter_ptr->set_wah_low_hz(static_cast<float>(cfg.filter_low_hz));
                filter_ptr->set_wah_high_hz(static_cast<float>(cfg.filter_high_hz));
                filter_ptr->set_position(static_cast<float>(cfg.filter_position));
                // filter_on resta com'e': lo comanda solo l'espressione.
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
                update_master();
            } else if (slot == "eq") {
                cfg.eq_on = d.eq_on;
                cfg.eq_gains = d.eq_gains;
                cfg.eq_volume_db = d.eq_volume_db;
                volume_db.store(static_cast<float>(cfg.eq_volume_db),
                                std::memory_order_relaxed);
                for (std::size_t i = 0; i < cfg.eq_gains.size(); ++i) {
                    eq_ptr->set_gain(static_cast<int>(i), static_cast<float>(cfg.eq_gains[i]));
                }
                chain.set_enabled(i_eq, cfg.eq_on);
                update_master();
            }
        } else if (cmd == "looper") {
            const std::string action = j.value("action", "");
            if (looper_ptr != nullptr) {
                if (action == "toggle") looper_ptr->command(namzero::LooperSlot::Command::kToggle);
                else if (action == "stop") looper_ptr->command(namzero::LooperSlot::Command::kStop);
                else if (action == "clear") looper_ptr->command(namzero::LooperSlot::Command::kClear);
            }
        } else if (cmd == "autoengage") {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            const std::string key = j.value("key", "");
            const double v = j.value("value", 0.0);
            if (key == "threshold") g_midi->wah_threshold = v;
            else if (key == "release_threshold") g_midi->wah_release_threshold = v;
            else if (key == "engage_ms") g_midi->wah_engage_ms = static_cast<int>(v);
            else if (key == "release_ms") g_midi->wah_release_ms = static_cast<int>(v);
            ae_rev.fetch_add(1, std::memory_order_relaxed);
        } else if (cmd == "boost") {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            g_boost_on = j.value("on", g_boost_on);
            update_master();
            apply_leds();
        } else if (cmd == "eq") {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            // Tre forme: {"cmd":"eq","on":bool}, {"cmd":"eq","band":0..4,
            // "gain":db} e {"cmd":"eq","volume":db}. Nessuna fa re-exec: lo
            // slot e' sempre in catena e i guadagni sono atomici.
            if (j.contains("on")) {
                cfg.eq_on = j.value("on", cfg.eq_on);
                chain.set_enabled(i_eq, cfg.eq_on);
            }
            if (j.contains("band")) {
                const int band = j.value("band", -1);
                if (band >= 0 && band < namzero::EqSlot::kBandCount) {
                    const double gain = std::clamp(
                        j.value("gain", cfg.eq_gains[static_cast<std::size_t>(band)]),
                        static_cast<double>(namzero::EqSlot::kMinDb),
                        static_cast<double>(namzero::EqSlot::kMaxDb));
                    cfg.eq_gains[static_cast<std::size_t>(band)] = gain;
                    eq_ptr->set_gain(band, static_cast<float>(gain));
                }
            }
            // Terza forma: {"cmd":"eq","volume":dB}. Volume globale: clamp
            // -24..+24 e gain master ricalcolato subito (anche a EQ spento).
            if (j.contains("volume")) {
                const double vol = std::clamp(
                    j.value("volume", cfg.eq_volume_db), kVolumeMinDb, kVolumeMaxDb);
                cfg.eq_volume_db = vol;
                volume_db.store(static_cast<float>(vol), std::memory_order_relaxed);
                update_master();
            }
        } else if (cmd == "set") {
            const std::string slot = j.value("slot", "");
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            if (slot == "mod") {
                if (j.contains("type")) {
                    const std::string type = j["type"].get<std::string>();
                    if (namzero::mod_type_index(type) >= 0) {
                        cfg.mod_type = type;
                        mod_ptr->set_type(mod_type_enum(namzero::mod_type_index(cfg.mod_type)));
                    } else {
                        std::printf("  mod: tipo \"%s\" non valido (tremolo/univibe/flanger/phaser)\n",
                                    type.c_str());
                        std::fflush(stdout);
                    }
                }
                // La posizione nella catena si applica solo ricaricando: si
                // salva il preset e si chiede il re-exec, come il comando file.
                if (j.contains("position")) {
                    const std::string position = j["position"].get<std::string>();
                    if (position != "pre_amp" && position != "post_ir") {
                        std::printf("  mod: posizione \"%s\" non valida (pre_amp/post_ir)\n",
                                    position.c_str());
                        std::fflush(stdout);
                    } else if (position != cfg.mod_position) {
                        cfg.mod_position = position;
                        std::string e;
                        if (!preset_path.empty() && save_preset(preset_path, cfg, e)) {
                            std::printf("  mod: posizione %s, ricarico il preset\n",
                                        position.c_str());
                            std::fflush(stdout);
                            switch_slot.store(current_slot.load());
                        } else {
                            std::printf("  mod: posizione %s non salvata (%s)\n",
                                        position.c_str(),
                                        preset_path.empty() ? "nessun preset" : e.c_str());
                            std::fflush(stdout);
                        }
                    }
                }
                cfg.mod_on = j.value("on", cfg.mod_on);
                if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
                apply_leds();
            } else if (slot == "filter") {
                // Il filter non ha on/off: lo comanda solo l'auto-engage.
                // Dal vivo si puo' cambiare tipo e range; il mix passa da param.
                if (j.contains("type")) {
                    const std::string type = j["type"].is_string()
                                                 ? j["type"].get<std::string>()
                                                 : std::string();
                    if (namzero::filter_type_index(type) < 0) {
                        std::printf("  filter: tipo \"%s\" non valido (wah/whammy)\n",
                                    type.c_str());
                        std::fflush(stdout);
                    } else {
                        cfg.filter_type = type;
                        filter_ptr->set_type(type == "whammy"
                                                 ? namzero::ModSlot::Type::kWhammy
                                                 : namzero::ModSlot::Type::kWah);
                    }
                }
                if (j.contains("range") && j["range"].is_number()) {
                    cfg.filter_range = std::clamp(j["range"].get<double>(),
                                                  namzero::kFilterRangeMin,
                                                  namzero::kFilterRangeMax);
                    filter_ptr->set_whammy_range(static_cast<float>(cfg.filter_range));
                }
            } else if (slot == "drive") {
                if (j.contains("type")) {
                    cfg.drive_type = j["type"].get<std::string>();
                    drive_ptr->set_type(drive_type_enum(namzero::drive_type_index(cfg.drive_type)));
                }
                // Interruttore dell'ottava: accettato anche via set, oltre che
                // via param (la UI usa param). Vale solo per il fuzz, ma resta
                // salvato sugli altri tipi come il mix.
                if (j.contains("octave_on")) {
                    cfg.drive_octave_on = json_truthy(j["octave_on"]);
                    drive_ptr->set_octave_on(cfg.drive_octave_on);
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
            // Il valore puo' essere numerico (manopole) o stringa (type del
            // filter): si legge solo se e' un numero, cosi' nlohmann non lancia.
            const double v = (j.contains("value") && j["value"].is_number())
                                 ? j["value"].get<double>()
                                 : 0.0;
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            if (slot == "boost") {
                // Il boost non e' uno slot della catena: e' un guadagno del
                // master. Il parametro arriva dalla manopola della web UI.
                if (key == "db") {
                    midi_cfg.boost_db = std::clamp(v, 0.0, 24.0);
                    update_master();
                }
            } else if (slot == "gate") {
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
                else if (key == "octave") {
                    // Clamp qui (e nel setter): la manopola non deve poter
                    // mandare il parametro fuori da 0..1 via socket.
                    cfg.drive_octave = std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
                    drive_ptr->set_octave(static_cast<float>(cfg.drive_octave));
                }
                else if (key == "octave_on") {
                    // Interruttore separato dal mix: spegnendolo il mix resta
                    // nel config e nello stato, pronto per la riaccensione.
                    cfg.drive_octave_on =
                        j.contains("value") ? json_truthy(j["value"]) : false;
                    drive_ptr->set_octave_on(cfg.drive_octave_on);
                }
            } else if (slot == "mod") {
                if (key == "rate") { cfg.mod_rate_hz = v; mod_ptr->set_rate_hz(static_cast<float>(v)); }
                else if (key == "depth") { cfg.mod_depth = v; mod_ptr->set_depth(static_cast<float>(v)); }
                else if (key == "feedback") { cfg.mod_feedback = v; mod_ptr->set_feedback(static_cast<float>(v)); }
                else if (key == "fx_mix") { cfg.mod_fx_mix = v; mod_ptr->set_fx_mix(static_cast<float>(v)); }
            } else if (slot == "filter") {
                if (key == "type") {
                    const std::string type =
                        (j.contains("value") && j["value"].is_string())
                            ? j["value"].get<std::string>()
                            : std::string();
                    if (namzero::filter_type_index(type) < 0) {
                        std::printf("  filter: tipo \"%s\" non valido (wah/whammy)\n",
                                    type.c_str());
                        std::fflush(stdout);
                    } else {
                        cfg.filter_type = type;
                        filter_ptr->set_type(type == "whammy"
                                                 ? namzero::ModSlot::Type::kWhammy
                                                 : namzero::ModSlot::Type::kWah);
                    }
                } else if (key == "range") {
                    cfg.filter_range = std::clamp(v, namzero::kFilterRangeMin,
                                                  namzero::kFilterRangeMax);
                    filter_ptr->set_whammy_range(static_cast<float>(cfg.filter_range));
                } else if (key == "rate") { cfg.filter_rate_hz = v; filter_ptr->set_rate_hz(static_cast<float>(v)); }
                else if (key == "depth") { cfg.filter_depth = v; filter_ptr->set_depth(static_cast<float>(v)); }
                else if (key == "q") { cfg.filter_q = v; filter_ptr->set_wah_q(static_cast<float>(v)); }
                else if (key == "mix") {
                    cfg.filter_mix = std::clamp(v, 0.0, 1.0);
                    filter_ptr->set_wah_mix(static_cast<float>(cfg.filter_mix));
                }
                else if (key == "low_hz") { cfg.filter_low_hz = v; filter_ptr->set_wah_low_hz(static_cast<float>(v)); }
                else if (key == "high_hz") { cfg.filter_high_hz = v; filter_ptr->set_wah_high_hz(static_cast<float>(v)); }
                else if (key == "position") { cfg.filter_position = v; filter_ptr->set_position(static_cast<float>(v)); }
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
    // Il filter non ha un tasto on/off: lo comanda solo l'auto-engage
    // dell'espressione. Qui la tastiera ne regola al massimo la posizione.
    std::thread kb_thread([&chain, &cfg, &preset_path, &ctrl_mtx, &switch_slot, &current_slot, &apply_leds, looper_ptr,
                           drive_ptr, amp_ptr, i_gate, i_comp, i_drive, i_mod,
                           i_delay, i_ir, i_reverb, mod_ptr, filter_ptr, delay_slot,
                           &tuner_reference_hz, &tuner_transpose]() {
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
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    toggle("mod", i_mod, cfg.mod_on);
                    break;
                }
                case 'w': {
                    if (mod_ptr == nullptr) break;
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    // Quattro tipi, in cerchio: il wah non e' piu' di casa qui.
                    int next = namzero::mod_type_index(cfg.mod_type) + 1;
                    if (next < 0 || next >= static_cast<int>(namzero::kModTypeCount)) next = 0;
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
                    if (filter_ptr == nullptr) break;
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    double p = cfg.filter_position + (ch == '.' ? 0.1 : -0.1);
                    p = p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
                    cfg.filter_position = p;
                    filter_ptr->set_position(static_cast<float>(p));
                    std::printf("  filter: %.2f%s\n", p,
                                cfg.filter_on ? "" : " (spento: si accende col pedale)");
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
                // Il gate e' globale: persiste anche in presets.json, altrimenti
                // al reload le sue manopole tornano ai valori del file.
                try {
                    std::ifstream gin("config/presets.json");
                    nlohmann::json gj;
                    gin >> gj;
                    gj["gate"]["threshold_db"] = cfg.gate_threshold_db;
                    gj["gate"]["attack_ms"] = cfg.gate_attack_ms;
                    gj["gate"]["release_ms"] = cfg.gate_release_ms;
                    gj["autoengage"]["threshold"] = g_midi->wah_threshold;
                    gj["autoengage"]["release_threshold"] = g_midi->wah_release_threshold;
                    gj["autoengage"]["engage_ms"] = g_midi->wah_engage_ms;
                    gj["autoengage"]["release_ms"] = g_midi->wah_release_ms;
                    gj["boost"]["on"] = g_boost_on;
                    gj["boost"]["db"] = g_midi->boost_db;
                    gj["eq"]["on"] = cfg.eq_on;
                    gj["eq"]["gains"] = cfg.eq_gains;
                    gj["eq"]["volume_db"] = cfg.eq_volume_db;
                    nlohmann::json tuner_json;
                    tuner_json["reference_hz"] =
                        tuner_reference_hz.load(std::memory_order_relaxed);
                    tuner_json["transpose"] =
                        tuner_transpose.load(std::memory_order_relaxed);
                    gj["tuner"] = tuner_json;
                    std::ofstream gout("config/presets.json");
                    gout << gj.dump(2) << '\n';
                } catch (...) {
                    std::printf("  save: presets.json non aggiornato\n");
                }
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
        std::uint64_t seen_clients = 0;
        while (g_running.load()) {
            std::vector<namzero::CtlLine> lines;
            ctl.poll(lines);
            for (const auto& in : lines) {
                try {
                    handle_ctl(in.line);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "ctl: %s\n", e.what());
                }
            }
            std::string snap;
            {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                snap = build_state();
            }
            // Broadcast: snapshot a TUTTI i client, anche a chi si e' appena
            // collegato (accepted_total cambia anche se il client e' gia'
            // uscito) e a ogni cambiamento di stato.
            const std::uint64_t accepted = ctl.accepted_total();
            if (ctl.connected() && (accepted != seen_clients || snap != last)) {
                ctl.broadcast(snap);
                last = snap;
            }
            seen_clients = accepted;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    // Accordatore: thread dedicato (~15 Hz). Analizza il DRY e pubblica la
    // lettura con isteresi: lo snapshot cambia solo quando serve, cosi' il
    // broadcast SSE resta fluido senza spam.
    std::atomic<bool> tuner_stop{false};
    const bool tuner_verbose = cfg.stats;
    std::thread tuner_thread([&]() {
        namzero::TunerDetector detector;
        std::array<float, namzero::TunerDetector::kWindow> window{};
        uint64_t analyses = 0;
        uint64_t total_us = 0;
        bool was_on = false;
        auto on_since = std::chrono::steady_clock::now();
        auto last_analysis = std::chrono::steady_clock::now();
        while (g_running.load() && !tuner_stop.load()) {
            for (int i = 0; i < 3 && g_running.load() && !tuner_stop.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (!g_running.load() || tuner_stop.load()) break;
            const bool on = tuner_on.load(std::memory_order_relaxed);
            if (!on) {
                // Spento: azzera l'hold, altrimenti riaccendendolo in silenzio
                // si vedrebbe per 600 ms l'ultima nota rimasta in memoria.
                detector.reset();
                was_on = false;
                last_analysis = std::chrono::steady_clock::now();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!was_on) {
                was_on = true;
                on_since = now;
                last_analysis = now;
            }
            // La finestra e' 85 ms: dopo l'accensione si aspetta che il ring
            // contenga solo audio nuovo (niente nota fantasma da dati vecchi).
            if (now - on_since < std::chrono::milliseconds(120)) continue;

            tuner_tap.read_latest(window.data(), window.size());
            const auto t0 = std::chrono::steady_clock::now();
            // dt reale fra le analisi: l'hold (600 ms) e il gate adattivo
            // seguono il tempo vero, non il numero di frame.
            const double dt =
                std::chrono::duration<double>(t0 - last_analysis).count();
            last_analysis = t0;
            const auto p = detector.analyze(window.data(), window.size(),
                                            io.rate(), dt);
            const auto t1 = std::chrono::steady_clock::now();
            const auto us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
            ++analyses;
            total_us += us;

            {
                std::lock_guard<std::mutex> lk(tuner_mtx);
                const namzero::TunerPitch& pub = tuner_reading;
                bool changed = p.voiced != pub.voiced;
                if (!changed && p.voiced) {
                    const double df = std::fabs(p.freq - pub.freq);
                    // Isteresi: 0.02% di frequenza (circa 0.35 cent a 440 Hz)
                    // e 5% di confidenza. Sotto questa soglia la lettura
                    // pubblicata non cambia e il broadcast non parte.
                    changed = df > std::max(0.02, pub.freq * 0.0002) ||
                              std::fabs(p.confidence - pub.confidence) > 0.05;
                }
                if (changed) tuner_reading = p;
            }
            if (tuner_verbose && analyses % 100 == 0) {
                const double avg_us = static_cast<double>(total_us) / analyses;
                std::printf("tuner: %" PRIu64
                            " analisi, media %.2f ms (~%.1f%% di un core)\n",
                            analyses, avg_us / 1000.0, avg_us * 15.0 / 1e4);
                std::fflush(stdout);
            }
        }
        if (analyses > 0) {
            const double avg_us = static_cast<double>(total_us) / analyses;
            std::printf("tuner: %" PRIu64
                        " analisi, media %.2f ms (~%.1f%% di un core a 15 Hz)\n",
                        analyses, avg_us / 1000.0, avg_us * 15.0 / 1e4);
            std::fflush(stdout);
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
        // La G-Board manda 127 alla pressione e 0 al rilascio: per lei conta
        // sempre e solo il fronte di salita, indipendentemente da `switches`
        // (che vale per gli switch a stato dell'XTONE).
        namzero::SwitchDebounce gboard_switches;
        gboard_switches.set_mode(namzero::SwitchDebounce::Mode::kEdge);

        // Le tre azioni della G-Board stanno qui una volta sola: le usano sia i
        // CC (50-57) sia i vecchi Program Change. I LED si aggiornano sempre
        // con le note di midi.json, mai con il numero del messaggio ricevuto.
        auto gboard_mod = [&](int idx, const char* kind, int number) {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            const bool same = (cfg.mod_type == namzero::kModTypeNames[idx] && cfg.mod_on);
            cfg.mod_type = namzero::kModTypeNames[idx];
            cfg.mod_rate_hz = kModDefaultRate[idx];
            cfg.mod_feedback = kModDefaultFeedback[idx];
            mod_ptr->set_type(mod_type_enum(idx));
            mod_ptr->set_rate_hz(static_cast<float>(cfg.mod_rate_hz));
            mod_ptr->set_feedback(static_cast<float>(cfg.mod_feedback));
            cfg.mod_on = !same;
            if (i_mod != kNoSlot) chain.set_enabled(i_mod, cfg.mod_on);
            apply_leds();
            std::printf("  mod: %s %s (%s%d)\n", cfg.mod_type.c_str(),
                        cfg.mod_on ? "on" : "off", kind, number);
            std::fflush(stdout);
        };
        auto gboard_drive = [&](int idx, const char* kind, int number) {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            const bool same =
                (cfg.drive_type == namzero::kDriveTypeNames[idx] && cfg.drive_on);
            cfg.drive_type = namzero::kDriveTypeNames[idx];
            drive_ptr->set_type(drive_type_enum(idx));
            cfg.drive_on = !same;
            if (i_drive != kNoSlot) chain.set_enabled(i_drive, cfg.drive_on);
            apply_leds();
            std::printf("  drive: %s %s (%s%d)\n", cfg.drive_type.c_str(),
                        cfg.drive_on ? "on" : "off", kind, number);
            std::fflush(stdout);
        };
        auto gboard_boost = [&](const char* kind, int number) {
            std::lock_guard<std::mutex> lk(ctrl_mtx);
            g_boost_on = !g_boost_on;
            update_master();
            apply_leds();
            std::printf("  boost: %s (%s%d)\n", g_boost_on ? "on" : "off", kind, number);
            std::fflush(stdout);
        };

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
        int ae_seen = ae_rev.load();

        // Il wah vive nel blocco FILTER, che e' separato dal mod: l'auto-engage
        // accende e spegne solo quello, senza toccare il tipo di modulazione.
        while (g_running.load()) {
            if (ae_rev.load(std::memory_order_relaxed) != ae_seen) {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                ae.configure(static_cast<float>(midi_cfg.wah_threshold),
                             midi_cfg.wah_engage_ms, midi_cfg.wah_release_ms,
                             static_cast<float>(midi_cfg.wah_release_threshold));
                ae_seen = ae_rev.load(std::memory_order_relaxed);
            }
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
                    {
                        std::lock_guard<std::mutex> lk(ctrl_mtx);
                        cfg.filter_position = pos;
                        if (filter_ptr != nullptr) filter_ptr->set_position(pos);
                    }
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

                // G-Board configurata per mandare CC (50-57): fronte di
                // salita = una pressione, il rilascio (0) non fa nulla. Il
                // debounce e' dedicato e in modo edge, cosi' non dipende da
                // `switches` (che descrive gli switch a stato dell'XTONE).
                if (is_cc) {
                    int mod_idx = -1;
                    int drive_idx = -1;
                    for (int i = 0; i < 4; ++i) {
                        if (midi_cfg.gboard_mod_cc[i] == e.cc) mod_idx = i;
                    }
                    for (int i = 0; i < 3; ++i) {
                        if (midi_cfg.gboard_drive_cc[i] == e.cc) drive_idx = i;
                    }
                    const bool is_gboard_boost =
                        midi_cfg.gboard_boost_cc >= 0 && e.cc == midi_cfg.gboard_boost_cc;
                    if (mod_idx >= 0 || drive_idx >= 0 || is_gboard_boost) {
                        if (gboard_switches.pressed(e.cc, e.value, now_ms)) {
                            if (mod_idx >= 0) {
                                gboard_mod(mod_idx, "CC", e.cc);
                            } else if (drive_idx >= 0) {
                                gboard_drive(drive_idx, "CC", e.cc);
                            } else {
                                gboard_boost("CC", e.cc);
                            }
                        }
                        continue;
                    }
                }

                if ((e.status & 0xF0) == 0xC0) {
                    // G-Board in Program mode: i PC 0-7 selezionano gli slot
                    // 0-7 con lo stesso percorso della tastiera. Il routing e'
                    // in route_gboard_pc: preset_pc ha la priorita' su
                    // pc_effects, quindi un PC non fa mai due cose. Restano
                    // one-shot (nessun debounce: un PC = una pressione).
                    const auto route = namzero::route_gboard_pc(midi_cfg, e.cc);
                    if (route.action == namzero::GboardPcAction::kPreset) {
                        switch_slot.store(route.index);
                        current_slot.store(route.index);
                        std::printf("  preset: slot %d (PC)\n", route.index);
                        std::fflush(stdout);
                    } else if (route.action == namzero::GboardPcAction::kMod) {
                        gboard_mod(route.index, "PC", e.cc);
                    } else if (route.action == namzero::GboardPcAction::kDrive) {
                        gboard_drive(route.index, "PC", e.cc);
                    } else if (route.action == namzero::GboardPcAction::kBoost) {
                        gboard_boost("PC", e.cc);
                    }
                    continue;
                }

                if (!is_cc || is_expr) continue;

                // Looper: gruppo rosso dell'XTONE.
                if (looper_ptr != nullptr) {
                    namzero::LooperSlot::Command lc = namzero::LooperSlot::Command::kNone;
                    if (midi_cfg.looper_toggle_cc >= 0 && e.cc == midi_cfg.looper_toggle_cc)
                        lc = namzero::LooperSlot::Command::kToggle;
                    else if (midi_cfg.looper_stop_cc >= 0 && e.cc == midi_cfg.looper_stop_cc)
                        lc = namzero::LooperSlot::Command::kStop;
                    else if (midi_cfg.looper_clear_cc >= 0 && e.cc == midi_cfg.looper_clear_cc)
                        lc = namzero::LooperSlot::Command::kClear;
                    if (lc != namzero::LooperSlot::Command::kNone) {
                        looper_ptr->command(lc);
                        std::printf("  looper: CC%d\n", e.cc);
                        std::fflush(stdout);
                        continue;
                    }
                }

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
                    g_boost_on = !g_boost_on;
                    update_master();
                    apply_leds();
                    if (g_boost_on) {
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
                    // ripremere lo stesso tasto lo spegne. Il filter non c'entra:
                    // vive in un altro slot e lo comanda solo l'espressione.
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
                    std::lock_guard<std::mutex> lk(ctrl_mtx);
                    // Il filter e' un altro slot: il mod cambia solo on/off,
                    // senza sovrascrivere il tipo scelto dall'utente.
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
                cfg.filter_on = true;
                chain.set_enabled(i_filter, true);
                std::printf("  filter: %s acceso dal pedale\n",
                            filter_ptr != nullptr &&
                                    filter_ptr->type() == namzero::ModSlot::Type::kWhammy
                                ? "whammy"
                                : "wah");
                std::fflush(stdout);
            } else if (transition == -1) {
                std::lock_guard<std::mutex> lk(ctrl_mtx);
                cfg.filter_on = false;
                chain.set_enabled(i_filter, false);
                std::printf("  filter: %s spento dal pedale\n",
                            filter_ptr != nullptr &&
                                    filter_ptr->type() == namzero::ModSlot::Type::kWhammy
                                ? "whammy"
                                : "wah");
                std::fflush(stdout);
            }
        }
    });

    // Muto dell'accordatore: l'uscita va a zero (e torna) con una rampa
    // esponenziale da ~4 ms, abbastanza breve da non sentire il click ma non
    // istantanea. L'analisi resta sul DRY, quindi funziona anche mutati.
    const float mute_coef =
        1.0f - std::exp(-1.0f / (0.004f * static_cast<float>(io.rate())));
    float mute_gain = 1.0f;

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

        // Copia DRY: PRIMA della catena, cosi' il tuner vede la chitarra e non
        // il segnale processato (distorsione, delay, riverbero...).
        if (tuner_on.load(std::memory_order_relaxed)) tuner_tap.push(buf.data(), frames);

        timespec t0{};
        timespec t1{};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        chain.process(buf.data(), frames);
        const float mg = master_gain.load(std::memory_order_relaxed);
        const float mute_target = tuner_muted.load(std::memory_order_relaxed) ? 0.0f : 1.0f;
        for (std::size_t i = 0; i < samples; ++i) {
            mute_gain += (mute_target - mute_gain) * mute_coef;
            if (std::fabs(mute_target - mute_gain) < 1e-4f) mute_gain = mute_target;
            buf[i] = soft_clip(buf[i] * mg) * mute_gain;
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

    // Se si arriva qui con g_running ancora vero il loop audio e' uscito per un
    // errore (device perso, read/write fallita) oppure il re-exec non e'
    // riuscito. Uscire con 0 farebbe credere a systemd (Restart=on-failure) che
    // va tutto bene: il servizio resterebbe morto. Codice 1 = da riavviare.
    const bool abnormal_stop = g_running.load();
    g_running.store(false);
    stats_stop.store(true);
    tuner_stop.store(true);
    if (stats_thread.joinable()) stats_thread.join();
    if (kb_thread.joinable()) kb_thread.join();
    if (midi_thread.joinable()) midi_thread.join();
    if (ctl_thread.joinable()) ctl_thread.join();
    if (tuner_thread.joinable()) tuner_thread.join();
    for (auto& m : midis) m->close();

    io.close();

    std::printf("\nriepilogo: %" PRIu64 " blocchi, %" PRIu64 " xrun\n",
                stats.blocks.load(), stats.xruns.load());
    return abnormal_stop ? 1 : 0;
}
