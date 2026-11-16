#include "backend.h"
#include "processor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <cmath>

using namespace atomic;

namespace {
constexpr int kHistory = 100;
constexpr double kGraphRate = 48000.0;
} // namespace

Backend::Backend(const QString &settingsPath, QObject *parent, bool manageService)
    : QObject(parent), m_path(settingsPath.isEmpty() ? paths::settingsFile() : settingsPath),
      m_manageService(manageService) {
    QString error;
    m_settings = Settings::load(m_path, &error);
    m_error = error;

    m_save.setSingleShot(true);
    m_save.setInterval(20);
    connect(&m_save, &QTimer::timeout, this, [this] {
        QString err;
        if (!m_settings.save(m_path, &err))
            m_error = tr("Couldn't save settings: %1").arg(err);
        else if (m_error.startsWith(tr("Couldn't save settings")))
            m_error.clear();
        emit engineChanged();
    });

    // The CLI can change settings while the window is open.
    m_reload.setSingleShot(true);
    m_reload.setInterval(60);
    connect(&m_reload, &QTimer::timeout, this, &Backend::loadFromDisk);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reload, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reload, qOverload<>(&QTimer::start));
    watch();

    connect(&m_isActive, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const bool on = status == QProcess::NormalExit && code == 0;
        if (on != m_engineOn) {
            m_engineOn = on;
            emit engineChanged();
        }
    });
    connect(&m_systemctl, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_busy = false;
        if (status != QProcess::NormalExit || code != 0)
            m_error = QString::fromUtf8(m_systemctl.readAllStandardError()).trimmed();
        else
            m_error.clear();
        emit engineChanged();
        refresh();
        // The filter takes a moment to come up after the service starts.
        QTimer::singleShot(800, this, &Backend::refresh);
    });
    connect(&m_systemctl, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart)
            return;
        m_busy = false;
        m_error = tr("systemctl is missing, so the engine can't be started.");
        emit engineChanged();
    });

    m_poll.setInterval(1500);
    connect(&m_poll, &QTimer::timeout, this, &Backend::refresh);
    if (m_manageService) {
        m_poll.start();
        refresh();
    }
    m_editHeadphones = m_status.headphones;
}

void Backend::watch() {
    const auto watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    const QString dir = QFileInfo(m_path).absolutePath();
    QDir().mkpath(dir);
    m_watcher.addPath(dir);
    if (QFileInfo::exists(m_path))
        m_watcher.addPath(m_path);
}

void Backend::loadFromDisk() {
    watch();
    if (m_save.isActive())
        return; // Our own pending write wins.
    QString error;
    const Settings next = Settings::load(m_path, &error);
    if (!error.isEmpty() || next.toJson() == m_settings.toJson())
        return;
    m_settings = next;
    emit profileChanged();
}

void Backend::readStatus() {
    const EngineStatus next = EngineStatus::read(paths::statusFile());
    const bool outputChanged = next.headphones != m_status.headphones;
    const bool differs = next.toJson() != m_status.toJson();
    m_status = next;
    if (outputChanged && !m_editPinned && m_editHeadphones != m_status.headphones) {
        m_editHeadphones = m_status.headphones;
        emit profileChanged();
    }
    if (differs)
        emit engineChanged();
}

void Backend::refresh() {
    if (!m_manageService)
        return;
    readStatus();
    if (m_isActive.state() == QProcess::NotRunning)
        m_isActive.start("systemctl", {"--user", "is-active", "--quiet", service::kUnit});
}

void Backend::runSystemctl(const QStringList &args) {
    if (m_systemctl.state() != QProcess::NotRunning)
        return;
    m_busy = true;
    emit engineChanged();
    m_systemctl.start("systemctl", QStringList{"--user"} + args);
}

void Backend::toggleEngine() {
    if (!m_manageService || m_busy)
        return;
    if (m_engineOn) {
        runSystemctl({"disable", "--now", service::kUnit});
        return;
    }
    // A build that wasn't installed as a package brings its own user unit.
    if (!service::unitInstalled()) {
        QString err;
        if (!service::writeUserUnit(QCoreApplication::applicationFilePath(), &err)) {
            m_error = tr("Couldn't write the service: %1").arg(err);
            emit engineChanged();
            return;
        }
        service::run({"daemon-reload"});
    }
    runSystemctl({"enable", "--now", service::kUnit});
}

void Backend::setEditingHeadphones(bool headphones) {
    m_editPinned = headphones != m_status.headphones;
    if (headphones == m_editHeadphones)
        return;
    m_editHeadphones = headphones;
    emit profileChanged();
}

QList<qreal> Backend::eq() const {
    QList<qreal> list;
    for (double g : profile().eq)
        list << g;
    return list;
}

QStringList Backend::modeIds() const {
    QStringList ids;
    for (const auto &m : Settings::modes())
        ids << m.id;
    return ids << "custom";
}

QStringList Backend::eqPresetIds() const {
    QStringList ids;
    for (const auto &p : Settings::eqPresets())
        ids << p.id;
    return ids << "custom";
}

QList<qreal> Backend::eqFrequencies() const {
    QList<qreal> list;
    for (double f : kEqFreqs)
        list << f;
    return list;
}

void Backend::remember(const QString &key) {
    // A slider drag or a run of key presses is one undo step.
    if (key != m_lastKey || !m_lastChange.isValid() || m_lastChange.elapsed() > 700) {
        m_undo.push_back(m_settings.toJson());
        if (m_undo.size() > kHistory)
            m_undo.removeFirst();
        m_redo.clear();
        emit historyChanged();
    }
    m_lastKey = key;
    m_lastChange.restart();
}

void Backend::changed() {
    m_save.start();
    emit profileChanged();
}

void Backend::restore(const QJsonObject &state) {
    m_settings = Settings::fromJson(state);
    m_lastKey.clear();
    changed();
    emit historyChanged();
}

void Backend::undo() {
    if (m_undo.isEmpty())
        return;
    m_redo.push_back(m_settings.toJson());
    restore(m_undo.takeLast());
}

void Backend::redo() {
    if (m_redo.isEmpty())
        return;
    m_undo.push_back(m_settings.toJson());
    restore(m_redo.takeLast());
}

void Backend::setMode(const QString &id) {
    if (id == profile().mode)
        return;
    Profile next = profile();
    if (!Settings::applyMode(next, id, m_editHeadphones))
        return;
    remember("mode");
    profile() = next;
    changed();
}

void Backend::setAmount(const QString &key, double value) {
    value = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    Profile next = profile();
    double *slot = key == "leveler"    ? &next.leveler
                   : key == "dialogue" ? &next.dialogue
                   : key == "surround" ? &next.surround
                   : key == "bass"     ? &next.bass
                   : key == "loudness" ? &next.loudness
                                       : nullptr;
    if (!slot || std::fabs(*slot - value) < 1e-6)
        return;
    *slot = value;
    // Moving a slider away from a mode's value makes it your own.
    next.mode = "custom";
    remember(key);
    profile() = next;
    changed();
}

void Backend::nudge(const QString &key, double delta) {
    const Profile &p = profile();
    const double current = key == "leveler"    ? p.leveler
                           : key == "dialogue" ? p.dialogue
                           : key == "surround" ? p.surround
                           : key == "bass"     ? p.bass
                           : key == "loudness" ? p.loudness
                                               : -1;
    if (current < 0)
        return;
    setAmount(key, std::round((current + delta) * 100.0) / 100.0);
}

void Backend::setTuning(bool on) {
    if (on == profile().tuning)
        return;
    remember("tuning");
    profile().tuning = on;
    changed();
}

void Backend::setEqBand(int band, double db) {
    if (band < 0 || band >= EqBands)
        return;
    db = std::isfinite(db) ? std::clamp(std::round(db * 2.0) / 2.0, -12.0, 12.0) : 0.0;
    if (profile().eq[band] == db)
        return;
    remember(QStringLiteral("eq%1").arg(band));
    profile().eq[band] = db;
    profile().eqPreset = "custom";
    changed();
}

void Backend::setEqPreset(const QString &id) {
    Profile next = profile();
    if (!Settings::applyEqPreset(next, id) || next == profile())
        return;
    remember("eqPreset");
    profile() = next;
    changed();
}

void Backend::cycleEqPreset() {
    const auto &presets = Settings::eqPresets();
    int index = -1;
    for (int i = 0; i < presets.size(); ++i)
        if (presets[i].id == profile().eqPreset)
            index = i;
    setEqPreset(presets[(index + 1) % presets.size()].id);
}

void Backend::toggleBypass() {
    remember("bypass");
    m_settings.bypass = !m_settings.bypass;
    changed();
}

void Backend::resetProfile() {
    const Profile fresh = Settings::defaultProfile(m_editHeadphones);
    if (fresh == profile())
        return;
    remember("reset");
    profile() = fresh;
    changed();
}

double Backend::frequencyAt(int index, int points) {
    return 20.0 * std::pow(1000.0, double(index) / std::max(1, points - 1));
}

QList<qreal> Backend::eqResponse(int points) const {
    QList<qreal> out;
    std::array<Coeffs, EqBands> k;
    for (int b = 0; b < EqBands; ++b)
        k[b] = peaking(kGraphRate, kEqFreqs[b], kEqQ, profile().eq[b]);
    for (int i = 0; i < points; ++i) {
        const double f = frequencyAt(i, points);
        double db = 0;
        for (const auto &c : k)
            db += magnitudeDb(c, f, kGraphRate);
        out << db;
    }
    return out;
}

QList<qreal> Backend::tuningResponse(int points) const {
    QList<qreal> out;
    const bool on = profile().tuning && !m_editHeadphones;
    std::vector<Coeffs> k;
    if (on) {
        k.push_back(highpass(kGraphRate, kSpeakerHighpass, 0.5412));
        k.push_back(highpass(kGraphRate, kSpeakerHighpass, 1.3066));
        for (const auto &band : kSpeakerTuning)
            k.push_back(band.kind == TuningBand::Peak ? peaking(kGraphRate, band.freq, band.q, band.db)
                                                      : highshelf(kGraphRate, band.freq, band.q, band.db));
    }
    for (int i = 0; i < points; ++i) {
        const double f = frequencyAt(i, points);
        double db = 0;
        for (const auto &c : k)
            db += magnitudeDb(c, f, kGraphRate);
        out << std::max(db, -24.0);
    }
    return out;
}
