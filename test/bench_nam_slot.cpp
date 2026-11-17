// Misura lo slot amp in isolamento: carica il modello, applica il slim e
// processa 2 secondi di audio a blocchi di 64, come fa benchmodel.
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "dsp/nam_slot.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("uso: %s <modello.nam> [slim]\n", argv[0]);
        return 2;
    }
    const double slim = (argc > 2) ? std::stod(argv[2]) : 0.49;

    namzero::NamSlot amp;
    std::string err;
    if (!amp.load(argv[1], slim, err)) {
        std::printf("errore: %s\n", err.c_str());
        return 1;
    }
    std::printf("modello: %s | slim %.2f | slimmable %s | %.0f Hz\n", argv[1], slim,
                amp.slimmable() ? "si" : "no", amp.expected_sample_rate());

    const std::size_t F = 64;
    amp.prepare(amp.expected_sample_rate(), F);

    std::vector<float> buf(F * 2, 0.0f);
    for (std::size_t i = 0; i < F; ++i) {
        buf[i * 2] = 0.1f * (i % 7);
        buf[i * 2 + 1] = buf[i * 2];
    }

    const int blocks = 1500;  // 2 secondi a 48 kHz
    auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b) amp.process(buf.data(), F);
    auto t1 = std::chrono::steady_clock::now();

    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("totale %.0f ms per 2 s -> RTF %.2f | %.3f ms per blocco\n", ms,
                ms / 2000.0, ms / blocks);
    return 0;
}
