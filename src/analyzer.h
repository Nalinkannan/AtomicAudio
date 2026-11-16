#pragma once

#include <QList>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVector>
#include <complex>
#include <vector>

// A live spectrum of a sink's monitor, read through `pw-record` so the app
// never links libpipewire. Bands are log-spaced from 30 Hz to 20 kHz and
// normalized to 0-1 for drawing.
class Analyzer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(QList<qreal> bands READ bands NOTIFY bandsChanged)
    Q_PROPERTY(bool receiving READ receiving NOTIFY bandsChanged)

public:
    static constexpr int kBands = 56;
    static constexpr int kFft = 4096;

    explicit Analyzer(QObject *parent = nullptr);
    ~Analyzer() override;

    bool active() const { return m_active; }
    void setActive(bool active);
    QString target() const { return m_target; }
    void setTarget(const QString &target);
    QList<qreal> bands() const { return m_bands; }
    bool receiving() const { return m_receiving; }

    // Fed directly by tests; the process feeds it otherwise.
    void feed(const float *interleavedStereo, int frames);
    void analyze();

    static void fft(std::vector<std::complex<float>> &data);

signals:
    void activeChanged();
    void targetChanged();
    void bandsChanged();

private:
    void restart();
    void stopProcess();

    bool m_active = false;
    bool m_receiving = false;
    QString m_target;
    QProcess m_process;
    QByteArray m_pending;
    std::vector<float> m_ring;
    size_t m_ringPos = 0;
    qint64 m_lastData = 0;
    QList<qreal> m_bands;
    std::vector<float> m_window;
    QTimer m_frame;
    QTimer m_retry;
};
