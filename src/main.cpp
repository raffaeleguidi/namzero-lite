#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdio>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

namespace {

std::atomic<bool> g_running{true};

void on_signal(int /*sig*/) { g_running.store(false); }

struct Options {
    std::string device = "hw:XTONE";
    unsigned rate = 48000;
    unsigned buffer = 64;
    unsigned periods = 3;
    float gain_db = 0.0f;
    std::string ir_path;
    unsigned ir_taps = 2048;
    std::string nam_path;
    double nam_slim = 0.49;
    float amp_gain_db = 0.0f;
    float drive_amount = 0.15f;
    float drive_tone = 0.5f;
    float drive_level_db = 0.0f;
    bool stats = false;
    bool reverb = false;
    float reverb_mix = 0.25f;
    float reverb_size = 0.55f;
    bool gate = false;
    bool comp = false;
    bool drive = false;
    bool realtime = true;
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
        "Uso: namzero-lite [opzioni]\n"
        "  --device <hw:ID>    PCM ALSA (default hw:XTONE)\n"
        "  --rate <hz>         sample rate (default 48000)\n"
        "  --buffer <frame>    frame per periodo (default 64)\n"
        "  --periods <n>       periodi nel buffer (default 3)\n"
        "  --gain <db>         gain master (default 0)\n"
        "  --ir <file.wav>     carica una IR (48 kHz, mono o stereo)\n"
        "  --taps <n>          limite di tap della IR (default 2048)\n"
        "  --nam <file.nam>    carica un modello NAM nello slot amp\n"
        "  --nam-slim <0..1>   sottomodello (default 0.49 = lite)\n"
        "  --amp-gain <db>     trim sullo slot amp (default 0)\n"
        "  --drive-amount <0..1> quantita' di drive (default 0.15)\n"
        "  --drive-tone <0..1>   tono del drive (default 0.5)\n"
        "  --stats               statistiche ogni 5 s (default: solo problemi)\n"
        "  --reverb              attiva il riverbero\n"
        "  --reverb-mix <0..1>   quantita' di riverbero (default 0.25)\n"
        "  --reverb-size <0..1>  dimensione della stanza (default 0.55)\n"
        "  --gate              attiva il noise gate\n"
        "  --comp              attiva il compressore\n"
        "  --drive             attiva il boost/overdrive\n"
        "  --no-rt             senza priorita' realtime\n"
        "  -h, --help          questo messaggio\n");
}

bool parse_uint(const char* s, unsigned& out) {
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = static_cast<unsigned>(v);
    return true;
}

bool parse_float(const char* s, float& out) {
    errno = 0;
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0') return false;
    out = static_cast<float>(v);
    return true;
}

bool parse_args(int argc, char** argv, Options& opt, std::string& err) {
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
            opt.help = true;
        } else if (a == "--device") {
            const char* v = next("--device");
            if (v == nullptr) return false;
            opt.device = v;
        } else if (a == "--rate") {
            const char* v = next("--rate");
            if (v == nullptr || !parse_uint(v, opt.rate)) {
                err = "valore non valido per --rate";
                return false;
            }
        } else if (a == "--buffer") {
            const char* v = next("--buffer");
            if (v == nullptr || !parse_uint(v, opt.buffer)) {
                err = "valore non valido per --buffer";
                return false;
            }
        } else if (a == "--periods") {
            const char* v = next("--periods");
            if (v == nullptr || !parse_uint(v, opt.periods)) {
                err = "valore non valido per --periods";
                return false;
            }
        } else if (a == "--gain") {
            const char* v = next("--gain");
            if (v == nullptr || !parse_float(v, opt.gain_db)) {
                err = "valore non valido per --gain";
                return false;
            }
        } else if (a == "--ir") {
            const char* v = next("--ir");
            if (v == nullptr) return false;
            opt.ir_path = v;
        } else if (a == "--taps") {
            const char* v = next("--taps");
            if (v == nullptr || !parse_uint(v, opt.ir_taps)) {
                err = "valore non valido per --taps";
                return false;
            }
        } else if (a == "--nam") {
            const char* v = next("--nam");
            if (v == nullptr) return false;
            opt.nam_path = v;
        } else if (a == "--nam-slim") {
            const char* v = next("--nam-slim");
            if (v == nullptr) {
                err = "manca il valore per --nam-slim";
                return false;
            }
            float tmp = 0.0f;
            if (!parse_float(v, tmp)) {
                err = "valore non valido per --nam-slim";
                return false;
            }
            opt.nam_slim = tmp;
        } else if (a == "--amp-gain") {
            const char* v = next("--amp-gain");
            if (v == nullptr || !parse_float(v, opt.amp_gain_db)) {
                err = "valore non valido per --amp-gain";
                return false;
            }
        } else if (a == "--drive-amount") {
            const char* v = next("--drive-amount");
            if (v == nullptr || !parse_float(v, opt.drive_amount)) {
                err = "valore non valido per --drive-amount";
                return false;
            }
        } else if (a == "--drive-tone") {
            const char* v = next("--drive-tone");
            if (v == nullptr || !parse_float(v, opt.drive_tone)) {
                err = "valore non valido per --drive-tone";
                return false;
            }
        } else if (a == "--stats") {
            opt.stats = true;
        } else if (a == "--reverb") {
            opt.reverb = true;
        } else if (a == "--reverb-mix") {
            const char* v = next("--reverb-mix");
            if (v == nullptr || !parse_float(v, opt.reverb_mix)) {
                err = "valore non valido per --reverb-mix";
                return false;
            }
        } else if (a == "--reverb-size") {
            const char* v = next("--reverb-size");
            if (v == nullptr || !parse_float(v, opt.reverb_size)) {
                err = "valore non valido per --reverb-size";
                return false;
            }
        } else if (a == "--gate") {
            opt.gate = true;
        } else if (a == "--comp") {
            opt.comp = true;
        } else if (a == "--drive") {
            opt.drive = true;
        } else if (a == "--no-rt") {
            opt.realtime = false;
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
    Options opt;
    std::string err;

    if (!parse_args(argc, argv, opt, err)) {
        std::fprintf(stderr, "errore: %s\n\n", err.c_str());
        usage();
        return 2;
    }
    if (opt.help) {
        usage();
        return 0;
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

    namzero::AudioConfig cfg;
    cfg.device = opt.device;
    cfg.rate = opt.rate;
    cfg.period_frames = opt.buffer;
    cfg.periods = opt.periods;

    namzero::AlsaDuplex io;
    if (!io.open(cfg, err)) {
        std::fprintf(stderr, "audio: %s\n", err.c_str());
        return 1;
    }
    std::printf("audio: %s | %u Hz | periodo %u frame | buffer %u frame | %u periodi\n",
                io.device().c_str(), io.rate(), io.period_frames(),
                io.buffer_frames(), io.periods());

    // Ordine della catena: gate -> comp -> drive -> amp -> ir -> delay -> reverb.
    // Gli indici si memorizzano man mano: gli slot opzionali sposterebbero gli altri.
    namzero::Chain chain;

    const std::size_t i_gate = chain.size();
    chain.add(std::make_unique<namzero::GateSlot>());
    const std::size_t i_comp = chain.size();
    chain.add(std::make_unique<namzero::CompSlot>());
    const std::size_t i_drive = chain.size();
    auto drive_slot = std::make_unique<namzero::DriveSlot>();
    drive_slot->set_drive(opt.drive_amount);
    drive_slot->set_tone(opt.drive_tone);
    drive_slot->set_level_db(opt.drive_level_db);
    chain.add(std::move(drive_slot));

    const std::size_t i_amp = chain.size();
    if (!opt.nam_path.empty()) {
        auto amp = std::make_unique<namzero::NamSlot>();
        if (!amp->load(opt.nam_path, opt.nam_slim, err)) {
            std::fprintf(stderr, "amp: %s\n", err.c_str());
            return 1;
        }
        amp->set_trim_db(opt.amp_gain_db);
        std::printf("amp: %s | slim %.2f%s | gain modello %.2f | %.0f Hz\n",
                    opt.nam_path.c_str(), opt.nam_slim,
                    amp->slimmable() ? "" : " (non slimmable)", amp->model_gain(),
                    amp->expected_sample_rate());
        chain.add(std::move(amp));
    } else {
        chain.add(std::make_unique<namzero::GainSlot>(0.0f));
    }

    std::size_t i_ir = 0;
    if (!opt.ir_path.empty()) {
        i_ir = chain.size();
        auto ir = std::make_unique<namzero::IrSlot>();
        if (!ir->load(opt.ir_path, opt.ir_taps, err)) {
            std::fprintf(stderr, "IR: %s\n", err.c_str());
            return 1;
        }
        std::printf("IR: %s | %zu tap (%.1f ms @ 48 kHz)\n", opt.ir_path.c_str(),
                    ir->taps(), static_cast<double>(ir->taps()) * 1000.0 / 48000.0);
        chain.add(std::move(ir));
    }

    std::size_t i_reverb = 0;
    if (opt.reverb) {
        i_reverb = chain.size();
        auto rv = std::make_unique<namzero::ReverbSlot>();
        rv->set_wet(opt.reverb_mix);
        rv->set_room_size(opt.reverb_size);
        chain.add(std::move(rv));
    }

    chain.prepare(io.rate(), io.period_frames());

    chain.set_enabled(i_gate, opt.gate);
    chain.set_enabled(i_comp, opt.comp);
    chain.set_enabled(i_drive, opt.drive);
    chain.set_enabled(i_amp, !opt.nam_path.empty());
    if (i_ir != 0) chain.set_enabled(i_ir, true);
    if (i_reverb != 0) chain.set_enabled(i_reverb, true);
    chain.reset();

    std::printf("slot: gate %s | comp %s | drive %s | amp %s | ir %s | reverb %s\n",
                opt.gate ? "on" : "off", opt.comp ? "on" : "off",
                opt.drive ? "on" : "off", opt.nam_path.empty() ? "off" : "on",
                opt.ir_path.empty() ? "off" : "on", opt.reverb ? "on" : "off");

    const std::size_t latency = chain.latency_frames();
    std::printf("latenza catena: %zu campioni (%.2f ms)\n", latency,
                static_cast<double>(latency) * 1000.0 / 48000.0);

    const std::size_t frames = io.period_frames();
    const std::size_t samples = frames * 2;

    std::vector<int32_t> in_i32(samples, 0);
    std::vector<int32_t> out_i32(samples, 0);
    std::vector<float> buf(samples, 0.0f);

    // Buffer di playback pieno a due periodi prima di partire.
    if (!io.prime_playback(2 * io.period_frames())) {
        std::fprintf(stderr,
                     "attenzione: priming del playback fallito, "
                     "possibile underrun iniziale\n");
    }

    if (opt.realtime) {
        if (!namzero::rt::install(err)) {
            std::fprintf(stderr,
                         "realtime non disponibile: %s\n"
                         "proseguo senza FIFO: usa sudo, oppure --no-rt\n",
                         err.c_str());
        }
    }

    Stats stats;
    std::atomic<bool> stats_stop{false};

    // In modalita' normale il thread tace: parla solo se qualcosa non va.
    // Le righe periodiche sono utili in sviluppo e moleste quando suoni.
    const double period_us =
        static_cast<double>(io.period_frames()) * 1e6 / static_cast<double>(io.rate());
    const uint64_t warn_us = static_cast<uint64_t>(period_us * 0.85);

    std::thread stats_thread([&stats, &stats_stop, &opt, period_us, warn_us]() {
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

            if (opt.stats) {
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

    const float master = std::pow(10.0f, opt.gain_db / 20.0f);

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
