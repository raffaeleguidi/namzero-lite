#pragma once

#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

namespace namzero {

// FFT radix-2 in-place, decimation in time. Semplice e senza dipendenze:
// per le dimensioni che usiamo (128 punti) la differenza con una libreria
// ottimizzata e' irrilevante, e una implementazione locale non porta vincoli.
inline void fft(std::vector<std::complex<float>>& a, bool inverse) {
    const std::size_t n = a.size();
    if (n < 2) return;

    // Riordino per bit reversal.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }

    constexpr float kPi = 3.14159265358979323846f;

    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = 2.0f * kPi / static_cast<float>(len) * (inverse ? 1.0f : -1.0f);
        const std::complex<float> wlen(std::cos(ang), std::sin(ang));

        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            const std::size_t half = len >> 1;
            for (std::size_t j = 0; j < half; ++j) {
                const std::complex<float> u = a[i + j];
                const std::complex<float> v = a[i + j + half] * w;
                a[i + j] = u + v;
                a[i + j + half] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse) {
        const float inv = 1.0f / static_cast<float>(n);
        for (auto& x : a) x *= inv;
    }
}

}  // namespace namzero
