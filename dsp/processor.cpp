#include "processor.h"

#include <algorithm>
#include <cmath>

#if defined(__x86_64__) || defined(__SSE2__)
#include <xmmintrin.h>
#define ATOMIC_HAVE_CSR 1
#endif

namespace atomic {

static_assert(kParams[SpeakerCurve].max == float(kSpeakerProfiles.size() - 1), "Profile range must cover every profile");

namespace {

// Parameters and gains move once per chunk; small enough that the steps are
// inaudible, large enough that coefficient updates cost nothing.
constexpr unsigned kChunk = 32;

// Where the leveler steers the K-weighted loudness.
constexpr double kLevelerTargetDb = -20.0;

// Butterworth Q values: one section is a 2nd-order Butterworth, two
// sections with these Qs a 4th-order one.
constexpr double kQ2 = 0.70710678;
constexpr double kQ4a = 0.5411961, kQ4b = 1.3065630;

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double timeCoef(double frames, double seconds, double fs) { return 1.0 - std::exp(-frames / (seconds * fs)); }

// Soft-knee compressor curve: gain change in dB for a level `over` dB above
// the threshold.
inline double compressDb(double over, double ratio, double knee) {
    if (2 * over <= -knee)
        return 0;
    if (2 * std::fabs(over) < knee)
        return (1.0 / ratio - 1.0) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
    return (1.0 / ratio - 1.0) * over;
}

// Per band: detector time, attack and release in seconds. Low frequencies
// move slowly so the gain doesn't follow individual bass cycles.
constexpr double kBandDetect[3] = {0.030, 0.012, 0.006};
constexpr double kBandAttack[3] = {0.020, 0.008, 0.004};
constexpr double kBandRelease[3] = {0.300, 0.180, 0.120};

} // namespace

Processor::Processor(double sampleRate) : m_fs(sampleRate > 1000 ? sampleRate : 48000) {
    for (int i = 0; i < ParamCount; ++i) {
        m_target[i] = kParams[i].def;
        m_smooth[i] = kParams[i].def;
        m_applied[i] = NAN;
    }

    const double fs = m_fs;
    m_look = std::max(8, int(std::lround(0.002 * fs)));
    m_xtDelay = std::clamp(int(std::lround(70e-6 * fs)), 1, kRing - 1);
    m_cfDelay = std::clamp(int(std::lround(300e-6 * fs)), 1, kRing - 1);

    for (int ch = 0; ch < 2; ++ch) {
        m_levHp[ch].k = highpass(fs, 60, 0.5);
        m_levShelf[ch].k = highshelf(fs, 1500, 0.7, 4.0);
        m_xtLp[ch].k = lowpass(fs, 5000, kQ2);
        m_cfLp[ch].setLowpass(fs, 700);
        m_ambLp[ch].setLowpass(fs, 7000);

        // Linkwitz-Riley 4th order = two identical Butterworth sections.
        for (Split *split : {&m_split1[ch], &m_split2[ch]}) {
            const double f = split == &m_split1[ch] ? kLowSplit : kHighSplit;
            split->lowA.k = split->lowB.k = lowpass(fs, f, kQ2);
            split->highA.k = split->highB.k = highpass(fs, f, kQ2);
        }
        m_lowAllpass[ch].k = allpass(fs, kHighSplit, kQ2);
    }

    // Everything under 120 Hz feeds the harmonic generator; its output keeps
    // only what the speakers reproduce (100-480 Hz).
    m_bLp1.k = lowpass(fs, 120, kQ4a);
    m_bLp2.k = lowpass(fs, 120, kQ4b);
    m_bHp.k = highpass(fs, 30, kQ2);
    m_hHp1.k = highpass(fs, 100, kQ4a);
    m_hHp2.k = highpass(fs, 100, kQ4b);
    m_hLp.k = lowpass(fs, 480, kQ2);
    // The envelope jumps to each peak and holds it for longer than the
    // slowest bass period (33 Hz), so the normalized signal never clips.
    m_bHoldLen = int(std::lround(0.032 * fs));
    m_bEnvRel = timeCoef(1, 0.12, fs);

    m_sideLp.setLowpass(fs, 300);

    // Mutually prime allpass lengths per side, so the two ambience signals are
    // decorrelated from each other and from the direct sound.
    const double apMs[2][3] = {{4.77, 3.59, 12.73}, {5.21, 3.97, 13.61}};
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < 3; ++i) {
            m_ap[ch][i].buf.assign(std::max<size_t>(1, size_t(std::lround(apMs[ch][i] * fs / 1000.0))), 0.0);
            m_ap[ch][i].g = 0.6;
        }

    m_relCoef = timeCoef(1, 0.08, fs);
    for (int ch = 0; ch < 2; ++ch) {
        m_limDelay[ch].assign(m_look, 0.0);
        m_dry[ch].assign(m_look, 0.0);
    }
    m_box.assign(m_look, 1.0);
    size_t cap = 1;
    while (cap < size_t(m_look) + 4)
        cap <<= 1;
    m_dqVal.assign(cap, 1.0);
    m_dqIdx.assign(cap, 0);
    m_dqMask = cap - 1;

    reset();
}

void Processor::reset() {
    for (Stereo *pair : {&m_levHp, &m_levShelf, &m_tunHp1, &m_tunHp2, &m_shelfSpk, &m_shelfHp, &m_contourLow,
                         &m_contourHigh, &m_xtLp, &m_lowAllpass})
        for (auto &f : *pair)
            f.reset();
    for (auto &band : m_tun)
        for (auto &f : band)
            f.reset();
    for (auto &band : m_eq)
        for (auto &f : band)
            f.reset();
    for (auto *splits : {&m_split1, &m_split2})
        for (auto &s : *splits)
            for (Biquad *f : {&s.lowA, &s.lowB, &s.highA, &s.highB})
                f->reset();
    for (Biquad *f : {&m_bLp1, &m_bLp2, &m_bHp, &m_hHp1, &m_hHp2, &m_hLp, &m_dPres, &m_dLow})
        f->reset();
    m_levMs = 0;
    m_levGainDb = 0;
    m_bEnv = 0;
    m_bHold = 0;
    m_sideLp.reset();
    for (auto &ch : m_ap)
        for (auto &ap : ch) {
            std::fill(ap.buf.begin(), ap.buf.end(), 0.0);
            ap.pos = 0;
        }
    for (auto *poles : {&m_cfLp, &m_ambLp})
        for (auto &f : *poles)
            f.reset();
    for (int ch = 0; ch < 2; ++ch) {
        m_xtRing[ch].fill(0);
        m_cfRing[ch].fill(0);
        std::fill(m_limDelay[ch].begin(), m_limDelay[ch].end(), 0.0);
        std::fill(m_dry[ch].begin(), m_dry[ch].end(), 0.0);
    }
    m_ringPos = 0;
    m_bandMs.fill(0);
    m_bandGrDb.fill(0);
    std::fill(m_box.begin(), m_box.end(), 1.0);
    m_boxSum = m_look;
    m_boxPos = m_limPos = 0;
    m_dqHead = m_dqTail = 0;
    m_limIndex = 0;
    m_relState = 1;
    m_lastGain = 1;
    m_wet = m_target[Bypass] > 0.5f ? 0.0 : 1.0;
    m_first = true;
}

void Processor::setParam(int index, float value) {
    if (index < 0 || index >= ParamCount)
        return;
    const auto &info = kParams[index];
    if (!std::isfinite(value))
        value = info.def;
    m_target[index] = std::clamp(value, info.min, info.max);
}

double Processor::limiterGainDb() const { return 20.0 * std::log10(std::max(m_lastGain, 1e-9)); }

void Processor::smoothParams(unsigned frames) {
    const double alpha = m_first ? 1.0 : timeCoef(frames, 0.05, m_fs);
    m_first = false;
    for (int i = 0; i < ParamCount; ++i) {
        const double target = m_target[i];
        double &v = m_smooth[i];
        v += (target - v) * alpha;
        if (std::fabs(target - v) < 1e-4)
            v = target;
    }
}

void Processor::updateFilters() {
    auto changed = [this](int p) { return !(std::fabs(m_smooth[p] - m_applied[p]) < 1e-5); };
    const bool modeChanged = changed(Headphones);
    const double fs = m_fs, hp = m_smooth[Headphones], spk = 1.0 - hp;

    // The profile is a choice, not a level, so it switches rather than glides.
    const int profile = std::clamp(int(std::lround(m_target[SpeakerCurve])), 0, int(kSpeakerProfiles.size()) - 1);
    const bool profileChanged = profile != m_profile;
    if (profileChanged) {
        m_profile = profile;
        const double f = kSpeakerProfiles[profile].highpass;
        for (int ch = 0; ch < 2; ++ch) {
            m_tunHp1[ch].k = highpass(fs, f, kQ4a);
            m_tunHp2[ch].k = highpass(fs, f, kQ4b);
        }
    }
    if (profileChanged || modeChanged || changed(Tuning)) {
        const double t = m_smooth[Tuning] * spk;
        const auto &bands = kSpeakerProfiles[profile].bands;
        for (size_t b = 0; b < bands.size(); ++b) {
            const auto &band = bands[b];
            const Coeffs k = band.kind == TuningBand::Peak ? peaking(fs, band.freq, band.q, band.db * t)
                                                           : highshelf(fs, band.freq, band.q, band.db * t);
            m_tun[b][0].k = m_tun[b][1].k = k;
        }
    }
    if (modeChanged || changed(Bass)) {
        const double bass = m_smooth[Bass];
        const Coeffs spkShelf = lowshelf(fs, 170, 0.7, 4.0 * bass * spk);
        const Coeffs hpShelf = lowshelf(fs, 90, 0.7, 8.0 * bass * hp);
        for (int ch = 0; ch < 2; ++ch) {
            m_shelfSpk[ch].k = spkShelf;
            m_shelfHp[ch].k = hpShelf;
        }
        m_harmGain = bass * 1.8 * spk;
    }
    if (modeChanged || changed(Bass) || changed(Volume)) {
        // Hearing loses bass (and some treble) faster than mids as the level
        // drops (ISO 226), so turn them up as the volume goes down, by as
        // much as the bass enhancer is turned up. Nothing changes until the
        // volume is 10 dB below full.
        const double quieter = std::max(0.0, -m_smooth[Volume] - 10.0);
        const double boost = std::min(7.0, quieter * 0.3) * m_smooth[Bass];
        const Coeffs low = lowshelf(fs, 150.0 - 70.0 * hp, 0.7, boost);
        const Coeffs high = highshelf(fs, 8000, 0.7, boost * 0.3);
        for (int ch = 0; ch < 2; ++ch) {
            m_contourLow[ch].k = low;
            m_contourHigh[ch].k = high;
        }
    }
    if (changed(Dialogue)) {
        const double d = m_smooth[Dialogue];
        m_dPres.k = peaking(fs, 2200, 0.7, 5.5 * d);
        m_dLow.k = peaking(fs, 220, 1.0, -2.5 * d);
    }
    for (int b = 0; b < EqBands; ++b)
        if (changed(Eq1 + b)) {
            const Coeffs k = peaking(fs, kEqFreqs[b], kEqQ, m_smooth[Eq1 + b]);
            m_eq[b][0].k = m_eq[b][1].k = k;
        }
    m_applied = m_smooth;
}

void Processor::limit(double &l, double &r, double ceiling) {
    const double peak = std::max(std::fabs(l), std::fabs(r));
    const double need = peak > ceiling ? ceiling / peak : 1.0;

    // Sliding minimum over the last look+1 samples (monotonic deque).
    while (m_dqTail != m_dqHead && m_dqVal[(m_dqTail - 1) & m_dqMask] >= need)
        --m_dqTail;
    m_dqVal[m_dqTail & m_dqMask] = need;
    m_dqIdx[m_dqTail & m_dqMask] = m_limIndex;
    ++m_dqTail;
    while (m_dqIdx[m_dqHead & m_dqMask] < m_limIndex - m_look)
        ++m_dqHead;
    const double held = m_dqVal[m_dqHead & m_dqMask];
    ++m_limIndex;

    // Release only lifts the gain slowly and never above the held value, so
    // the box filter below still guarantees no overshoot.
    if (held < m_relState)
        m_relState = held;
    else
        m_relState += (held - m_relState) * m_relCoef;

    m_boxSum += m_relState - m_box[m_boxPos];
    m_box[m_boxPos] = m_relState;
    if (++m_boxPos == size_t(m_look)) {
        // Re-add from scratch once per lap so rounding never accumulates.
        m_boxPos = 0;
        double sum = 0;
        for (double v : m_box)
            sum += v;
        m_boxSum = sum;
    }
    const double gain = std::min(1.0, m_boxSum / m_look);
    m_lastGain = gain;

    const double dl = m_limDelay[0][m_limPos], dr = m_limDelay[1][m_limPos];
    m_limDelay[0][m_limPos] = l;
    m_limDelay[1][m_limPos] = r;
    l = clampd(dl * gain, -ceiling, ceiling);
    r = clampd(dr * gain, -ceiling, ceiling);
}

void Processor::processChunk(const float *inL, const float *inR, float *outL, float *outR, unsigned n) {
    smoothParams(n);
    updateFilters();

    const double fs = m_fs;
    const double hp = m_smooth[Headphones], spk = 1.0 - hp;
    const double lev = m_smooth[Leveler], d = m_smooth[Dialogue], s = m_smooth[Surround], loud = m_smooth[Loudness];
    const double tMix = m_smooth[Tuning] * spk;
    const double ceiling = dbToLin(m_smooth[Ceiling]);

    // Leveler: measure this chunk on both channels first, then glide the
    // gain across it.
    const double msCoef = timeCoef(1, 0.4, fs);
    for (unsigned i = 0; i < n; ++i) {
        const double kl = m_levShelf[0].run(m_levHp[0].run(inL[i]));
        const double kr = m_levShelf[1].run(m_levHp[1].run(inR[i]));
        m_levMs += msCoef * (0.5 * (kl * kl + kr * kr) - m_levMs);
    }
    const double levelDb = 10.0 * std::log10(m_levMs + 1e-12);
    double desired;
    if (lev < 1e-4)
        desired = 0;
    else if (levelDb < -50)
        desired = m_levGainDb; // Silence: hold, so the next sound doesn't burst in.
    else
        desired = clampd((kLevelerTargetDb - levelDb) * 0.85 * lev, -18.0 * lev, 12.0 * lev);
    double levTime;
    if (lev < 1e-4)
        levTime = 0.05;
    else if (desired >= m_levGainDb)
        levTime = 2.0; // Rise slowly, so quiet passages aren't pumped up.
    else if (levelDb + m_levGainDb > kLevelerTargetDb + 6.0)
        levTime = 0.04; // A sudden loud scene after a quiet one: catch it fast.
    else
        levTime = 0.15;
    const double lev0 = dbToLin(m_levGainDb);
    m_levGainDb += (desired - m_levGainDb) * timeCoef(n, levTime, fs);
    const double lev1 = dbToLin(m_levGainDb);

    // Loudness: each band's gain comes from its level over the previous
    // chunk (0.7 ms ago) and glides across this one.
    const double threshold = -18.0 - 10.0 * loud, ratio = 1.0 + 2.5 * loud, knee = 8.0;
    const double makeup = 8.0 * loud;
    std::array<double, kLoudBands> band0{}, band1{}, bandCoef{};
    for (int b = 0; b < kLoudBands; ++b) {
        const double gr = loud > 1e-4 ? compressDb(10.0 * std::log10(m_bandMs[b] + 1e-12) - threshold, ratio, knee) : 0.0;
        band0[b] = dbToLin(m_bandGrDb[b] + makeup);
        m_bandGrDb[b] += (gr - m_bandGrDb[b]) * timeCoef(n, gr < m_bandGrDb[b] ? kBandAttack[b] : kBandRelease[b], fs);
        band1[b] = dbToLin(m_bandGrDb[b] + makeup);
        bandCoef[b] = timeCoef(1, kBandDetect[b], fs);
    }

    const double widen = 1.0 + 1.0 * s * spk + 0.4 * s * hp;
    const double amb = s * (0.28 * spk + 0.18 * hp);
    const double xt = 0.38 * s * spk;
    const double xtNorm = 1.0 / (1.0 - 0.5 * xt);
    const double cf = 0.45 * s * hp;
    const double sideKeep = 1.0 - 0.2 * d;
    const double wetTarget = m_target[Bypass] > 0.5f ? 0.0 : 1.0;
    const double wetStep = 1.0 / (0.02 * fs);
    constexpr int mask = kRing - 1;

    for (unsigned i = 0; i < n; ++i) {
        const double frac = double(i + 1) / n;
        const double inl = inL[i], inr = inR[i];
        double l = inl, r = inr;

        const double g = lev0 + (lev1 - lev0) * frac;
        l *= g;
        r *= g;

        // Virtual bass: normalize the low band by its envelope so the
        // Chebyshev polynomials give clean 2nd/3rd/4th harmonics at any level.
        const double low = m_bHp.run(m_bLp2.run(m_bLp1.run(0.5 * (l + r))));
        const double a = std::fabs(low);
        if (a >= m_bEnv) {
            m_bEnv = a;
            m_bHold = m_bHoldLen;
        } else if (m_bHold > 0) {
            --m_bHold;
        } else {
            m_bEnv += m_bEnvRel * (a - m_bEnv);
        }
        const double x = clampd(low / (m_bEnv + 1e-9), -1.0, 1.0), x2 = x * x;
        const double h = 0.5 * (2 * x2 - 1) + 0.4 * x * (4 * x2 - 3) + 0.2 * (8 * x2 * x2 - 8 * x2 + 1);
        const double harm = m_harmGain * m_hLp.run(m_hHp2.run(m_hHp1.run(h * m_bEnv)));

        double chans[2] = {l, r};
        for (int ch = 0; ch < 2; ++ch) {
            double v = chans[ch];
            const double hpv = m_tunHp2[ch].run(m_tunHp1[ch].run(v));
            v += tMix * (hpv - v);
            for (auto &band : m_tun)
                v = band[ch].run(v);
            v = m_shelfHp[ch].run(m_shelfSpk[ch].run(v));
            v = m_contourHigh[ch].run(m_contourLow[ch].run(v));
            chans[ch] = v + harm;
        }
        l = chans[0];
        r = chans[1];

        // Mid/side: dialogue lives in the mid, space in the side.
        double mid = 0.5 * (l + r), side = 0.5 * (l - r) * sideKeep;
        mid = m_dLow.run(m_dPres.run(mid));
        const double sl = m_sideLp.run(side), sh = side - sl;
        side = sl + sh * widen;
        double aL = sh, aR = sh;
        for (auto &ap : m_ap[0])
            aL = ap.run(aL);
        for (auto &ap : m_ap[1])
            aR = ap.run(aR);
        // Darken the ambience a little: real rooms absorb the top end, and
        // bright diffuse sound reads as metallic.
        aL = m_ambLp[0].run(aL);
        aR = m_ambLp[1].run(aR);
        l = mid + side + amb * aL;
        r = mid - side - amb * aR;

        // Speakers: cancel part of each speaker's sound reaching the far ear,
        // which pushes the image past the edges of the laptop.
        m_xtRing[0][m_ringPos] = l;
        m_xtRing[1][m_ringPos] = r;
        const int xi = (m_ringPos - m_xtDelay) & mask;
        const double cl = m_xtLp[0].run(m_xtRing[1][xi]), cr = m_xtLp[1].run(m_xtRing[0][xi]);
        l = (l - xt * cl) * xtNorm;
        r = (r - xt * cr) * xtNorm;

        // Headphones: crossfeed like speakers in a room, so sound sits in
        // front of you instead of inside your head.
        m_cfRing[0][m_ringPos] = l;
        m_cfRing[1][m_ringPos] = r;
        const int ci = (m_ringPos - m_cfDelay) & mask;
        const double fl = m_cfLp[0].run(m_cfRing[1][ci]), fr = m_cfLp[1].run(m_cfRing[0][ci]);
        l = (l + cf * fl) / (1.0 + cf);
        r = (r + cf * fr) / (1.0 + cf);
        m_ringPos = (m_ringPos + 1) & mask;

        for (auto &band : m_eq) {
            l = band[0].run(l);
            r = band[1].run(r);
        }

        // Three bands that add back up to the input (up to phase).
        double bands[2][kLoudBands];
        for (int ch = 0; ch < 2; ++ch) {
            const double v = ch == 0 ? l : r;
            Split &s1 = m_split1[ch], &s2 = m_split2[ch];
            const double lowBand = m_lowAllpass[ch].run(s1.lowB.run(s1.lowA.run(v)));
            const double rest = s1.highB.run(s1.highA.run(v));
            bands[ch][0] = lowBand;
            bands[ch][1] = s2.lowB.run(s2.lowA.run(rest));
            bands[ch][2] = s2.highB.run(s2.highA.run(rest));
        }
        l = r = 0;
        for (int b = 0; b < kLoudBands; ++b) {
            const double bl = bands[0][b], br = bands[1][b];
            m_bandMs[b] += bandCoef[b] * (0.5 * (bl * bl + br * br) - m_bandMs[b]);
            const double bg = band0[b] + (band1[b] - band0[b]) * frac;
            l += bl * bg;
            r += br * bg;
        }

        limit(l, r, ceiling);

        const double dl = m_dry[0][m_limPos], dr = m_dry[1][m_limPos];
        m_dry[0][m_limPos] = inl;
        m_dry[1][m_limPos] = inr;
        if (++m_limPos == size_t(m_look))
            m_limPos = 0;

        m_wet += clampd(wetTarget - m_wet, -wetStep, wetStep);
        outL[i] = float(dl + (l - dl) * m_wet);
        outR[i] = float(dr + (r - dr) * m_wet);
    }

    // A non-finite value would poison every filter forever; start over.
    bool finite = std::isfinite(m_levMs) && std::isfinite(m_bEnv);
    for (double ms : m_bandMs)
        finite = finite && std::isfinite(ms);
    for (unsigned i = 0; i < n && finite; ++i)
        finite = std::isfinite(outL[i]) && std::isfinite(outR[i]);
    if (!finite) {
        reset();
        std::fill(outL, outL + n, 0.0f);
        std::fill(outR, outR + n, 0.0f);
    }
}

void Processor::process(const float *inL, const float *inR, float *outL, float *outR, unsigned frames) {
#ifdef ATOMIC_HAVE_CSR
    // Flush denormals to zero: decaying filter tails would otherwise crawl.
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040);
#endif
    for (unsigned done = 0; done < frames;) {
        const unsigned n = std::min(kChunk, frames - done);
        processChunk(inL + done, inR + done, outL + done, outR + done, n);
        done += n;
    }
#ifdef ATOMIC_HAVE_CSR
    _mm_setcsr(csr);
#endif
}

} // namespace atomic
