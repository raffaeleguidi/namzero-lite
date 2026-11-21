#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

#include "dsp/slot.h"

namespace namzero {

// Looper a una traccia, in fondo alla catena.
// Il buffer e' allocato in prepare() (mai nel thread audio); il controllo
// (tastiera/MIDI/web) deposita un comando atomico che il thread audio applica
// all'inizio del blocco. Overdub semplice: somma sul buffer, CLEAR per
// ricominciare (niente undo del layer).
class LooperSlot : public Slot {
public:
    enum class State { kIdle, kRecording, kPlaying, kOverdub };
    enum class Command { kNone, kToggle, kStop, kClear };

    explicit LooperSlot(double max_seconds) : max_seconds_(max_seconds) {}

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        rate_ = sample_rate;
        frames_total_ = static_cast<std::size_t>(max_seconds_ * sample_rate);
        if (frames_total_ < 2) frames_total_ = 2;
        buf_.assign(frames_total_ * 2, 0.0f);
        pos_ = 0;
        loop_len_ = 0;
        state_.store(static_cast<int>(State::kIdle));
        cmd_.store(static_cast<int>(Command::kNone));
        publish();
    }

    void reset() override {
        pos_ = 0;
        loop_len_ = 0;
        state_.store(static_cast<int>(State::kIdle));
        publish();
    }

    const char* name() const override { return "looper"; }

    void command(Command c) { cmd_.store(static_cast<int>(c)); }

    State state() const { return static_cast<State>(state_.load()); }
    double position_seconds() const { return static_cast<double>(pos_shared_.load()) / rate_; }
    double length_seconds() const { return static_cast<double>(len_shared_.load()) / rate_; }
    double max_seconds() const { return max_seconds_; }

    static const char* state_name(State s) {
        switch (s) {
            case State::kRecording: return "rec";
            case State::kPlaying: return "play";
            case State::kOverdub: return "dub";
            default: return "idle";
        }
    }

    void process(float* io, std::size_t frames) override {
        const Command c = static_cast<Command>(cmd_.exchange(0));
        if (c != Command::kNone) apply(c);

        State st = state();
        for (std::size_t i = 0; i < frames; ++i) {
            float& l = io[2 * i];
            float& r = io[2 * i + 1];
            if (st == State::kRecording) {
                buf_[2 * pos_] = l;
                buf_[2 * pos_ + 1] = r;
                ++pos_;
                if (pos_ >= frames_total_) {
                    loop_len_ = frames_total_;
                    pos_ = 0;
                    st = State::kPlaying;
                    state_.store(static_cast<int>(st));
                }
            } else if (st == State::kPlaying && loop_len_ > 0) {
                l += buf_[2 * pos_];
                r += buf_[2 * pos_ + 1];
                pos_ = (pos_ + 1) % loop_len_;
            } else if (st == State::kOverdub && loop_len_ > 0) {
                const float old_l = buf_[2 * pos_];
                const float old_r = buf_[2 * pos_ + 1];
                buf_[2 * pos_] = old_l + l * 0.9f;
                buf_[2 * pos_ + 1] = old_r + r * 0.9f;
                l += old_l;
                r += old_r;
                pos_ = (pos_ + 1) % loop_len_;
            }
        }
        publish();
    }

private:
    void publish() {
        pos_shared_.store(static_cast<long long>(pos_), std::memory_order_relaxed);
        len_shared_.store(static_cast<long long>(loop_len_), std::memory_order_relaxed);
    }

    void apply(Command c) {
        State st = state();
        if (c == Command::kStop) {
            pos_ = 0;
            state_.store(static_cast<int>(State::kIdle));
        } else if (c == Command::kClear) {
            pos_ = 0;
            loop_len_ = 0;
            state_.store(static_cast<int>(State::kIdle));
        } else if (c == Command::kToggle) {
            if (st == State::kIdle) {
                pos_ = 0;
                loop_len_ = 0;
                state_.store(static_cast<int>(State::kRecording));
            } else if (st == State::kRecording) {
                loop_len_ = pos_ > 0 ? pos_ : 1;
                pos_ = 0;
                state_.store(static_cast<int>(State::kPlaying));
            } else if (st == State::kPlaying) {
                pos_ = 0;
                state_.store(static_cast<int>(State::kOverdub));
            } else {
                pos_ = 0;
                state_.store(static_cast<int>(State::kPlaying));
            }
        }
    }

    std::vector<float> buf_;
    std::size_t frames_total_ = 0;
    std::size_t pos_ = 0;
    std::size_t loop_len_ = 0;
    double rate_ = 48000.0;
    double max_seconds_ = 60.0;
    std::atomic<int> state_{0};
    std::atomic<int> cmd_{0};
    std::atomic<long long> pos_shared_{0};
    std::atomic<long long> len_shared_{0};
};

}  // namespace namzero
