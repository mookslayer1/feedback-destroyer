#pragma once
#include <cmath>

// One narrow cut filter (a peaking filter with negative gain), used as a notch.
// Written by hand, in double precision, so it never allocates memory on the
// audio thread and stays accurate at low frequencies with a high Q.
struct Biquad
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;

    void setPeak (double sampleRate, double freq, double q, double gainDb)
    {
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double w0    = 6.283185307179586 * freq / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c     = std::cos (w0);
        const double a0    = 1.0 + alpha / A;

        b0 = (1.0 + alpha * A) / a0;
        b1 = (-2.0 * c) / a0;
        b2 = (1.0 - alpha * A) / a0;
        a1 = b1;
        a2 = (1.0 - alpha / A) / a0;
    }

    void reset() { z1 = z2 = 0.0; }

    float process (float x)
    {
        const double in = x;
        const double y  = b0 * in + z1;
        z1 = b1 * in - a1 * y + z2;
        z2 = b2 * in - a2 * y;
        return (float) y;
    }
};
