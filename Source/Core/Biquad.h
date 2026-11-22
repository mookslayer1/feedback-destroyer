#pragma once
#include <cmath>

namespace fd
{
// Biquad coefficients, a0 normalised to 1. Double precision: narrow low-frequency
// filters put poles very close to the unit circle and float state gets noisy.
struct Coeffs
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    bool isIdentity() const noexcept
    {
        return b0 == 1.0 && b1 == 0.0 && b2 == 0.0 && a1 == 0.0 && a2 == 0.0;
    }
};

// RBJ peaking EQ. Negative gainDb gives a notch whose depth is set by the gain while the
// bandwidth is set by q, so depth can be changed without touching the pole radius.
inline Coeffs designPeaking (double fs, double f0, double q, double gainDb)
{
    const double A     = std::pow (10.0, gainDb / 40.0);
    const double w0    = 2.0 * 3.14159265358979323846 * f0 / fs;
    const double cw    = std::cos (w0);
    const double alpha = std::sin (w0) / (2.0 * q);
    const double a0    = 1.0 + alpha / A;

    Coeffs c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = (-2.0 * cw) / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = (-2.0 * cw) / a0;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}
} // namespace fd
