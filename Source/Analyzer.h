#pragma once
#include <JuceHeader.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

struct Candidate
{
    float  prom = 0.f;   // how far the peak stands above its surroundings (dB)
    double freq = 0.0;   // exact frequency (Hz)
};

// Listens to a stream of samples and looks for feedback-like peaks in one
// frequency band. Two of these run side by side with different window lengths:
// a long, slow one for the low end (fine frequency detail) and a shorter,
// faster one for everything above.
class Analyzer
{
public:
    void prepare (int fftOrder, int hopSize)
    {
        size = 1 << fftOrder;
        hop = hopSize;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);

        window.assign ((size_t) size, 0.f);
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            window.data(), (size_t) size, juce::dsp::WindowingFunction<float>::hann, false);

        const size_t bins = (size_t) size / 2 + 1;
        ring.assign ((size_t) size, 0.f);
        fftData.assign ((size_t) size * 2, 0.f);
        level.assign (bins, -120.f);
        hist.assign (bins * 4, -120.f);
        persist.assign (bins, 0);
        persistNew.assign (bins, 0);
        peakProm.assign (bins, 0.f);

        ringPos = hopCount = histFilled = 0;
        frame = 0;
        lastLo = 0;
        lastHi = -1;
    }

    bool isReady() const { return fft != nullptr; }

    void clearTracking()
    {
        std::fill (persist.begin(), persist.end(), 0);
        histFilled = 0;
    }

    // Feed one sample. Returns true when a new analysis frame is due.
    bool push (float s)
    {
        ring[(size_t) ringPos] = s;
        ringPos = (ringPos + 1) & (size - 1);

        if (++hopCount >= hop)
        {
            hopCount = 0;
            return true;
        }
        return false;
    }

    // Looks at the last 'size' samples and returns feedback-like peaks between
    // lowHz and highHz, strongest first.
    int analyse (double sr, double lowHz, double highHz, float promThr, double steadySec,
                 Candidate* out, int maxOut)
    {
        const int half = size / 2;

        for (int j = 0; j < size; ++j)
            fftData[(size_t) j] = ring[(size_t) ((ringPos + j) & (size - 1))] * window[(size_t) j];

        std::fill (fftData.begin() + size, fftData.end(), 0.f);
        fft->performFrequencyOnlyForwardTransform (fftData.data(), true);

        const float norm = 1.f / ((float) size * 0.25f);   // a full-scale sine reads about 0 dB
        for (int k = 0; k <= half; ++k)
            level[(size_t) k] = juce::Decibels::gainToDecibels (fftData[(size_t) k] * norm, -120.f);

        std::fill (peakProm.begin(), peakProm.end(), 0.f);
        std::fill (persistNew.begin(), persistNew.end(), 0);

        const int kLo = juce::jmax (16, (int) std::ceil (lowHz * size / sr));
        const int kHi = juce::jmin (half - 16, (int) std::floor (highHz * size / sr));
        lastLo = kLo;
        lastHi = kHi;

        const size_t oldSlot = (size_t) ((frame + 1) % 4);   // the frame from 3 frames ago
        const int steadyFrames = juce::jmax (4, (int) std::ceil (steadySec * sr / hop));
        int n = 0;

        for (int k = kLo; k <= kHi; ++k)
        {
            const float lv = level[(size_t) k];

            if (lv < -65.f)
                continue;

            const bool isPeak = lv > level[(size_t) k - 1] && lv >= level[(size_t) k + 1]
                             && lv > level[(size_t) k - 2] && lv >= level[(size_t) k + 2];
            if (! isPeak)
                continue;

            float sum = 0.f;
            for (int d = 6; d <= 14; ++d)
                sum += level[(size_t) (k - d)] + level[(size_t) (k + d)];

            const float prom = lv - sum / 18.f;
            peakProm[(size_t) k] = juce::jmax (0.f, prom);

            if (prom < promThr)
                continue;

            const int p = 1 + juce::jmax (persist[(size_t) k - 1], persist[(size_t) k], persist[(size_t) k + 1]);
            persistNew[(size_t) k] = p;

            const bool growing = histFilled >= 4
                              && (lv - hist[oldSlot * (size_t) (half + 1) + (size_t) k]) >= 2.5f;
            const bool steady  = p >= steadyFrames && prom >= promThr + 6.f;

            if ((p >= 3 && growing) || steady)
            {
                // sharpen the frequency estimate between bins
                const float a = level[(size_t) k - 1], b = lv, c = level[(size_t) k + 1];
                const float denom = a - 2.f * b + c;
                const float delta = std::abs (denom) > 1.0e-6f ? 0.5f * (a - c) / denom : 0.f;

                Candidate cand;
                cand.prom = prom;
                cand.freq = ((double) k + juce::jlimit (-0.5f, 0.5f, delta)) * sr / size;

                if (n < maxOut)
                {
                    out[n++] = cand;
                }
                else
                {
                    int weakest = 0;
                    for (int i = 1; i < n; ++i)
                        if (out[i].prom < out[weakest].prom)
                            weakest = i;
                    if (cand.prom > out[weakest].prom)
                        out[weakest] = cand;
                }
            }
        }

        persist.swap (persistNew);

        std::copy (level.begin(), level.end(), hist.begin() + (size_t) (frame % 4) * (size_t) (half + 1));
        histFilled = juce::jmin (histFilled + 1, 4);
        ++frame;

        std::sort (out, out + n, [] (const Candidate& x, const Candidate& y) { return x.prom > y.prom; });
        return n;
    }

    std::vector<float> peakProm;   // prominence of every peak found in the last frame (0 elsewhere)
    int lastLo = 0, lastHi = -1;   // bin range that was searched
    int size = 0, hop = 0;

private:
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, ring, fftData, level, hist;
    std::vector<int> persist, persistNew;
    int ringPos = 0, hopCount = 0, histFilled = 0;
    long frame = 0;
};
