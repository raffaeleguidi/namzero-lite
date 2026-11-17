#include "dsp/nam_slot.h"

#ifdef NAMZERO_HAVE_NAM

#include <filesystem>
#include <vector>

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
};

NamSlot::NamSlot() : impl_(std::make_unique<Impl>()) {}
NamSlot::~NamSlot() = default;

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

    for (std::size_t i = 0; i < frames; ++i) {
        const float v = static_cast<float>(impl_->out_buf[i]);
        interleaved[i * 2 + 0] = v;
        interleaved[i * 2 + 1] = v;
    }
}

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
double NamSlot::expected_sample_rate() const { return 0.0; }
bool NamSlot::slimmable() const { return false; }
bool NamSlot::loaded() const { return false; }

}  // namespace namzero

#endif
