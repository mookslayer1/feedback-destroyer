#pragma once
#include <JuceHeader.h>
#include <memory>
#include <vector>

// Averaged power spectrum of a stream of samples (Welch's method: overlapping Hann windows, 50% hop).
// Used to compare the room before and during the measurement burst. All memory is allocated in prepare().
class Welch
{
public:
    void prepare (int fftOrder)
    {
        size = 1 << fftOrder;
        hop = size / 2;
        fft = std::make_unique<juce::dsp::FFT> (fftOrder);

        window.assign ((size_t) size, 0.f);
        juce::dsp::WindowingFunction<float>::fillWindowingTables (
            window.data(), (size_t) size, juce::dsp::WindowingFunction<float>::hann, false);

        ring.assign ((size_t) size, 0.f);
        data.assign ((size_t) size * 2, 0.f);
        acc.assign ((size_t) size / 2 + 1, 0.0);
        reset();
    }

    void reset()
    {
        std::fill (ring.begin(), ring.end(), 0.f);
        std::fill (acc.begin(), acc.end(), 0.0);
        count = 0;
        ringPos = 0;
        hopCount = 0;
        filled = 0;
    }

    void push (float s)
    {
        ring[(size_t) ringPos] = s;
        ringPos = (ringPos + 1) & (size - 1);
        ++filled;

        if (++hopCount >= hop)
        {
            hopCount = 0;

            if (filled >= size)
                accumulate();
        }
    }

    bool   hasData() const { return count > 0; }
    int    segments() const { return count; }
    double power (int k) const { return count > 0 ? acc[(size_t) k] / (double) count : 0.0; }

    int size = 0, hop = 0;

private:
    void accumulate()
    {
        for (int j = 0; j < size; ++j)
            data[(size_t) j] = ring[(size_t) ((ringPos + j) & (size - 1))] * window[(size_t) j];

        std::fill (data.begin() + size, data.end(), 0.f);
        fft->performFrequencyOnlyForwardTransform (data.data(), true);

        for (int k = 0; k <= size / 2; ++k)
        {
            const double m = (double) data[(size_t) k];
            acc[(size_t) k] += m * m;
        }

        ++count;
    }

    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window, ring, data;
    std::vector<double> acc;
    int ringPos = 0, hopCount = 0, filled = 0, count = 0;
};
