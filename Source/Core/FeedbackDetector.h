#pragma once
#include "Fft.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace fd
{
struct Detection
{
    float freqHz   = 0.f;
    float levelDb  = -200.f;  // dBFS amplitude of the (interpolated) peak
    float growthDb = 0.f;     // level change over the last 4 analysis frames
    float pnpr     = 0.f;     // peak-to-neighbour power ratio, dB
};

struct DetectorParams
{
    float sensitivity = 0.5f; // 0..1
    int   speed       = 1;    // 0 slow, 1 medium, 2 fast
    float minFreqHz   = 80.f;
    float maxFreqHz   = 16000.f;
};

// Analysis-only feedback detector. Never touches the audio, so it adds no latency.
// Cues combined per spectral peak (see Waterschoot & Moonen, "Fifty years of acoustic
// feedback control"):
//   PNPR        - feedback tones are far narrower than speech/music partials
//   PHPR        - feedback has no harmonic series at 2f, 3f
//   growth      - magnitude climbs frame over frame
//   persistence - the same bin (+-1) must qualify over consecutive frames
//   tonal guard - many strong narrow lines at once means music, so steady-state
//                 triggering is disabled and only growing peaks can trigger
class FeedbackDetector
{
public:
    static constexpr int kFftSize       = 8192;
    static constexpr int kHop           = 2048;
    static constexpr int kBins          = kFftSize / 2 + 1;
    static constexpr int kHistory       = 5;
    static constexpr int kMaxDetections = 4;
    static constexpr int kWarmupFrames  = 24; // ignore the first ~1 s while history fills
    static constexpr int kMaxCands      = 64;

    void prepare (double sampleRate)
    {
        sr_ = sampleRate;
        fft_.init (kFftSize);
        ring_.assign (kFftSize, 0.f);
        window_.resize (kFftSize);
        re_.assign (kFftSize, 0.f);
        im_.assign (kFftSize, 0.f);
        hist_.assign ((size_t) kHistory * kBins, -200.f);
        persist_.assign (kBins, 0);
        persistNew_.assign (kBins, 0);

        for (int i = 0; i < kFftSize; ++i)
            window_[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * 3.14159265358979323846 * i / kFftSize);

        reset();
    }

    void reset()
    {
        std::fill (ring_.begin(), ring_.end(), 0.f);
        std::fill (hist_.begin(), hist_.end(), -200.f);
        std::fill (persist_.begin(), persist_.end(), 0);
        writePos_ = hopCounter_ = histIdx_ = 0;
        frameCount_ = 0;
    }

    void setParams (const DetectorParams& p) noexcept { params_ = p; }
    double binHz() const noexcept { return sr_ / kFftSize; }

    // Feed mono samples. Detections from any analysis frames completed inside this call
    // are appended to out (up to maxOut). Returns the number written.
    int push (const float* x, int n, Detection* out, int maxOut)
    {
        int written = 0;
        for (int i = 0; i < n; ++i)
        {
            ring_[(size_t) writePos_] = x[i];
            writePos_ = (writePos_ + 1) & (kFftSize - 1);

            if (++hopCounter_ >= kHop)
            {
                hopCounter_ = 0;
                if (written < maxOut)
                    written += analyse (out + written, maxOut - written);
                else
                    analyse (nullptr, 0);
            }
        }
        return written;
    }

private:
    struct Cand { int bin; float level; float pnpr; int persist; float growth; bool harmonic; };

    int analyse (Detection* out, int maxOut)
    {
        const float* const win = window_.data();
        float* const re = re_.data();
        float* const im = im_.data();

        for (int i = 0; i < kFftSize; ++i)
        {
            re[i] = ring_[(size_t) ((writePos_ + i) & (kFftSize - 1))] * win[i];
            im[i] = 0.f;
        }
        fft_.forward (re, im);

        // Amplitude in dBFS: a full-scale-amplitude-A sine gives |X| = A*N/4 with a Hann window.
        float* const mag = hist_.data() + (size_t) histIdx_ * kBins;
        const float scale = 4.f / (float) kFftSize;
        for (int k = 0; k < kBins; ++k)
        {
            const float p = (re[k] * re[k] + im[k] * im[k]) * scale * scale;
            mag[k] = 10.f * std::log10 (std::max (p, 1e-18f));
        }

        const float* const old = hist_.data() + (size_t) ((histIdx_ + 1) % kHistory) * kBins; // t-4
        const bool haveOld = frameCount_ >= kHistory - 1;

        const float sens      = std::clamp (params_.sensitivity, 0.f, 1.f);
        const float pnprThr   = 22.f - 12.f * sens;
        const float phprThr   = 16.f - 8.f * sens;
        const float growthThr = 5.f - 3.f * sens;
        const float minLevel  = -80.f + 15.f * (1.f - sens); // absolute gate on peak amplitude, dBFS
        const int steadyFrames = params_.speed == 0 ? 24 : (params_.speed == 2 ? 6 : 12);

        const int kMin = std::max (9, (int) std::ceil (params_.minFreqHz / binHz()));
        const int kMax = std::min (kBins - 10, (int) std::floor (params_.maxFreqHz / binHz()));

        std::fill (persistNew_.begin(), persistNew_.end(), 0);

        std::array<Cand, kMaxCands> cands;
        int nc = 0;

        for (int b = kMin; b <= kMax; ++b)
        {
            const float m = mag[b];
            if (m <= mag[b - 1] || m < mag[b + 1] || m < minLevel) continue;

            float sum = 0.f;
            for (int o = 3; o <= 8; ++o) sum += mag[b - o] + mag[b + o];
            const float pnpr = m - sum / 12.f;
            if (pnpr < pnprThr) continue;

            float h = -200.f;
            for (int mult = 2; mult <= 3; ++mult)
            {
                const int hb = b * mult;
                if (hb + 2 < kBins)
                    for (int j = -2; j <= 2; ++j) h = std::max (h, mag[hb + j]);
            }
            if (m - h < phprThr) continue;

            // Parent-line check: if b is a harmonic of a stronger narrow line at b/2..b/8
            // (e.g. the 3rd/5th harmonics of a clipped feedback tone, or the top partial of
            // a note) it is not the frequency to notch. The parent gets judged on its own.
            bool isHarmonic = false;
            for (int div = 2; div <= 8 && ! isHarmonic; ++div)
            {
                const int lb = (b + div / 2) / div;
                if (lb < 10) break;
                int best = lb;
                for (int j = -2; j <= 2; ++j) if (mag[lb + j] > mag[best]) best = lb + j;
                if (best < 9 || mag[best] < m - 3.f) continue;
                float ls = 0.f;
                for (int o = 3; o <= 8; ++o) ls += mag[best - o] + mag[best + o];
                isHarmonic = (mag[best] - ls / 12.f) >= 10.f;
            }

            const int prevPersist = std::max ({ persist_[(size_t) b - 1], persist_[(size_t) b], persist_[(size_t) b + 1] });
            persistNew_[(size_t) b] = prevPersist + 1;

            if (nc < kMaxCands)
            {
                float growth = 0.f;
                if (haveOld)
                    growth = m - std::max ({ old[b - 1], old[b], old[b + 1] });
                cands[(size_t) nc++] = { b, m, pnpr, prevPersist + 1, growth, isHarmonic };
            }
        }
        persist_.swap (persistNew_);

        int nOut = 0;
        const bool warm = frameCount_ >= kWarmupFrames;

        if (warm && nc > 0 && out != nullptr)
        {
            float strongest = -200.f;
            for (int i = 0; i < nc; ++i) strongest = std::max (strongest, cands[(size_t) i].level);
            int strong = 0;
            for (int i = 0; i < nc; ++i) strong += cands[(size_t) i].level >= strongest - 20.f;
            const bool tonal = strong > 4;

            std::array<Detection, kMaxCands> found;
            int nf = 0;

            for (int i = 0; i < nc; ++i)
            {
                const Cand& c = cands[(size_t) i];
                if (c.harmonic) continue; // still counted above for the tonal guard
                const bool steady = c.persist >= steadyFrames && ! tonal;
                const bool rising = c.persist >= 3 && c.growth >= growthThr;
                if (! (steady || rising)) continue;

                // Parabolic interpolation on the log magnitude for sub-bin frequency.
                const float a = mag[c.bin - 1], bb = mag[c.bin], cc = mag[c.bin + 1];
                const float denom = a - 2.f * bb + cc;
                const float p = std::abs (denom) > 1e-9f ? 0.5f * (a - cc) / denom : 0.f;

                Detection d;
                d.freqHz   = (float) ((c.bin + std::clamp (p, -0.5f, 0.5f)) * binHz());
                d.levelDb  = bb - 0.25f * (a - cc) * p;
                d.growthDb = c.growth;
                d.pnpr     = c.pnpr;
                found[(size_t) nf++] = d;
            }

            std::sort (found.begin(), found.begin() + nf,
                       [] (const Detection& x, const Detection& y) { return x.levelDb > y.levelDb; });
            // Dominant-line-first: deal with the loudest ringing line before reacting to much
            // weaker ones, which are usually distortion products of it (clipped PA, intermod).
            while (nf > 1 && found[(size_t) nf - 1].levelDb < found[0].levelDb - 15.f) --nf;
            nOut = std::min ({ nf, maxOut, kMaxDetections });
            for (int i = 0; i < nOut; ++i) out[i] = found[(size_t) i];
        }

        histIdx_ = (histIdx_ + 1) % kHistory;
        ++frameCount_;
        return nOut;
    }

    double sr_ = 48000.0;
    DetectorParams params_;
    Fft fft_;
    std::vector<float> ring_, window_, re_, im_, hist_;
    std::vector<int> persist_, persistNew_;
    int writePos_ = 0, hopCounter_ = 0, histIdx_ = 0;
    long frameCount_ = 0;
};
} // namespace fd
