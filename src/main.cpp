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
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
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
#include "dsp/reverb_slot.h"
#include "json.hpp"

namespace {

std::atomic<bool> g_running{true};

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
    double drive_amount = 0.15;
    double drive_tone = 0.5;
    double drive_level_db = 0.0;

    bool amp_on = false;
    std::string amp_file;
    double amp_slim = 0.49;
    double amp_trim_db = 0.0;

    bool ir_on = false;
    std::string ir_file;
    unsigned ir_taps = 2048;

    bool reverb_on = false;
    double reverb_mix = 0.25;
    double reverb_size = 0.55;
    double reverb_damp = 0.45;

    // comportamento
    bool stats = false;
    bool help = false;
};

struct Stats {
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> xruns{0};
    std::atomic<uint64_t> max_us{0};
};

void usage() {
    std::puts(
        "namzero-lite — motore audio realtime per chitarra\n"
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
        "  --comp               compressore\n"
        "  --drive              boost/overdrive\n"
        "  --drive-amount <0..1> quantita' di drive (default 0.15)\n"
        "  --drive-tone <0..1>  tono del drive (default 0.5)\n"
        "  --reverb             riverbero\n"
        "  --reverb-mix <0..1>  quantita' (default 0.25)\n"
        "  --reverb-size <0..1> stanza (default 0.55)\n"
        "  --stats              statistiche ogni 5 s (default: solo problemi)\n"
        "  -h, --help           questo messaggio\n");
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
        cfg.drive_amount = get_num(d, "amount", cfg.drive_amount);
        cfg.drive_tone = get_num(d, "tone", cfg.drive_tone);
        cfg.drive_level_db = get_num(d, "level_db", cfg.drive_level_db);
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

    auto rv = slot_or_empty(j, "reverb");
    if (!rv.empty()) {
        cfg.reverb_on = get_bool(rv, "on", cfg.reverb_on);
        cfg.reverb_mix = get_num(rv, "mix", cfg.reverb_mix);
        cfg.reverb_size = get_num(rv, "size", cfg.reverb_size);
        cfg.reverb_damp = get_num(rv, "damp", cfg.reverb_damp);
    }

    if (j.contains("stats") && j["stats"].is_boolean()) {
        cfg.stats = j["stats"].get<bool>();
    }
    return true;
}

// --- riga di comando ------------------------------------------------------

bool parse_args(int argc, char** argv, Config& cfg, std::string& preset_path,
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
        } else if (a == "--gate") {
            cfg.gate_on = true;
        } else if (a == "--comp") {
            cfg.comp_on = true;
        } else if (a == "--drive") {
            cfg.drive_on = true;
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
    std::string preset_path;
    std::string err;
    bool saw_flag = false;

    if (!parse_args(argc, argv, cfg, preset_path, saw_flag, err)) {
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
        cfg = from_file;
        std::printf("preset: %s\n", preset_path.c_str());
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // Istanza singola: il device audio e' esclusivo, e un residuo in background
    // fa fallire l'avvio con un "device busy" che sembra un problema di config.
    const int lock_fd = ::open("/tmp/namzero-lite.lock", O_CREAT | O_RDWR, 0666);
    if (lock_fd >= 0 && ::flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        std::fprintf(stderr,
                     "errore: un'altra istanza di namzero-lite e' gia' in "
                     "esecuzione (lock /tmp/namzero-lite.lock)\n");
        return 1;
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

    // Ordine della catena: gate -> comp -> drive -> amp -> ir -> reverb.
    // Gli indici si memorizzano man mano: gli slot opzionali sposterebbero gli altri.
    namzero::Chain chain;

    const std::size_t i_gate = chain.size();
    chain.add(std::make_unique<namzero::GateSlot>());
    const std::size_t i_comp = chain.size();
    chain.add(std::make_unique<namzero::CompSlot>());
    const std::size_t i_drive = chain.size();
    chain.add(std::make_unique<namzero::DriveSlot>());

    const std::size_t i_amp = chain.size();
    const bool has_amp = cfg.amp_on && !cfg.amp_file.empty();
    if (has_amp) {
        auto amp = std::make_unique<namzero::NamSlot>();
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

    std::size_t i_ir = 0;
    const bool has_ir = cfg.ir_on && !cfg.ir_file.empty();
    if (has_ir) {
        i_ir = chain.size();
        auto ir = std::make_unique<namzero::IrSlot>();
        if (!ir->load(cfg.ir_file, cfg.ir_taps, err)) {
            std::fprintf(stderr, "IR: %s\n", err.c_str());
            return 1;
        }
        std::printf("IR: %s | %zu tap (%.1f ms @ 48 kHz)\n", cfg.ir_file.c_str(),
                    ir->taps(), static_cast<double>(ir->taps()) * 1000.0 / 48000.0);
        chain.add(std::move(ir));
    }

    std::size_t i_reverb = 0;
    if (cfg.reverb_on) {
        i_reverb = chain.size();
        auto rv = std::make_unique<namzero::ReverbSlot>();
        rv->set_wet(static_cast<float>(cfg.reverb_mix));
        rv->set_room_size(static_cast<float>(cfg.reverb_size));
        rv->set_damp(static_cast<float>(cfg.reverb_damp));
        chain.add(std::move(rv));
    }

    chain.prepare(io.rate(), io.period_frames());

    chain.set_enabled(i_gate, cfg.gate_on);
    chain.set_enabled(i_comp, cfg.comp_on);
    chain.set_enabled(i_drive, cfg.drive_on);
    chain.set_enabled(i_amp, has_amp);
    if (i_ir != 0) chain.set_enabled(i_ir, true);
    if (i_reverb != 0) chain.set_enabled(i_reverb, true);
    chain.reset();

    std::printf("slot: gate %s | comp %s | drive %s | amp %s | ir %s | reverb %s\n",
                cfg.gate_on ? "on" : "off", cfg.comp_on ? "on" : "off",
                cfg.drive_on ? "on" : "off", has_amp ? "on" : "off",
                has_ir ? "on" : "off", cfg.reverb_on ? "on" : "off");
    const std::size_t latency = chain.latency_frames();
    std::printf("latenza catena: %zu campioni (%.2f ms)\n", latency,
                static_cast<double>(latency) * 1000.0 / 48000.0);

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
        std::fprintf(stderr, "realtime non disponibile: %s\n", err.c_str());
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

    io.close();

    std::printf("\nriepilogo: %" PRIu64 " blocchi, %" PRIu64 " xrun\n",
                stats.blocks.load(), stats.xruns.load());
    return 0;
}
