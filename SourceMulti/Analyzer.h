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
    void prepare (int fftOrder, int hopSize, int histStride = 1)
    {
        stride = juce::jmax (1, histStride);
        histCount = 0;
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
        bonus.assign (bins, 0.f);
        gateLevel.assign (bins, -200.f);
        baseP.assign (bins, 0.0);
        toneCnt.assign (bins, 0);
        toneMask.assign (bins, 0);
        baseFrames = 0;
        gateOn = false;

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

        const size_t oldSlot = (size_t) ((histCount + 1) % 4);   // the snapshot from 3 snapshots ago
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

            const float thr = promThr - bonus[(size_t) k];   // watch bands from the measurement are more sensitive

            if (prom < thr)
                continue;

            const int p = 1 + juce::jmax (persist[(size_t) k - 1], persist[(size_t) k], persist[(size_t) k + 1]);
            persistNew[(size_t) k] = p;

            const bool growing = histFilled >= 4
                              && (lv - hist[oldSlot * (size_t) (half + 1) + (size_t) k]) >= 2.5f;
            const bool steady  = p >= steadyFrames && prom >= thr + 6.f;

            // A steady room tone (fan, hum) found in the Listen stage only counts once it climbs well above its baseline.
            const bool gated = gateOn && lv < gateLevel[(size_t) k] + gateMarginDb;

            if (((p >= 3 && growing) || steady) && ! gated)
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

        if ((frame % stride) == 0)
        {
            std::copy (level.begin(), level.end(), hist.begin() + (size_t) (histCount % 4) * (size_t) (half + 1));
            histFilled = juce::jmin (histFilled + 1, 4);
            ++histCount;
        }
        ++frame;

        std::sort (out, out + n, [] (const Candidate& x, const Candidate& y) { return x.prom > y.prom; });
        return n;
    }

    // ---- Listen stage: learn the room's own steady tones ----
    void clearBaseline()
    {
        std::fill (baseP.begin(), baseP.end(), 0.0);
        std::fill (toneCnt.begin(), toneCnt.end(), 0);
        std::fill (toneMask.begin(), toneMask.end(), 0);
        std::fill (gateLevel.begin(), gateLevel.end(), -200.f);
        baseFrames = 0;
        gateOn = false;
    }

    // Call once after each analyse() while listening.
    void accumulateBaseline()
    {
        const int half = size / 2;
        for (int k = 0; k <= half; ++k)
        {
            baseP[(size_t) k] += std::pow (10.0, (double) level[(size_t) k] * 0.1);

            if (peakProm[(size_t) k] >= 10.f)
                ++toneCnt[(size_t) k];
        }
        ++baseFrames;
    }

    // Marks bins that held a steady peak through most of the listening time and sets their gate level.
    int finishBaseline()
    {
        const int half = size / 2;
        int tones = 0;

        if (baseFrames < 8)
        {
            gateOn = false;
            return 0;
        }

        std::vector<float> baseDb ((size_t) half + 1);
        for (int k = 0; k <= half; ++k)
            baseDb[(size_t) k] = (float) (10.0 * std::log10 (juce::jmax (1.0e-20, baseP[(size_t) k] / baseFrames)));

        const int need = (int) (0.6 * baseFrames);

        for (int k = 2; k < half - 2; ++k)
        {
            const int c = juce::jmax (toneCnt[(size_t) k - 1], toneCnt[(size_t) k], toneCnt[(size_t) k + 1]);

            if (c >= need && toneCnt[(size_t) k] >= need / 2)
            {
                toneMask[(size_t) k] = 1;
                ++tones;
            }
        }

        for (int k = 2; k < half - 2; ++k)
        {
            bool near = false;
            for (int d = -1; d <= 1; ++d)
                near = near || toneMask[(size_t) (k + d)] != 0;

            if (near)
                gateLevel[(size_t) k] = juce::jmax (baseDb[(size_t) k - 1], baseDb[(size_t) k], baseDb[(size_t) k + 1]);
        }

        gateOn = true;
        return tones;
    }

    void clearWatch() { std::fill (bonus.begin(), bonus.end(), 0.f); }

    // Makes detection more sensitive around fc (+/- halfOctaves).
    void addWatch (double sr, double fc, double halfOctaves, float bonusDb)
    {
        const double lo = fc * std::pow (2.0, -halfOctaves), hi = fc * std::pow (2.0, halfOctaves);
        const int kLo = juce::jmax (0, (int) std::floor (lo * size / sr));
        const int kHi = juce::jmin (size / 2, (int) std::ceil (hi * size / sr));
        for (int k = kLo; k <= kHi; ++k)
            bonus[(size_t) k] = juce::jmax (bonus[(size_t) k], bonusDb);
    }

    bool isTone (int k) const { return k >= 0 && k <= size / 2 && toneMask[(size_t) k] != 0; }

    float gateMarginDb = 6.f;
    bool  gateOn = false;

    std::vector<float> peakProm;   // prominence of every peak found in the last frame (0 elsewhere)
    int lastLo = 0, lastHi = -1;   // bin range that was searched
    int size = 0, hop = 0;

private:
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, ring, fftData, level, hist, bonus, gateLevel;
    std::vector<double> baseP;
    std::vector<int> toneCnt;
    std::vector<unsigned char> toneMask;
    int baseFrames = 0;
    std::vector<int> persist, persistNew;
    int ringPos = 0, hopCount = 0, histFilled = 0;
    long frame = 0;
    long histCount = 0;
    int stride = 1;
};
