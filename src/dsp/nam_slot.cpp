#include "dsp/nam_slot.h"

#ifdef NAMZERO_HAVE_NAM

#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

#include "json.hpp"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "NAM/slimmable.h"

namespace namzero {

struct NamSlot::Impl {
    std::unique_ptr<nam::DSP> dsp;
    std::vector<NAM_SAMPLE> in_buf;
    std::vector<NAM_SAMPLE> out_buf;
    NAM_SAMPLE* in_ptrs[1] = {nullptr};
    NAM_SAMPLE* out_ptrs[1] = {nullptr};
    double sample_rate = 0.0;
    bool slimmable = false;
    double model_gain = 1.0;   // gain dichiarato dal .nam
    double trim_db = 0.0;      // trim utente
};

NamSlot::NamSlot() : impl_(std::make_unique<Impl>()) {}
NamSlot::~NamSlot() = default;

namespace {

// Il gain di normalizzazione sta in metadata.gain. Nei SlimmableContainer ogni
// sottomodello ha il suo, quindi si sceglie quello che verra' attivato.
double read_model_gain(const std::string& path, double slim) {
    try {
        std::ifstream in(path);
        if (!in) return 1.0;
        nlohmann::json j;
        in >> j;

        if (j.contains("config") && j["config"].contains("submodels")) {
            const auto& subs = j["config"]["submodels"];
            for (const auto& s : subs) {
                const double max_value = s.value("max_value", 1.0);
                if (slim < max_value) {
                    if (s.contains("model") && s["model"].contains("metadata")) {
                        return s["model"]["metadata"].value("gain", 1.0);
                    }
                    return 1.0;
                }
            }
        }
        if (j.contains("metadata")) {
            return j["metadata"].value("gain", 1.0);
        }
    } catch (...) {
    }
    return 1.0;
}

}  // namespace

bool NamSlot::load(const std::string& path, double slim, std::string& err) {
    try {
        auto dsp = nam::get_dsp(std::filesystem::path(path));
        if (!dsp) {
            err = "il modello non e' stato caricato";
            return false;
        }

        impl_->slimmable = false;
        if (auto* s = dynamic_cast<nam::SlimmableModel*>(dsp.get())) {
            impl_->slimmable = true;
            if (slim >= 0.0 && slim <= 1.0) {
                s->SetSlimmableSize(slim);
            }
        }

        impl_->sample_rate = dsp->GetExpectedSampleRate();
        impl_->model_gain = read_model_gain(path, slim);
        impl_->dsp = std::move(dsp);
        impl_->in_ptrs[0] = nullptr;
        impl_->out_ptrs[0] = nullptr;
        return true;
    } catch (const std::exception& e) {
        err = std::string("NAM: ") + e.what();
        return false;
    }
}

void NamSlot::prepare(double sample_rate, std::size_t max_frames) {
    if (!impl_->dsp) return;

    impl_->in_buf.assign(max_frames, NAM_SAMPLE(0));
    impl_->out_buf.assign(max_frames, NAM_SAMPLE(0));
    impl_->in_ptrs[0] = impl_->in_buf.data();
    impl_->out_ptrs[0] = impl_->out_buf.data();

    impl_->dsp->Reset(sample_rate, static_cast<int>(max_frames));
}

void NamSlot::reset() {
    if (!impl_->dsp) return;
    impl_->dsp->Reset(impl_->sample_rate, static_cast<int>(impl_->in_buf.size()));
}

void NamSlot::process(float* interleaved, std::size_t frames) {
    if (!impl_->dsp || frames > impl_->in_buf.size()) return;

    // Il modello e' mono: somma dei due canali in ingresso.
    for (std::size_t i = 0; i < frames; ++i) {
        impl_->in_buf[i] = static_cast<NAM_SAMPLE>(
            0.5f * (interleaved[i * 2 + 0] + interleaved[i * 2 + 1]));
    }

    impl_->dsp->process(impl_->in_ptrs, impl_->out_ptrs, static_cast<int>(frames));

    const float gain = static_cast<float>(
        impl_->model_gain * std::pow(10.0, impl_->trim_db / 20.0));
    for (std::size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(impl_->out_buf[i]) * gain;
        interleaved[i * 2 + 0] = v;
        interleaved[i * 2 + 1] = v;
    }
}

void NamSlot::set_trim_db(double db) { impl_->trim_db = db; }
double NamSlot::model_gain() const { return impl_->model_gain; }
double NamSlot::expected_sample_rate() const { return impl_->sample_rate; }
bool NamSlot::slimmable() const { return impl_->slimmable; }
bool NamSlot::loaded() const { return impl_->dsp != nullptr; }

}  // namespace namzero

#else  // !NAMZERO_HAVE_NAM

namespace namzero {

struct NamSlot::Impl {};

NamSlot::NamSlot() : impl_(std::make_unique<Impl>()) {}
NamSlot::~NamSlot() = default;

bool NamSlot::load(const std::string&, double, std::string& err) {
    err = "binario compilato senza supporto NAM (manca -DNAM_DIR)";
    return false;
}

void NamSlot::prepare(double, std::size_t) {}
void NamSlot::process(float*, std::size_t) {}
void NamSlot::reset() {}
void NamSlot::set_trim_db(double) {}
double NamSlot::model_gain() const { return 1.0; }
double NamSlot::expected_sample_rate() const { return 0.0; }
bool NamSlot::slimmable() const { return false; }
bool NamSlot::loaded() const { return false; }

}  // namespace namzero

#endif
