// Second-order filters from Robert Bristow-Johnson's Audio EQ Cookbook, in
// transposed direct form II with double-precision state so the 30 Hz bands
// stay clean at 48 kHz. The app uses the same coefficient math to draw the
// EQ curve, so what you see is exactly what the engine does.
#pragma once

#include <cmath>
#include <complex>

namespace atomic {

struct Coeffs {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

inline Coeffs normalized(double b0, double b1, double b2, double a0, double a1, double a2) {
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

inline double omega(double fs, double f) {
    // Keep the corner below Nyquist so a 16 kHz band at 32 kHz stays stable.
    return 2.0 * M_PI * std::fmin(f, fs * 0.49) / fs;
}

inline Coeffs lowpass(double fs, double f, double q) {
    const double w = omega(fs, f), c = std::cos(w), al = std::sin(w) / (2 * q);
    return normalized((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}

inline Coeffs highpass(double fs, double f, double q) {
    const double w = omega(fs, f), c = std::cos(w), al = std::sin(w) / (2 * q);
    return normalized((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}

// Second-order all-pass; with Q = 1/sqrt(2) it has exactly the phase of a
// Linkwitz-Riley crossover's summed output, so bands split at one frequency
// stay in phase with bands split at another.
inline Coeffs allpass(double fs, double f, double q) {
    const double w = omega(fs, f), c = std::cos(w), al = std::sin(w) / (2 * q);
    return normalized(1 - al, -2 * c, 1 + al, 1 + al, -2 * c, 1 - al);
}

inline Coeffs peaking(double fs, double f, double q, double db) {
    if (std::fabs(db) < 1e-6)
        return {};
    const double A = std::pow(10.0, db / 40), w = omega(fs, f), c = std::cos(w), al = std::sin(w) / (2 * q);
    return normalized(1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

inline Coeffs lowshelf(double fs, double f, double q, double db) {
    if (std::fabs(db) < 1e-6)
        return {};
    const double A = std::pow(10.0, db / 40), w = omega(fs, f), c = std::cos(w);
    const double s = 2 * std::sqrt(A) * std::sin(w) / (2 * q);
    return normalized(A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s),
                      (A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s);
}

inline Coeffs highshelf(double fs, double f, double q, double db) {
    if (std::fabs(db) < 1e-6)
        return {};
    const double A = std::pow(10.0, db / 40), w = omega(fs, f), c = std::cos(w);
    const double s = 2 * std::sqrt(A) * std::sin(w) / (2 * q);
    return normalized(A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s),
                      (A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s);
}

inline double magnitudeDb(const Coeffs &k, double f, double fs) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * M_PI * f / fs), z2 = z1 * z1;
    const auto h = (k.b0 + k.b1 * z1 + k.b2 * z2) / (1.0 + k.a1 * z1 + k.a2 * z2);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-12));
}

struct Biquad {
    Coeffs k;
    double z1 = 0, z2 = 0;

    inline double run(double x) {
        const double y = k.b0 * x + z1;
        z1 = k.b1 * x - k.a1 * y + z2;
        z2 = k.b2 * x - k.a2 * y;
        return y;
    }
    void reset() { z1 = z2 = 0; }
};

struct OnePole {
    double a = 1, y = 0;
    void setLowpass(double fs, double f) { a = 1.0 - std::exp(-2.0 * M_PI * f / fs); }
    inline double run(double x) { return y += a * (x - y); }
    void reset() { y = 0; }
};

} // namespace atomic
