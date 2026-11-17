#include "dsp/partitioned_convolver.h"

#include <algorithm>

namespace namzero {

bool PartitionedConvolver::init(const std::vector<float>& ir, std::size_t block) {
    if (ir.empty()) return false;
    if (block < 2 || (block & (block - 1)) != 0) return false;

    block_ = block;
    nfft_ = 2 * block;
    parts_ = (ir.size() + block - 1) / block;
    cur_ = 0;

    h_.assign(parts_ * nfft_, std::complex<float>(0.0f, 0.0f));
    x_.assign(parts_ * nfft_, std::complex<float>(0.0f, 0.0f));
    frame_.assign(nfft_, std::complex<float>(0.0f, 0.0f));
    acc_.assign(nfft_, std::complex<float>(0.0f, 0.0f));

    prev_.assign(block_, 0.0f);
    in_buf_.assign(block_, 0.0f);
    out_buf_.assign(block_, 0.0f);
    in_count_ = 0;
    out_pos_ = block_;  // buffer di uscita vuoto

    for (std::size_t p = 0; p < parts_; ++p) {
        std::fill(frame_.begin(), frame_.end(), std::complex<float>(0.0f, 0.0f));
        const std::size_t base = p * block;
        for (std::size_t i = 0; i < block_; ++i) {
            const std::size_t idx = base + i;
            if (idx < ir.size()) {
                frame_[i] = std::complex<float>(ir[idx], 0.0f);
            }
        }
        fft(frame_, false);
        std::copy(frame_.begin(), frame_.end(), h_.begin() + p * nfft_);
    }
    return true;
}

void PartitionedConvolver::reset() {
    std::fill(x_.begin(), x_.end(), std::complex<float>(0.0f, 0.0f));
    std::fill(prev_.begin(), prev_.end(), 0.0f);
    std::fill(in_buf_.begin(), in_buf_.end(), 0.0f);
    std::fill(out_buf_.begin(), out_buf_.end(), 0.0f);
    in_count_ = 0;
    out_pos_ = block_;
    cur_ = 0;
}

void PartitionedConvolver::step() {
    // Framing overlap-save: [blocco precedente, blocco corrente].
    for (std::size_t i = 0; i < block_; ++i) {
        frame_[i] = std::complex<float>(prev_[i], 0.0f);
        frame_[i + block_] = std::complex<float>(in_buf_[i], 0.0f);
    }
    fft(frame_, false);

    // Salva lo spettro nel ritardo circolare.
    std::copy(frame_.begin(), frame_.end(), x_.begin() + cur_ * nfft_);

    std::fill(acc_.begin(), acc_.end(), std::complex<float>(0.0f, 0.0f));
    for (std::size_t p = 0; p < parts_; ++p) {
        const std::size_t idx = (cur_ + parts_ - p) % parts_;
        const std::complex<float>* h = h_.data() + p * nfft_;
        const std::complex<float>* x = x_.data() + idx * nfft_;
        for (std::size_t k = 0; k < nfft_; ++k) {
            acc_[k] += h[k] * x[k];
        }
    }
    fft(acc_, true);

    // Overlap-save: la parte valida e' la seconda meta'.
    for (std::size_t i = 0; i < block_; ++i) {
        out_buf_[i] = acc_[i + block_].real();
    }

    std::copy(in_buf_.begin(), in_buf_.end(), prev_.begin());
    cur_ = (cur_ + 1) % parts_;
}

void PartitionedConvolver::process(const float* in, float* out, std::size_t frames) {
    if (!ready()) {
        std::fill(out, out + frames, 0.0f);
        return;
    }

    // Indici separati: l'uscita e' ritardata di un blocco, quindi non puo'
    // usare lo stesso contatore dell'ingresso.
    std::size_t in_i = 0;
    std::size_t out_i = 0;

    while (out_i < frames) {
        if (out_pos_ < block_) {
            out[out_i++] = out_buf_[out_pos_++];
            continue;
        }
        while (in_count_ < block_ && in_i < frames) {
            in_buf_[in_count_++] = in[in_i++];
        }
        if (in_count_ == block_) {
            step();
            in_count_ = 0;
            out_pos_ = 0;
        } else {
            break;
        }
    }

    // Fine dello stream: quel che resta e' silenzio (la coda e' gia' uscita).
    while (out_i < frames) {
        out[out_i++] = 0.0f;
    }
}

}  // namespace namzero
