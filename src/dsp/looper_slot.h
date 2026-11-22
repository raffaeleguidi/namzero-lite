#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <vector>

#include "dsp/slot.h"

namespace namzero {

// Looper a una traccia, in fondo alla catena.
// Il buffer e' allocato in prepare() (mai nel thread audio); il controllo
// (tastiera/MIDI/web) deposita un comando atomico che il thread audio applica
// all'inizio del blocco. Overdub semplice: somma sul buffer, CLEAR per
// ricominciare (niente undo del layer).
//
// Autostart (opzione globale, spento di default): con il flag acceso il toggle
// da fermo con loop vuoto NON registra subito, ma mette il looper in ATTESA
// (kArmed, "armed" nello stato). La registrazione parte da sola al primo
// campione del SECONDO blocco consecutivo in cui l'ingresso supera la soglia:
// il doppio blocco evita di innescare su un click isolato.
class LooperSlot : public Slot {
public:
    enum class State { kIdle, kRecording, kPlaying, kOverdub, kArmed };
    enum class Command { kNone, kToggle, kStop, kClear };

    static constexpr double kDefaultMaxSeconds = 60.0;
    static constexpr double kMaxSecondsMin = 1.0;
    static constexpr double kMaxSecondsMax = 120.0;
    static constexpr double kAutostartThresholdMinDb = -90.0;
    static constexpr double kAutostartThresholdMaxDb = 0.0;
    static constexpr double kDefaultAutostartThresholdDb = -42.0;
    // Blocchi consecutivi sopra soglia prima di far partire la registrazione.
    static constexpr int kAutostartTriggerBlocks = 2;

    explicit LooperSlot(double max_seconds = kDefaultMaxSeconds)
        : max_seconds_(std::clamp(max_seconds, kMaxSecondsMin, kMaxSecondsMax)) {}

    void prepare(double sample_rate, std::size_t /*max_frames*/) override {
        rate_ = sample_rate;
        frames_total_ = static_cast<std::size_t>(max_seconds_ * sample_rate);
        if (frames_total_ < 2) frames_total_ = 2;
        buf_.assign(frames_total_ * 2, 0.0f);
        pos_ = 0;
        loop_len_ = 0;
        armed_ = false;
        start_blocks_ = 0;
        state_.store(static_cast<int>(State::kIdle));
        cmd_.store(static_cast<int>(Command::kNone));
        publish();
    }

    void reset() override {
        pos_ = 0;
        loop_len_ = 0;
        armed_ = false;
        start_blocks_ = 0;
        state_.store(static_cast<int>(State::kIdle));
        publish();
    }

    const char* name() const override { return "looper"; }

    void command(Command c) { cmd_.store(static_cast<int>(c)); }

    // Configurazione dell'autostart (opzione globale in presets.json). Si puo'
    // cambiare dal vivo: il thread audio la legge al blocco successivo.
    void set_autostart(bool on) { autostart_.store(on, std::memory_order_relaxed); }
    bool autostart() const { return autostart_.load(std::memory_order_relaxed); }

    void set_autostart_threshold_db(double db) {
        if (!std::isfinite(db)) db = kDefaultAutostartThresholdDb;
        autostart_threshold_db_.store(
            std::clamp(db, kAutostartThresholdMinDb, kAutostartThresholdMaxDb),
            std::memory_order_relaxed);
    }
    double autostart_threshold_db() const {
        return autostart_threshold_db_.load(std::memory_order_relaxed);
    }

    State state() const { return static_cast<State>(state_.load()); }
    // armed() e' SOLO l'overdub quantizzato (in play). L'attesa dell'autostart
    // e' lo stato kArmed, distinto: la UI non li deve confondere.
    bool armed() const { return armed_; }
    double position_seconds() const { return static_cast<double>(pos_shared_.load()) / rate_; }
    double length_seconds() const { return static_cast<double>(len_shared_.load()) / rate_; }
    double max_seconds() const { return max_seconds_; }

    static const char* state_name(State s) {
        switch (s) {
            case State::kRecording: return "rec";
            case State::kPlaying: return "play";
            case State::kOverdub: return "dub";
            case State::kArmed: return "armed";
            default: return "idle";
        }
    }

    void process(float* io, std::size_t frames) override {
        const Command c = static_cast<Command>(cmd_.exchange(0));
        if (c != Command::kNone) apply(c);

        State st = state();
        if (st == State::kArmed) {
            if (!autostart()) {
                // Flag spento mentre era in attesa: si comporta come un toggle
                // da fermo con autostart OFF, cioe' registra da subito.
                start_blocks_ = 0;
                pos_ = 0;
                loop_len_ = 0;
                st = State::kRecording;
                state_.store(static_cast<int>(st));
            } else if (input_above_threshold(io, frames)) {
                if (++start_blocks_ >= kAutostartTriggerBlocks) {
                    start_blocks_ = 0;
                    pos_ = 0;
                    loop_len_ = 0;
                    st = State::kRecording;
                    state_.store(static_cast<int>(st));
                }
            } else {
                start_blocks_ = 0;
            }
        }

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
                if (pos_ == 0 && armed_) {
                    // overdub armato: parte al confine del loop (quantizzato).
                    armed_ = false;
                    st = State::kOverdub;
                    state_.store(static_cast<int>(st));
                }
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

    // RMS stereo del blocco in dBFS, confrontato con la soglia. Il blocco che
    // fa scattare il trigger viene registrato INTERO, dal campione 0.
    bool input_above_threshold(const float* io, std::size_t frames) const {
        if (frames == 0) return false;
        double acc = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            const double l = io[2 * i];
            const double r = io[2 * i + 1];
            acc += l * l + r * r;
        }
        const double rms = std::sqrt(acc / (2.0 * static_cast<double>(frames)));
        const double threshold =
            std::pow(10.0, autostart_threshold_db() / 20.0);
        return rms >= threshold;
    }

    void apply(Command c) {
        State st = state();
        if (c == Command::kStop) {
            // FS B: stop/riprendi SEMPRE. Se e' fermo con un loop riparte,
            // altrimenti ferma (finalizzando l'eventuale registrazione).
            if (st == State::kIdle) {
                if (loop_len_ > 0) {
                    pos_ = 0;
                    state_.store(static_cast<int>(State::kPlaying));
                }
            } else {
                if (st == State::kRecording && pos_ > 0) loop_len_ = pos_;
                armed_ = false;
                start_blocks_ = 0;
                pos_ = 0;
                state_.store(static_cast<int>(State::kIdle));
            }
        } else if (c == Command::kClear) {
            armed_ = false;
            start_blocks_ = 0;
            pos_ = 0;
            loop_len_ = 0;
            state_.store(static_cast<int>(State::kIdle));
        } else if (c == Command::kToggle) {
            if (st == State::kIdle) {
                pos_ = 0;
                armed_ = false;
                start_blocks_ = 0;
                if (loop_len_ > 0) {
                    state_.store(static_cast<int>(State::kPlaying));
                    armed_ = true;  // riprende e arma l'overdub al confine
                } else {
                    loop_len_ = 0;
                    // Autostart ON: attesa della prima nota, niente registrazione.
                    state_.store(static_cast<int>(autostart() ? State::kArmed
                                                              : State::kRecording));
                }
            } else if (st == State::kArmed) {
                // Secondo tocco in attesa: annulla, si torna fermi.
                start_blocks_ = 0;
                pos_ = 0;
                state_.store(static_cast<int>(State::kIdle));
            } else if (st == State::kRecording) {
                loop_len_ = pos_ > 0 ? pos_ : 1;
                pos_ = 0;
                state_.store(static_cast<int>(State::kPlaying));
            } else if (st == State::kPlaying) {
                armed_ = true;  // arma: parte al prossimo giro del loop
            } else {
                state_.store(static_cast<int>(State::kPlaying));  // chiude il dub
            }
        }
    }

    std::vector<float> buf_;
    std::size_t frames_total_ = 0;
    std::size_t pos_ = 0;
    std::size_t loop_len_ = 0;
    bool armed_ = false;
    int start_blocks_ = 0;
    double rate_ = 48000.0;
    double max_seconds_ = kDefaultMaxSeconds;
    std::atomic<int> state_{0};
    std::atomic<int> cmd_{0};
    std::atomic<bool> autostart_{false};
    std::atomic<double> autostart_threshold_db_{kDefaultAutostartThresholdDb};
    std::atomic<long long> pos_shared_{0};
    std::atomic<long long> len_shared_{0};
};

}  // namespace namzero
