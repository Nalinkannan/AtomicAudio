#include "processor.h"

#include <algorithm>
#include <cmath>

#if defined(__x86_64__) || defined(__SSE2__)
#include <xmmintrin.h>
#define ATOMIC_HAVE_CSR 1
#endif

namespace atomic {

namespace {

// Parameters and gains move once per chunk; small enough that the steps are
// inaudible, large enough that coefficient updates cost nothing.
constexpr unsigned kChunk = 32;

// Where the leveler steers the K-weighted loudness of the mid signal.
constexpr double kLevelerTargetDb = -20.0;

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double timeCoef(double frames, double seconds, double fs) { return 1.0 - std::exp(-frames / (seconds * fs)); }

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

    m_levHp.k = highpass(fs, 60, 0.5);
    m_levShelf.k = highshelf(fs, 1500, 0.7, 4.0);

    // Fourth-order Butterworth: two sections with Q 0.54 and 1.31.
    for (int ch = 0; ch < 2; ++ch) {
        m_tunHp1[ch].k = highpass(fs, kSpeakerHighpass, 0.5412);
        m_tunHp2[ch].k = highpass(fs, kSpeakerHighpass, 1.3066);
        m_xtLp[ch].k = lowpass(fs, 5000, 0.707);
        m_cfLp[ch].setLowpass(fs, 700);
    }

    // Everything under 120 Hz feeds the harmonic generator; its output keeps
    // only what the speakers reproduce (100-480 Hz).
    m_bLp1.k = lowpass(fs, 120, 0.5412);
    m_bLp2.k = lowpass(fs, 120, 1.3066);
    m_bHp.k = highpass(fs, 30, 0.707);
    m_hHp1.k = highpass(fs, 100, 0.5412);
    m_hHp2.k = highpass(fs, 100, 1.3066);
    m_hLp.k = lowpass(fs, 480, 0.707);
    m_bEnvAtk = timeCoef(1, 0.002, fs);
    m_bEnvRel = timeCoef(1, 0.06, fs);

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
    m_levHp.reset();
    m_levShelf.reset();
    m_levMs = 0;
    m_levGainDb = 0;
    for (auto *pair : {&m_tunHp1, &m_tunHp2, &m_shelfSpk, &m_shelfHp, &m_xtLp})
        for (auto &f : *pair)
            f.reset();
    for (auto &band : m_tun)
        for (auto &f : band)
            f.reset();
    for (auto &band : m_eq)
        for (auto &f : band)
            f.reset();
    for (Biquad *f : {&m_bLp1, &m_bLp2, &m_bHp, &m_hHp1, &m_hHp2, &m_hLp, &m_dPres, &m_dLow})
        f->reset();
    m_bEnv = 0;
    m_sideLp.reset();
    for (auto &ch : m_ap)
        for (auto &ap : ch) {
            std::fill(ap.buf.begin(), ap.buf.end(), 0.0);
            ap.pos = 0;
        }
    for (auto &f : m_cfLp)
        f.reset();
    for (int ch = 0; ch < 2; ++ch) {
        m_xtRing[ch].fill(0);
        m_cfRing[ch].fill(0);
        std::fill(m_limDelay[ch].begin(), m_limDelay[ch].end(), 0.0);
        std::fill(m_dry[ch].begin(), m_dry[ch].end(), 0.0);
    }
    m_ringPos = 0;
    m_compMs = 0;
    m_compGrDb = 0;
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

    if (modeChanged || changed(Tuning)) {
        const double t = m_smooth[Tuning] * spk;
        for (size_t b = 0; b < kSpeakerTuning.size(); ++b) {
            const auto &band = kSpeakerTuning[b];
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

double Processor::limit(double &l, double &r) {
    const double ceiling = dbToLin(m_smooth[Ceiling]);
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
    return gain;
}

void Processor::processChunk(const float *inL, const float *inR, float *outL, float *outR, unsigned n) {
    smoothParams(n);
    updateFilters();

    const double fs = m_fs;
    const double hp = m_smooth[Headphones], spk = 1.0 - hp;
    const double lev = m_smooth[Leveler], d = m_smooth[Dialogue], s = m_smooth[Surround], loud = m_smooth[Loudness];
    const double tMix = m_smooth[Tuning] * spk;

    // Leveler: measure this chunk first, then glide the gain across it.
    const double msCoef = timeCoef(1, 0.4, fs);
    for (unsigned i = 0; i < n; ++i) {
        const double k = m_levShelf.run(m_levHp.run(0.5 * (double(inL[i]) + double(inR[i]))));
        m_levMs += msCoef * (k * k - m_levMs);
    }
    const double levelDb = 10.0 * std::log10(m_levMs + 1e-12);
    double desired;
    if (lev < 1e-4)
        desired = 0;
    else if (levelDb < -50)
        desired = m_levGainDb; // Silence: hold, so the next sound doesn't burst in.
    else
        desired = clampd((kLevelerTargetDb - levelDb) * 0.85 * lev, -18.0 * lev, 12.0 * lev);
    const double levCoef = desired < m_levGainDb ? timeCoef(n, 0.15, fs) : timeCoef(n, lev < 1e-4 ? 0.05 : 2.0, fs);
    const double lev0 = dbToLin(m_levGainDb);
    m_levGainDb += (desired - m_levGainDb) * levCoef;
    const double lev1 = dbToLin(m_levGainDb);

    // Loudness: a soft-knee RMS compressor with makeup gain, measured on the
    // previous chunk (0.7 ms ago), feeding the limiter.
    const double compDb = 10.0 * std::log10(m_compMs + 1e-12);
    const double threshold = -14.0 - 12.0 * loud, ratio = 1.0 + 3.0 * loud, knee = 6.0;
    const double over = compDb - threshold;
    double gr = 0;
    if (loud > 1e-4) {
        if (2 * over <= -knee)
            gr = 0;
        else if (2 * std::fabs(over) < knee)
            gr = (1.0 / ratio - 1.0) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
        else
            gr = (1.0 / ratio - 1.0) * over;
    }
    const double compCoef = gr < m_compGrDb ? timeCoef(n, 0.006, fs) : timeCoef(n, 0.25, fs);
    const double makeup = 9.0 * loud;
    const double comp0 = dbToLin(m_compGrDb + makeup);
    m_compGrDb += (gr - m_compGrDb) * compCoef;
    const double comp1 = dbToLin(m_compGrDb + makeup);
    const double compA = timeCoef(1, 0.01, fs);

    const double widen = 1.0 + 1.0 * s * spk + 0.4 * s * hp;
    const double amb = s * (0.28 * spk + 0.18 * hp);
    const double xt = 0.38 * s * spk;
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
        m_bEnv += (a > m_bEnv ? m_bEnvAtk : m_bEnvRel) * (a - m_bEnv);
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
        l = mid + side + amb * aL;
        r = mid - side - amb * aR;

        // Speakers: cancel part of each speaker's sound reaching the far ear,
        // which pushes the image past the edges of the laptop.
        m_xtRing[0][m_ringPos] = l;
        m_xtRing[1][m_ringPos] = r;
        const int xi = (m_ringPos - m_xtDelay) & mask;
        const double cl = m_xtLp[0].run(m_xtRing[1][xi]), cr = m_xtLp[1].run(m_xtRing[0][xi]);
        const double xtNorm = 1.0 / (1.0 - 0.5 * xt);
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

        m_compMs += compA * (0.5 * (l * l + r * r) - m_compMs);
        const double cg = comp0 + (comp1 - comp0) * frac;
        l *= cg;
        r *= cg;

        limit(l, r);

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
    if (!std::isfinite(m_levMs) || !std::isfinite(m_compMs) || !std::isfinite(m_bEnv) || !std::isfinite(outL[n - 1]) ||
        !std::isfinite(outR[n - 1])) {
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
