#pragma once
#include <algorithm>
#include <cmath>

namespace fd
{
// Single-sideband frequency shifter: shifts every frequency by a fixed number of Hz
// (not a ratio, so it is not a pitch shifter). A shift of a couple of Hz stops a feedback
// loop from staying phase-aligned with itself, which buys a few dB of gain before ringing.
//
// The Hilbert (90 degree) pair is a 2 x 4-section allpass IIR design by Olli Niemitalo.
// Accurate from roughly 0.2% to 49.8% of the sample rate (about 100 Hz up at 48 kHz) and
// adds no fixed latency, only frequency-dependent phase.
class FrequencyShifter
{
public:
    static constexpr int kMaxChannels = 2;

    void prepare (double sampleRate) { sr_ = sampleRate; reset(); }

    void reset()
    {
        for (auto& c : ch_) c = ChannelState{};
        phase_ = 0.0;
        mix_ = 0.0;
        dirty_ = false;
    }

    // shiftHz > 0 moves frequencies up. enabled=false fades the effect out over ~20 ms.
    void process (float* const* ch, int numCh, int n, float shiftHz, bool enabled)
    {
        const bool wantOn = enabled && std::abs (shiftHz) > 1e-4f;
        if (! wantOn && mix_ <= 0.0)
        {
            if (dirty_) { for (auto& c : ch_) c = ChannelState{}; dirty_ = false; }
            return;
        }
        dirty_ = true;

        const double mixStep = 1.0 / (0.02 * sr_);
        const double dphi = 2.0 * 3.14159265358979323846 * (double) shiftHz / sr_;
        numCh = std::min (numCh, kMaxChannels);

        for (int i = 0; i < n; ++i)
        {
            mix_ = wantOn ? std::min (1.0, mix_ + mixStep) : std::max (0.0, mix_ - mixStep);
            phase_ += dphi;
            if (phase_ > 3.14159265358979323846)       phase_ -= 2.0 * 3.14159265358979323846;
            else if (phase_ < -3.14159265358979323846) phase_ += 2.0 * 3.14159265358979323846;
            const double c = std::cos (phase_), s = std::sin (phase_);

            for (int k = 0; k < numCh; ++k)
            {
                const double x = ch[k][i];
                const double y = ch_[k].shift (x, c, s);
                ch[k][i] = (float) ((1.0 - mix_) * x + mix_ * y);
            }
        }
    }

private:
    struct Section { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };

    static constexpr double kA0[4] = { 0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737 };
    static constexpr double kA1[4] = { 0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278 };

    static double run (Section* s, const double* a, double x) noexcept
    {
        for (int k = 0; k < 4; ++k)
        {
            const double coef = a[k] * a[k];
            const double y = coef * (x + s[k].y2) - s[k].x2;
            s[k].x2 = s[k].x1; s[k].x1 = x;
            s[k].y2 = s[k].y1; s[k].y1 = y;
            x = y;
        }
        return x;
    }

    struct ChannelState
    {
        Section s0[4], s1[4];
        double a1 = 0.0; // one-sample delay on the first path (gives the 90 degree pair)

        double shift (double x, double c, double s) noexcept
        {
            const double a = run (s0, kA0, x);
            const double b = run (s1, kA1, x);
            const double ad = a1;
            a1 = a;
            return ad * c + kSign * b * s;
        }
    };

    static constexpr double kSign = 1.0; // verified by test: +shiftHz moves frequencies up

    double sr_ = 48000.0, phase_ = 0.0, mix_ = 0.0;
    bool dirty_ = false;
    ChannelState ch_[kMaxChannels];
};
} // namespace fd
