#include "dsp/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace namzero {
namespace {

uint32_t le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t le16(const unsigned char* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

// 24 bit little endian, con estensione di segno a 32.
int32_t s24(const unsigned char* p) {
    const int32_t v = (static_cast<int32_t>(p[0]) << 8) |
                      (static_cast<int32_t>(p[1]) << 16) |
                      (static_cast<int32_t>(p[2]) << 24);
    return v >> 8;
}

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

}  // namespace

AudioFile load_wav(const std::string& path) {
    AudioFile out;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        out.error = "impossibile aprire " + path;
        return out;
    }

    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (data.size() < 44) {
        out.error = "file troppo corto";
        return out;
    }
    if (std::memcmp(data.data(), "RIFF", 4) != 0 ||
        std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        out.error = "non e' un WAV RIFF";
        return out;
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint32_t rate = 0;
    uint16_t bits = 0;
    bool have_fmt = false;

    std::size_t pos = 12;
    const unsigned char* audio = nullptr;
    std::size_t audio_size = 0;

    while (pos + 8 <= data.size()) {
        const unsigned char* chunk = data.data() + pos;
        const uint32_t size = le32(chunk + 4);
        const std::size_t body = pos + 8;

        if (std::memcmp(chunk, "fmt ", 4) == 0 && body + 16 <= data.size()) {
            const unsigned char* f = data.data() + body;
            format = le16(f + 0);
            channels = le16(f + 2);
            rate = le32(f + 4);
            bits = le16(f + 14);
            if (format == kFormatExtensible && size >= 40 && body + 26 <= data.size()) {
                // I primi due byte del sub-format coincidono con il formato vero.
                format = le16(f + 24);
            }
            have_fmt = true;
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            audio = data.data() + body;
            audio_size = std::min<std::size_t>(size, data.size() - body);
        }

        // I chunk sono allineati a 2 byte.
        pos = body + size + (size & 1u);
    }

    if (!have_fmt) {
        out.error = "chunk fmt assente";
        return out;
    }
    if (audio == nullptr || audio_size == 0) {
        out.error = "chunk data assente o vuoto";
        return out;
    }
    if (channels == 0) {
        out.error = "numero di canali nullo";
        return out;
    }

    const unsigned bytes_per_sample = bits / 8u;
    if (bytes_per_sample == 0) {
        out.error = "bits per sample non validi";
        return out;
    }

    const bool is_float = (format == kFormatFloat);
    const bool is_pcm = (format == kFormatPcm);
    if (!is_float && !is_pcm) {
        out.error = "formato non supportato (solo PCM e float)";
        return out;
    }

    const std::size_t frame_bytes = bytes_per_sample * channels;
    if (frame_bytes == 0) {
        out.error = "frame nullo";
        return out;
    }
    const std::size_t frames = audio_size / frame_bytes;
    if (frames == 0) {
        out.error = "nessun campione";
        return out;
    }

    out.samples.resize(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        for (unsigned c = 0; c < channels; ++c) {
            const unsigned char* p = audio + i * frame_bytes + c * bytes_per_sample;
            float v = 0.0f;
            if (is_float) {
                if (bits == 32) {
                    uint32_t u = le32(p);
                    float f;
                    std::memcpy(&f, &u, sizeof(f));
                    v = f;
                } else if (bits == 64) {
                    uint64_t u = static_cast<uint64_t>(le32(p)) |
                                 (static_cast<uint64_t>(le32(p + 4)) << 32);
                    double d;
                    std::memcpy(&d, &u, sizeof(d));
                    v = static_cast<float>(d);
                } else {
                    out.error = "float con bits non supportati";
                    return out;
                }
            } else {
                switch (bits) {
                    case 8:
                        v = (static_cast<int>(p[0]) - 128) / 128.0f;
                        break;
                    case 16:
                        v = static_cast<int16_t>(le16(p)) / 32768.0f;
                        break;
                    case 24:
                        v = s24(p) / 8388608.0f;
                        break;
                    case 32:
                        v = static_cast<float>(static_cast<int32_t>(le32(p))) /
                            2147483648.0f;
                        break;
                    default:
                        out.error = "PCM con bits non supportati";
                        return out;
                }
            }
            out.samples[i * channels + c] = v;
        }
    }

    out.ok = true;
    out.sample_rate = rate;
    out.channels = channels;
    return out;
}

AudioFile load_ir_mono(const std::string& path) {
    AudioFile f = load_wav(path);
    if (!f.ok) return f;

    if (f.channels > 1) {
        const std::size_t frames = f.samples.size() / f.channels;
        std::vector<float> mono(frames, 0.0f);
        for (std::size_t i = 0; i < frames; ++i) {
            float acc = 0.0f;
            for (unsigned c = 0; c < f.channels; ++c) {
                acc += f.samples[i * f.channels + c];
            }
            mono[i] = acc / static_cast<float>(f.channels);
        }
        f.samples.swap(mono);
        f.channels = 1;
    }

    // Normalizza sul picco: una IR serve per la forma, non per il livello.
    float peak = 0.0f;
    for (float v : f.samples) peak = std::max(peak, std::fabs(v));
    if (peak > 0.0f) {
        const float g = 1.0f / peak;
        for (float& v : f.samples) v *= g;
    }

    return f;
}

}  // namespace namzero
