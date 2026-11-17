#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/alsa_io.h"
#include "audio/rt.h"
#include "dsp/chain.h"
#include "dsp/gain_slot.h"

namespace {

std::atomic<bool> g_running{true};

void on_signal(int /*sig*/) { g_running.store(false); }

struct Options {
    std::string device = "hw:XTONE";
    unsigned rate = 48000;
    unsigned buffer = 64;
    unsigned periods = 3;
    float gain_db = 0.0f;
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
    std::printf("audio: %s | %u Hz | %u frame | %u periodi\n", io.device().c_str(),
                io.rate(), io.period_frames(), io.periods());

    namzero::Chain chain;
    chain.add(std::make_unique<namzero::GainSlot>(0.0f));
    chain.prepare(io.rate(), io.period_frames());

    const std::size_t frames = io.period_frames();
    const std::size_t samples = frames * 2;

    std::vector<int32_t> in_i32(samples, 0);
    std::vector<int32_t> out_i32(samples, 0);
    std::vector<float> buf(samples, 0.0f);

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

    std::thread stats_thread([&stats, &stats_stop]() {
        uint64_t prev_blocks = 0;
        uint64_t prev_xruns = 0;
        while (g_running.load() && !stats_stop.load()) {
            for (int i = 0; i < 50 && g_running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            const uint64_t blocks = stats.blocks.load();
            const uint64_t xruns = stats.xruns.load();
            const uint64_t max_us = stats.max_us.exchange(0);
            std::printf("blocchi %" PRIu64 " (+%" PRIu64 ") | xrun %" PRIu64
                        " (+%" PRIu64 ") | max blocco %.2f ms\n",
                        blocks, blocks - prev_blocks, xruns, xruns - prev_xruns,
                        static_cast<double>(max_us) / 1000.0);
            std::fflush(stdout);
            prev_blocks = blocks;
            prev_xruns = xruns;
        }
    });

    const float master = std::pow(10.0f, opt.gain_db / 20.0f);

    while (g_running.load()) {
        const long n = io.read_i32(in_i32.data(), frames);
        if (n < 0) {
            if (io.recover(static_cast<int>(n))) {
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
            if (io.recover(static_cast<int>(w))) {
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
