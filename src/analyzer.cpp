#include "analyzer.h"

#include <QDateTime>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kRate = 48000.0;
constexpr double kLowHz = 30.0, kHighHz = 20000.0;
constexpr double kFloorDb = -78.0, kTopDb = -12.0;
} // namespace

Analyzer::Analyzer(QObject *parent) : QObject(parent) {
    m_ring.assign(kFft, 0.0f);
    m_window.resize(kFft);
    for (int i = 0; i < kFft; ++i)
        m_window[i] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * i / (kFft - 1)));
    for (int i = 0; i < kBands; ++i)
        m_bands << 0.0;

    m_frame.setInterval(33);
    connect(&m_frame, &QTimer::timeout, this, &Analyzer::analyze);

    m_retry.setSingleShot(true);
    m_retry.setInterval(2000);
    connect(&m_retry, &QTimer::timeout, this, &Analyzer::restart);

    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        m_pending += m_process.readAllStandardOutput();
        const int frameBytes = int(2 * sizeof(float));
        const int frames = m_pending.size() / frameBytes;
        if (frames == 0)
            return;
        feed(reinterpret_cast<const float *>(m_pending.constData()), frames);
        m_pending.remove(0, frames * frameBytes);
    });
    connect(&m_process, &QProcess::finished, this, [this] {
        if (m_active)
            m_retry.start();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && m_active)
            m_retry.start();
    });
}

Analyzer::~Analyzer() { stopProcess(); }

void Analyzer::setActive(bool active) {
    if (active == m_active)
        return;
    m_active = active;
    emit activeChanged();
    restart();
}

void Analyzer::setTarget(const QString &target) {
    if (target == m_target)
        return;
    m_target = target;
    emit targetChanged();
    restart();
}

void Analyzer::stopProcess() {
    m_retry.stop();
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(500);
    }
    m_pending.clear();
}

void Analyzer::restart() {
    stopProcess();
    if (!m_active || m_target.isEmpty()) {
        m_frame.stop();
        for (auto &b : m_bands)
            b = 0;
        m_receiving = false;
        emit bandsChanged();
        return;
    }
    // Passive, so watching the spectrum never keeps the speakers awake, and no
    // fallback, so a missing target never turns into the microphone.
    m_process.start("pw-record", {"--raw", "--format", "f32", "--rate", "48000", "--channels", "2", "--latency", "15ms",
                                  "-P",
                                  "{ stream.capture.sink = true node.passive = true node.dont-fallback = true "
                                  "node.dont-reconnect = true node.name = atomicaudio.analyzer }",
                                  "--target", m_target, "-"});
    m_frame.start();
}

void Analyzer::feed(const float *data, int frames) {
    for (int i = 0; i < frames; ++i) {
        m_ring[m_ringPos] = 0.5f * (data[2 * i] + data[2 * i + 1]);
        m_ringPos = (m_ringPos + 1) % kFft;
    }
    m_lastData = QDateTime::currentMSecsSinceEpoch();
}

void Analyzer::fft(std::vector<std::complex<float>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = float(-2.0 * M_PI / double(len));
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

void Analyzer::analyze() {
    const bool fresh = QDateTime::currentMSecsSinceEpoch() - m_lastData < 250;
    QList<qreal> next = m_bands;
    if (fresh) {
        std::vector<std::complex<float>> buf(kFft);
        for (int i = 0; i < kFft; ++i)
            buf[i] = {m_ring[(m_ringPos + i) % kFft] * m_window[i], 0.0f};
        fft(buf);
        // Hann window coherent gain is 0.5, so a full-scale sine reads 0 dB.
        const double norm = 2.0 / (0.5 * kFft);
        const double binHz = kRate / kFft;
        for (int b = 0; b < kBands; ++b) {
            const double lo = kLowHz * std::pow(kHighHz / kLowHz, double(b) / kBands);
            const double hi = kLowHz * std::pow(kHighHz / kLowHz, double(b + 1) / kBands);
            int i0 = std::max(1, int(std::floor(lo / binHz))), i1 = std::max(i0, int(std::ceil(hi / binHz)));
            i1 = std::min(i1, kFft / 2 - 1);
            double peak = 0;
            for (int i = i0; i <= i1; ++i)
                peak = std::max(peak, double(std::abs(buf[i])) * norm);
            const double db = 20.0 * std::log10(peak + 1e-12);
            const double v = std::clamp((db - kFloorDb) / (kTopDb - kFloorDb), 0.0, 1.0);
            // Rise at once, fall gently, like a meter.
            next[b] = v > next[b] ? v : next[b] * 0.86 + v * 0.14;
        }
    } else {
        // Nothing playing: let the bars fall, then stop redrawing at all.
        bool any = false;
        for (auto &v : next) {
            v = v < 0.002 ? 0.0 : v * 0.82;
            any = any || v > 0;
        }
        if (!any && !m_receiving && m_bands == next)
            return;
    }
    m_receiving = fresh;
    m_bands = next;
    emit bandsChanged();
}
