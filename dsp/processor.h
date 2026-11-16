// The Atomic Audio signal chain: leveler, speaker optimizer, virtual bass,
// dialogue enhancer, surround virtualizer, 10-band EQ, loudness compressor and
// a lookahead peak limiter. Plain C++ with no allocation after construction,
// so it can run inside PipeWire's real-time thread.
#pragma once

#include "biquad.h"
#include "params.h"

#include <array>
#include <vector>

namespace atomic {

// The speaker optimizer for the ASUS ROG Zephyrus G14 (GA403), measured with
// its own microphone: the amps cut everything under ~95 Hz, the woofer cavity
// rings at 630 Hz (+12 dB, Q ~4), and the woofer/tweeter handover leaves a dip
// around 2.5 kHz. Scaled by `amount` so the optimizer can fade in and out.
struct TuningBand {
    enum Kind { Peak, HighShelf } kind;
    double freq, q, db;
};
inline constexpr std::array<TuningBand, 5> kSpeakerTuning = {{
    {TuningBand::Peak, 150, 0.9, 2.0},
    {TuningBand::Peak, 630, 4.0, -7.0},
    {TuningBand::Peak, 1200, 1.2, -2.0},
    {TuningBand::Peak, 2500, 1.5, 2.5},
    {TuningBand::HighShelf, 11000, 0.7, 1.5},
}};
inline constexpr double kSpeakerHighpass = 80.0;

class Processor {
public:
    explicit Processor(double sampleRate);

    void reset();
    void setParam(int index, float value);
    float param(int index) const { return m_target[index]; }

    // Safe in place: every frame reads both inputs before writing an output.
    void process(const float *inL, const float *inR, float *outL, float *outR, unsigned frames);

    int latency() const { return m_look; }
    double sampleRate() const { return m_fs; }
    double limiterGainDb() const;

private:
    void smoothParams(unsigned frames);
    void updateFilters();
    void processChunk(const float *inL, const float *inR, float *outL, float *outR, unsigned n);
    double limit(double &l, double &r);

    double m_fs;
    std::array<float, ParamCount> m_target{};
    std::array<double, ParamCount> m_smooth{};
    std::array<double, ParamCount> m_applied{};
    bool m_first = true;

    // Leveler: K-weighted loudness of the mid signal drives a slow gain.
    Biquad m_levHp, m_levShelf;
    double m_levMs = 0, m_levGainDb = 0;

    // Speaker optimizer.
    std::array<Biquad, 2> m_tunHp1, m_tunHp2;
    std::array<std::array<Biquad, 2>, kSpeakerTuning.size()> m_tun;

    // Virtual bass: the band the speakers can't play is turned into
    // harmonics they can, and shelves add what they can play for real.
    Biquad m_bLp1, m_bLp2, m_bHp, m_hHp1, m_hHp2, m_hLp;
    double m_bEnv = 0, m_bEnvAtk = 0, m_bEnvRel = 0, m_harmGain = 0;
    std::array<Biquad, 2> m_shelfSpk, m_shelfHp;

    // Dialogue enhancer works on the mid (center) signal.
    Biquad m_dPres, m_dLow;

    // Surround: widened side, decorrelated ambience, and crosstalk
    // cancellation on speakers or crossfeed on headphones.
    OnePole m_sideLp;
    struct Allpass {
        std::vector<double> buf;
        size_t pos = 0;
        double g = 0.5;
        double run(double x) {
            const double d = buf[pos];
            const double y = -g * x + d;
            buf[pos] = x + g * y;
            pos = (pos + 1) % buf.size();
            return y;
        }
    };
    std::array<std::array<Allpass, 3>, 2> m_ap;
    std::array<Biquad, 2> m_xtLp;
    std::array<OnePole, 2> m_cfLp;
    static constexpr int kRing = 64;
    std::array<std::array<double, kRing>, 2> m_xtRing{}, m_cfRing{};
    int m_ringPos = 0, m_xtDelay = 3, m_cfDelay = 14;

    std::array<std::array<Biquad, 2>, EqBands> m_eq;

    // Loudness compressor (stereo-linked RMS).
    double m_compMs = 0, m_compGrDb = 0;

    // Lookahead limiter: hold-min of the needed gain, release, then a box
    // filter as long as the lookahead so the gain is down before the peak.
    int m_look = 96;
    std::vector<double> m_limDelay[2];
    std::vector<double> m_box;
    std::vector<double> m_dqVal;
    std::vector<long long> m_dqIdx;
    size_t m_dqHead = 0, m_dqTail = 0, m_dqMask = 0;
    long long m_limIndex = 0;
    size_t m_limPos = 0, m_boxPos = 0;
    double m_boxSum = 0, m_relState = 1, m_relCoef = 0, m_lastGain = 1;

    // Bypass crossfades against the input, delayed to line up with the limiter.
    std::vector<double> m_dry[2];
    double m_wet = 1;
};

} // namespace atomic
