#include "audio/alsa_io.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace namzero {
namespace {

constexpr unsigned kChannels = 2;

std::string describe(const char* what, int rc) {
    std::string s(what);
    s += ": ";
    s += snd_strerror(rc);
    return s;
}

}  // namespace

AlsaDuplex::~AlsaDuplex() { close(); }

void AlsaDuplex::close_one(snd_pcm_t** pcm) {
    if (*pcm != nullptr) {
        snd_pcm_drop(*pcm);
        snd_pcm_close(*pcm);
        *pcm = nullptr;
    }
}

void AlsaDuplex::close() {
    close_one(&capture_);
    close_one(&playback_);
    rate_ = 0;
    period_frames_ = 0;
    periods_ = 0;
    buffer_frames_ = 0;
}

void AlsaDuplex::configure_offline(unsigned rate, unsigned period_frames,
                                   unsigned periods, unsigned input,
                                   unsigned output) {
    close();
    // Il device resta quello configurato: la UI mostra "aperto: no" e il
    // messaggio d'errore, ma il campo device non sparisce.
    rate_ = rate;
    period_frames_ = period_frames;
    periods_ = periods;
    buffer_frames_ = period_frames * periods;
    input_ = clamp_audio_input(input);
    output_ = clamp_audio_output(output);
}

bool AlsaDuplex::open_one(snd_pcm_t** pcm, const char* what,
                          snd_pcm_stream_t stream, std::string& err) {
    int rc = snd_pcm_open(pcm, device_.c_str(), stream, 0);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }
    return true;
}

bool AlsaDuplex::apply_params(snd_pcm_t* pcm, const char* what, unsigned& rate,
                              unsigned& period_frames, unsigned& periods,
                              std::string& err) {
    snd_pcm_hw_params_t* hw;
    snd_pcm_hw_params_alloca(&hw);

    int rc = snd_pcm_hw_params_any(pcm, hw);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }

    rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (rc < 0) {
        err = std::string(what) + ": accesso RW interleaved non supportato";
        return false;
    }

    rc = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    if (rc < 0) {
        err = std::string(what) + ": formato S32_LE non supportato";
        return false;
    }

    rc = snd_pcm_hw_params_set_channels(pcm, hw, kChannels);
    if (rc < 0) {
        err = std::string(what) + ": 2 canali non supportati";
        return false;
    }

    int dir = 0;
    rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, &dir);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }

    dir = 0;
    snd_pcm_uframes_t pf = static_cast<snd_pcm_uframes_t>(period_frames);
    rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &pf, &dir);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }
    if (pf == 0) {
        err = std::string(what) + ": il device ha negoziato un periodo nullo";
        return false;
    }
    period_frames = static_cast<unsigned>(pf);

    dir = 0;
    unsigned pds = periods;
    rc = snd_pcm_hw_params_set_periods_near(pcm, hw, &pds, &dir);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }
    periods = pds;

    rc = snd_pcm_hw_params(pcm, hw);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }
    return true;
}

bool AlsaDuplex::configure_sw(snd_pcm_t* pcm, const char* what,
                              bool capture_side, unsigned period_frames,
                              std::string& err) {
    snd_pcm_sw_params_t* sw;
    snd_pcm_sw_params_alloca(&sw);

    int rc = snd_pcm_sw_params_current(pcm, sw);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }

    snd_pcm_sw_params_set_avail_min(pcm, sw, period_frames);

    if (capture_side) {
        // La capture parte al primo periodo disponibile.
        snd_pcm_sw_params_set_start_threshold(pcm, sw, 1);
    } else {
        // Il playback parte solo con due periodi gia' in buffer: partire "a
        // meta'" e' la causa classica dell'underrun immediato.
        snd_pcm_sw_params_set_start_threshold(pcm, sw, 2u * period_frames);
    }

    rc = snd_pcm_sw_params(pcm, sw);
    if (rc < 0) {
        err = describe(what, rc);
        return false;
    }
    return true;
}

bool AlsaDuplex::prime_playback(std::size_t frames) {
    if (playback_ == nullptr || frames == 0) return false;
    std::vector<int32_t> silence(frames * kChannels, 0);
    const long n = write_i32(silence.data(), frames);
    return n == static_cast<long>(frames);
}

bool AlsaDuplex::open(const AudioConfig& cfg, std::string& err) {
    close();
    device_ = cfg.device;
    input_ = clamp_audio_input(cfg.input);
    output_ = clamp_audio_output(cfg.output);

    if (!open_one(&capture_, "capture", SND_PCM_STREAM_CAPTURE, err)) {
        close();
        return false;
    }
    if (!open_one(&playback_, "playback", SND_PCM_STREAM_PLAYBACK, err)) {
        close();
        return false;
    }

    unsigned cap_rate = cfg.rate;
    unsigned cap_frames = cfg.period_frames;
    unsigned cap_periods = cfg.periods;
    if (!apply_params(capture_, "capture", cap_rate, cap_frames, cap_periods, err)) {
        close();
        return false;
    }

    unsigned pb_rate = cfg.rate;
    unsigned pb_frames = cfg.period_frames;
    unsigned pb_periods = cfg.periods;
    if (!apply_params(playback_, "playback", pb_rate, pb_frames, pb_periods, err)) {
        close();
        return false;
    }

    // Il device e' lo stesso: i parametri devono coincidere.
    if (pb_rate != cap_rate || pb_frames != cap_frames) {
        err = "capture e playback hanno negoziato parametri diversi";
        close();
        return false;
    }

    int rc = snd_pcm_prepare(capture_);
    if (rc < 0) {
        err = describe("prepare capture", rc);
        close();
        return false;
    }
    rc = snd_pcm_prepare(playback_);
    if (rc < 0) {
        err = describe("prepare playback", rc);
        close();
        return false;
    }

    // Parametri REALMENTE negoziati: chiedere 256 non garantisce 256.
    snd_pcm_uframes_t cap_buf = 0, cap_per = 0, pb_buf = 0, pb_per = 0;
    rc = snd_pcm_get_params(capture_, &cap_buf, &cap_per);
    if (rc < 0) {
        err = describe("get_params capture", rc);
        close();
        return false;
    }
    rc = snd_pcm_get_params(playback_, &pb_buf, &pb_per);
    if (rc < 0) {
        err = describe("get_params playback", rc);
        close();
        return false;
    }
    if (cap_per != pb_per || cap_buf != pb_buf) {
        err = "capture e playback hanno buffer/periodo diversi";
        close();
        return false;
    }

    period_frames_ = static_cast<unsigned>(cap_per);
    buffer_frames_ = static_cast<unsigned>(cap_buf);
    periods_ = (period_frames_ != 0) ? (buffer_frames_ / period_frames_) : 0;
    rate_ = cap_rate;

    if (!configure_sw(capture_, "sw capture", true, period_frames_, err)) {
        close();
        return false;
    }
    if (!configure_sw(playback_, "sw playback", false, period_frames_, err)) {
        close();
        return false;
    }
    return true;
}

long AlsaDuplex::read_i32(int32_t* dst, std::size_t frames) {
    std::size_t done = 0;
    while (done < frames) {
        snd_pcm_sframes_t n =
            snd_pcm_readi(capture_, dst + done * kChannels, frames - done);
        if (n < 0) {
            if (n == -EAGAIN) continue;
            return static_cast<long>(n);
        }
        if (n == 0) continue;
        done += static_cast<std::size_t>(n);
    }
    return static_cast<long>(frames);
}

long AlsaDuplex::write_i32(const int32_t* src, std::size_t frames) {
    std::size_t done = 0;
    while (done < frames) {
        snd_pcm_sframes_t n =
            snd_pcm_writei(playback_, src + done * kChannels, frames - done);
        if (n < 0) {
            if (n == -EAGAIN) continue;
            return static_cast<long>(n);
        }
        if (n == 0) continue;
        done += static_cast<std::size_t>(n);
    }
    return static_cast<long>(frames);
}

bool AlsaDuplex::recover(int err, bool capture_side) {
    if (err == -EPIPE) {
        snd_pcm_t* pcm = capture_side ? capture_ : playback_;
        if (pcm != nullptr) snd_pcm_prepare(pcm);
        return true;
    }
#ifdef ESTRPIPE
    if (err == -ESTRPIPE) {
        // Device sospeso (tipico con USB autosuspend): attende e riprepara.
        snd_pcm_t* pcm = capture_side ? capture_ : playback_;
        if (pcm == nullptr) return false;
        for (int i = 0; i < 100; ++i) {
            int rc = snd_pcm_resume(pcm);
            if (rc == 0) break;
            if (rc != -EAGAIN) break;
            usleep(10000);
        }
        snd_pcm_prepare(pcm);
        return true;
    }
#endif
    return false;
}

}  // namespace namzero
