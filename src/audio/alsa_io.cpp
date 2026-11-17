#include "audio/alsa_io.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

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

bool AlsaDuplex::open(const AudioConfig& cfg, std::string& err) {
    close();
    device_ = cfg.device;

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

    rate_ = cap_rate;
    period_frames_ = cap_frames;
    periods_ = cap_periods;
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

bool AlsaDuplex::recover(int err) {
    if (err == -EPIPE) {
        if (capture_ != nullptr) snd_pcm_prepare(capture_);
        if (playback_ != nullptr) snd_pcm_prepare(playback_);
        return true;
    }
#ifdef ESTRPIPE
    if (err == -ESTRPIPE) {
        // Device sospeso (tipico con USB autosuspend): attende e riprepara.
        for (int i = 0; i < 100; ++i) {
            if (capture_ != nullptr) {
                int rc = snd_pcm_resume(capture_);
                if (rc == 0) break;
                if (rc != -EAGAIN) break;
            }
            usleep(10000);
        }
        if (capture_ != nullptr) snd_pcm_prepare(capture_);
        if (playback_ != nullptr) snd_pcm_prepare(playback_);
        return true;
    }
#endif
    return false;
}

}  // namespace namzero
