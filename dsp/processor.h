// The Atomic Audio signal chain: leveler, speaker optimizer, virtual bass,
// volume-aware loudness contour, dialogue enhancer, surround virtualizer,
// 10-band EQ, three-band loudness compressor and a lookahead peak limiter.
// Plain C++ with no allocation after construction, so it can run inside
// PipeWire's real-time thread.
#pragma once

#include "biquad.h"
#include "params.h"
#include "profiles.h"

#include <array>
#include <vector>

namespace atomic {

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

    // Crossover points of the loudness compressor.
    static constexpr double kLowSplit = 200.0;
    static constexpr double kHighSplit = 3000.0;
    static constexpr int kLoudBands = 3;

private:
    using Stereo = std::array<Biquad, 2>;

    void smoothParams(unsigned frames);
    void updateFilters();
    void processChunk(const float *inL, const float *inR, float *outL, float *outR, unsigned n);
    double truePeak(double l, double r);
    void limit(double &l, double &r, double ceiling);

    double m_fs;
    std::array<float, ParamCount> m_target{};
    std::array<double, ParamCount> m_smooth{};
    std::array<double, ParamCount> m_applied{};
    int m_profile = -1;
    bool m_first = true;

    // Leveler: K-weighted loudness of both channels drives a slow gain.
    Stereo m_levHp, m_levShelf;
    double m_levMs = 0, m_levGainDb = 0;

    // Speaker optimizer.
    Stereo m_tunHp1, m_tunHp2;
    std::array<Stereo, 5> m_tun;

    // Virtual bass: the band the speakers can't play is turned into
    // harmonics they can, and shelves add what they can play for real.
    Biquad m_bLp1, m_bLp2, m_bHp, m_hHp1, m_hHp2, m_hLp;
    double m_bEnv = 0, m_bEnvRel = 0, m_harmGain = 0;
    int m_bHold = 0, m_bHoldLen = 0;
    Stereo m_shelfSpk, m_shelfHp;

    // Equal-loudness contour that follows the output volume.
    Stereo m_contourLow, m_contourHigh;

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
    std::array<OnePole, 2> m_ambLp;
    Stereo m_xtLp;
    std::array<OnePole, 2> m_cfLp;
    static constexpr int kRing = 64;
    std::array<std::array<double, kRing>, 2> m_xtRing{}, m_cfRing{};
    int m_ringPos = 0, m_xtDelay = 3, m_cfDelay = 14;

    std::array<Stereo, EqBands> m_eq;

    // Three-band loudness compressor on Linkwitz-Riley crossovers, so a
    // kick drum no longer pushes the voices down.
    struct Split {
        Biquad lowA, lowB, highA, highB;
    };
    std::array<Split, 2> m_split1, m_split2; // per channel, at kLowSplit and kHighSplit
    Stereo m_lowAllpass;                     // keeps the low band in phase with the other two
    std::array<double, kLoudBands> m_bandMs{}, m_bandGrDb{};

    // Lookahead limiter: hold-min of the needed gain, release, then a box
    // filter as long as the lookahead so the gain is down before the peak.
    // The detector looks at the waveform 4x oversampled, so peaks that fall
    // between samples (and come back when the DAC reconstructs them) count
    // too. Interpolating needs a few samples past the peak, which the audio
    // delay adds on top of the lookahead: m_look = m_attack + kTpDelay.
    static constexpr int kTpOver = 4, kTpTaps = 12, kTpDelay = kTpTaps / 2 - 1;
    std::array<std::array<double, kTpTaps>, kTpOver - 1> m_tpCoef{};
    std::array<std::array<double, 2 * kTpTaps>, 2> m_tpHist{};
    int m_tpPos = 0;
    int m_attack = 96, m_look = 96 + kTpDelay;
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
