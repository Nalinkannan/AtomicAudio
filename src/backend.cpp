#include "backend.h"
#include "biquad.h"
#include "profiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <vector>
#include <cmath>

using namespace atomic;

namespace {
constexpr int kHistory = 100;
constexpr double kGraphRate = 48000.0;
} // namespace

Backend::Backend(const QString &settingsPath, QObject *parent, bool manageService)
    : QObject(parent), m_path(settingsPath.isEmpty() ? paths::settingsFile() : settingsPath),
      m_manageService(manageService), m_product(productName()) {
    QString error;
    m_settings = Settings::load(m_path, &error);
    if (!error.isEmpty()) {
        // Keep the broken file: the next save would otherwise replace the
        // only copy of whatever was in it.
        const QString backup = m_path + ".broken";
        QFile::remove(backup);
        QFile::copy(m_path, backup);
        m_error = tr("Your settings couldn't be read, so the defaults are in use. The old file is at %1.").arg(backup);
    }

    m_save.setSingleShot(true);
    m_save.setInterval(20);
    connect(&m_save, &QTimer::timeout, this, &Backend::save);

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

    // Without the engine there's no status file, so ask the sound server
    // ourselves which output is in use.
    connect(&m_query, &OutputQuery::finished, this, [this](const OutputState &state) {
        if (m_status.running)
            return;
        m_ownOutput = state;
        followOutput(state.kind);
        emit engineChanged();
    });

    m_poll.setInterval(1500);
    connect(&m_poll, &QTimer::timeout, this, &Backend::refresh);
    if (m_manageService) {
        m_poll.start();
        refresh();
    }
    m_editKind = output().kind;
}

Backend::~Backend() {
    // Quitting right after a change must not lose it.
    if (m_save.isActive()) {
        m_save.stop();
        save();
    }
}

void Backend::save() {
    QString err;
    if (!m_settings.save(m_path, &err)) {
        m_error = tr("Couldn't save settings: %1").arg(err);
        emit engineChanged();
    } else if (m_error.startsWith(tr("Couldn't save settings"))) {
        m_error.clear();
        emit engineChanged();
    }
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
    if (m_save.isActive()) {
        // Our own pending write goes first; look again after it.
        m_reload.start();
        return;
    }
    QString error;
    const Settings next = Settings::load(m_path, &error);
    if (!error.isEmpty() || next.toJson() == m_settings.toJson())
        return;
    m_settings = next;
    emit profileChanged();
}

void Backend::setOutputForTest(const OutputState &output) {
    m_ownOutput = output;
    followOutput(output.kind);
    emit engineChanged();
}

void Backend::followOutput(OutputKind kind) {
    // Switching outputs moves the editor to what you now hear, unless you
    // chose to edit another output; once both agree again, it follows once
    // more.
    if (m_editPinned) {
        if (m_editKind == kind)
            m_editPinned = false;
        return;
    }
    if (m_editKind != kind) {
        m_editKind = kind;
        emit profileChanged();
    }
}

void Backend::readStatus() {
    EngineStatus next = EngineStatus::read(paths::statusFile());
    // A crashed engine leaves its file behind; don't believe it.
    if (next.running && !next.alive())
        next = EngineStatus();
    const bool differs = next.toJson() != m_status.toJson();
    m_status = next;
    if (m_status.running)
        followOutput(m_status.output.kind);
    if (differs)
        emit engineChanged();
}

void Backend::refresh() {
    if (!m_manageService)
        return;
    readStatus();
    if (!m_status.running)
        m_query.start();
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
    // A build that wasn't installed as a package brings its own user unit,
    // pointing at this very binary.
    bool changed = false;
    QString err;
    if (!service::ensureUserUnit(QCoreApplication::applicationFilePath(), &changed, &err)) {
        m_error = tr("Couldn't write the service: %1").arg(err);
        emit engineChanged();
        return;
    }
    if (changed)
        service::run({"daemon-reload"});
    runSystemctl({"enable", "--now", service::kUnit});
}

void Backend::setEditingKind(OutputKind kind) {
    m_editPinned = kind != output().kind;
    if (kind == m_editKind)
        return;
    m_editKind = kind;
    emit profileChanged();
}

void Backend::setEditing(const QString &kind) {
    bool ok = false;
    const OutputKind k = outputKindFromId(kind, &ok);
    if (ok)
        setEditingKind(k);
}

void Backend::editNextOutput() {
    const int count = int(std::size(kOutputKinds));
    setEditingKind(kOutputKinds[(int(m_editKind) + 1) % count]);
}

QString Backend::speakerProfileName() const {
    return QString::fromUtf8(kSpeakerProfiles[m_settings.resolveSpeakerProfile(m_product)].name);
}

bool Backend::speakerProfileMeasured() const {
    return m_settings.resolveSpeakerProfile(m_product) != kGenericProfile;
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
    return Settings::eqPresetIds(EqGroup::House) << "custom";
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
    // Comparing with the original is listening, not editing: undo never
    // flips it.
    const bool bypass = m_settings.bypass;
    m_settings = Settings::fromJson(state);
    m_settings.bypass = bypass;
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
    if (!Settings::applyMode(next, id, m_editKind))
        return;
    remember("mode");
    profile() = next;
    changed();
}

void Backend::setAmount(const QString &key, double value) {
    Profile next = profile();
    if (!Settings::setAmount(next, key, value) || next == profile())
        return;
    remember(key);
    profile() = next;
    changed();
}

void Backend::nudge(const QString &key, double delta) {
    Profile copy = profile();
    const double *current = copy.amounts.find(key);
    if (!current)
        return;
    setAmount(key, std::round((*current + delta) * 100.0) / 100.0);
}

void Backend::setTuning(bool on) {
    if (on == profile().tuning)
        return;
    remember("tuning");
    profile().tuning = on;
    changed();
}

void Backend::setEqBand(int band, double db) {
    Profile next = profile();
    if (!Settings::setEqBand(next, band, db) || next == profile())
        return;
    remember(QStringLiteral("eq%1").arg(band));
    profile() = next;
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

// Steps through every preset, the library's too, so the TUI reaches them all.
void Backend::cycleEqPreset() {
    const QStringList ids = Settings::allEqPresetIds();
    setEqPreset(ids[(ids.indexOf(profile().eqPreset) + 1) % ids.size()]);
}

void Backend::toggleBypass() {
    m_settings.bypass = !m_settings.bypass;
    changed();
}

void Backend::resetProfile() {
    const Profile fresh = Settings::defaultProfile(m_editKind);
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
    const bool on = profile().tuning && m_editKind == OutputKind::Speakers;
    std::vector<Coeffs> k;
    if (on) {
        const auto &sp = kSpeakerProfiles[m_settings.resolveSpeakerProfile(m_product)];
        k.push_back(highpass(kGraphRate, sp.highpass, 0.5412));
        k.push_back(highpass(kGraphRate, sp.highpass, 1.3066));
        for (const auto &band : sp.bands)
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
