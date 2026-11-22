#pragma once
#include <cmath>
#include <utility>
#include <vector>

namespace fd
{
// Minimal radix-2 complex FFT (forward only). No JUCE dependency so the engine can be
// unit-tested off-plugin. Allocation happens in init() only.
class Fft
{
public:
    void init (int n)
    {
        n_ = n;
        rev_.assign ((size_t) n, 0);
        cosT_.assign ((size_t) n / 2, 0.f);
        sinT_.assign ((size_t) n / 2, 0.f);

        int bits = 0;
        while ((1 << bits) < n) ++bits;

        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b)
                if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev_[(size_t) i] = r;
        }

        for (int k = 0; k < n / 2; ++k)
        {
            const double a = 2.0 * 3.14159265358979323846 * k / n;
            cosT_[(size_t) k] = (float) std::cos (a);
            sinT_[(size_t) k] = (float) std::sin (a);
        }
    }

    void forward (float* re, float* im) const noexcept
    {
        for (int i = 0; i < n_; ++i)
        {
            const int j = rev_[(size_t) i];
            if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }

        for (int len = 2; len <= n_; len <<= 1)
        {
            const int half = len >> 1, step = n_ / len;
            for (int i = 0; i < n_; i += len)
                for (int k = 0; k < half; ++k)
                {
                    const float wr =  cosT_[(size_t) (k * step)];
                    const float wi = -sinT_[(size_t) (k * step)];
                    const int p = i + k, q = p + half;
                    const float vr = re[q] * wr - im[q] * wi;
                    const float vi = re[q] * wi + im[q] * wr;
                    re[q] = re[p] - vr;  im[q] = im[p] - vi;
                    re[p] += vr;         im[p] += vi;
                }
        }
    }

private:
    int n_ = 0;
    std::vector<int> rev_;
    std::vector<float> cosT_, sinT_;
};
} // namespace fd
