#include "dsp/chain.h"

#include <algorithm>
#include <cstring>

namespace namzero {
namespace {

constexpr std::size_t kChannels = 2;
constexpr double kBypassMs = 5.0;

}  // namespace

void Chain::add(std::unique_ptr<Slot> slot) {
    auto entry = std::make_unique<Entry>();
    entry->slot = std::move(slot);
    entries_.push_back(std::move(entry));
}

void Chain::prepare(double sample_rate, std::size_t max_frames) {
    dry_.assign(max_frames * kChannels, 0.0f);

    for (auto& entry : entries_) {
        entry->slot->prepare(sample_rate, max_frames);
    }

    float frames = static_cast<float>(sample_rate * kBypassMs * 0.001);
    if (frames < 1.0f) frames = 1.0f;
    ramp_ = 1.0f / frames;
}

void Chain::set_enabled(std::size_t index, bool on) {
    if (index < entries_.size()) {
        entries_[index]->enabled.store(on, std::memory_order_relaxed);
    }
}

bool Chain::enabled(std::size_t index) const {
    return index < entries_.size() &&
           entries_[index]->enabled.load(std::memory_order_relaxed);
}

void Chain::reset() {
    for (auto& entry : entries_) {
        entry->slot->reset();
        entry->mix = entry->enabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
    }
}

void Chain::process(float* interleaved, std::size_t frames) {
    const std::size_t samples = frames * kChannels;

    for (auto& up : entries_) {
        Entry& entry = *up;

        const float target =
            entry.enabled.load(std::memory_order_relaxed) ? 1.0f : 0.0f;

        if (entry.mix == target) {
            if (target > 0.0f) {
                entry.slot->process(interleaved, frames);
            }
            continue;
        }

        std::memcpy(dry_.data(), interleaved, samples * sizeof(float));
        entry.slot->process(interleaved, frames);

        float mix = entry.mix;
        for (std::size_t i = 0; i < frames; ++i) {
            if (mix != target) {
                mix = (target > mix) ? std::min(target, mix + ramp_)
                                     : std::max(target, mix - ramp_);
            }
            for (std::size_t ch = 0; ch < kChannels; ++ch) {
                const std::size_t k = i * kChannels + ch;
                interleaved[k] = dry_[k] + (interleaved[k] - dry_[k]) * mix;
            }
        }
        entry.mix = mix;
    }
}

}  // namespace namzero
