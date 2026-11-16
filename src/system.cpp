#include "system.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <utility>

namespace paths {

QString configDir() {
    const QString overridden = qEnvironmentVariable("ATOMICAUDIO_CONFIG_DIR");
    if (!overridden.isEmpty())
        return overridden;
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/atomicaudio";
}

QString settingsFile() { return configDir() + "/settings.json"; }

QString runtimeDir() {
    const QString overridden = qEnvironmentVariable("ATOMICAUDIO_RUNTIME_DIR");
    if (!overridden.isEmpty())
        return overridden;
    QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty())
        base = QDir::tempPath();
    return base + "/atomicaudio";
}

QString engineConfigFile() { return runtimeDir() + "/engine.conf"; }
QString statusFile() { return runtimeDir() + "/status.json"; }

QString pluginPath() {
    const QString overridden = qEnvironmentVariable("ATOMICAUDIO_PLUGIN");
    if (!overridden.isEmpty())
        return overridden;
    // A development build keeps it in build/dsp; an install keeps it in
    // <prefix>/lib/ladspa beside <prefix>/bin, whether that's /usr or ~/.local.
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {dir + "/dsp/atomicaudio.so", dir + "/../lib/ladspa/atomicaudio.so"})
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).canonicalFilePath();
    return "/usr/lib/ladspa/atomicaudio.so";
}

} // namespace paths

QString productName() {
    const QString overridden = qEnvironmentVariable("ATOMICAUDIO_PRODUCT");
    if (!overridden.isEmpty())
        return overridden;
    QFile file("/sys/class/dmi/id/product_name");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).trimmed();
}

OutputState parseSinks(const QByteArray &sinksJson, const QString &defaultSink) {
    OutputState state;
    state.defaultSink = defaultSink;
    const QJsonArray sinks = QJsonDocument::fromJson(sinksJson).array();
    for (const auto &value : sinks) {
        const QJsonObject sink = value.toObject();
        if (sink.value("name").toString() == defaultSink)
            state.defaultDescription = sink.value("description").toString();
        if (state.found)
            continue;
        const QJsonArray ports = sink.value("ports").toArray();
        bool hasSpeaker = false;
        QString activeType;
        const QString active = sink.value("active_port").toString();
        for (const auto &p : ports) {
            const QJsonObject port = p.toObject();
            const QString name = port.value("name").toString(), type = port.value("type").toString();
            if (type.compare("Speaker", Qt::CaseInsensitive) == 0 || name.contains("speaker", Qt::CaseInsensitive))
                hasSpeaker = true;
            if (name == active)
                activeType = type.isEmpty() ? name : type;
        }
        if (!hasSpeaker)
            continue;
        state.found = true;
        state.sink = sink.value("name").toString();
        state.description = sink.value("description").toString();
        state.headphones = activeType.contains("headphone", Qt::CaseInsensitive) ||
                           active.contains("headphone", Qt::CaseInsensitive);
        state.muted = sink.value("mute").toBool();
        // PulseAudio volumes are cubic: amplitude = (value / 65536)^3.
        const QJsonObject volume = sink.value("volume").toObject();
        double sum = 0;
        int channels = 0;
        for (auto it = volume.begin(); it != volume.end(); ++it) {
            sum += it.value().toObject().value("value").toDouble();
            ++channels;
        }
        const double level = channels ? sum / channels / 65536.0 : 1.0;
        state.volumeDb = level > 0 ? std::clamp(60.0 * std::log10(level), -90.0, 12.0) : -90.0;
    }
    if (state.defaultDescription.isEmpty())
        state.defaultDescription = defaultSink;
    return state;
}

QString parseDefaultSink(const QByteArray &infoJson) {
    return QJsonDocument::fromJson(infoJson).object().value("default_sink_name").toString();
}

namespace {

QString pactlError(QProcess &p) {
    if (p.error() == QProcess::FailedToStart)
        return QCoreApplication::translate("System", "pactl is missing; install libpulse.");
    const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
    return err.isEmpty() ? QCoreApplication::translate("System", "pactl couldn't reach the sound server.") : err;
}

} // namespace

OutputState queryOutput(int timeoutMs, QString *error) {
    QProcess info, sinks;
    info.start("pactl", {"-f", "json", "info"});
    sinks.start("pactl", {"-f", "json", "list", "sinks"});
    const bool ok = info.waitForFinished(timeoutMs) && info.exitCode() == 0 && sinks.waitForFinished(timeoutMs) &&
                    sinks.exitCode() == 0;
    if (!ok) {
        if (error)
            *error = pactlError(sinks.exitCode() != 0 || sinks.state() != QProcess::NotRunning ? sinks : info);
        info.kill();
        sinks.kill();
        return {};
    }
    return parseSinks(sinks.readAllStandardOutput(), parseDefaultSink(info.readAllStandardOutput()));
}

OutputQuery::OutputQuery(QObject *parent) : QObject(parent) {
    connect(&m_info, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0) {
            emit failed(pactlError(m_info));
            if (std::exchange(m_again, false))
                start();
            return;
        }
        m_default = parseDefaultSink(m_info.readAllStandardOutput());
        m_sinks.start("pactl", {"-f", "json", "list", "sinks"});
    });
    connect(&m_sinks, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (status != QProcess::NormalExit || code != 0)
            emit failed(pactlError(m_sinks));
        else if (!m_again) // A newer request supersedes this answer.
            emit finished(parseSinks(m_sinks.readAllStandardOutput(), m_default));
        if (std::exchange(m_again, false))
            start();
    });
    for (QProcess *p : {&m_info, &m_sinks})
        connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
            if (e == QProcess::FailedToStart)
                emit failed(pactlError(*p));
        });
}

void OutputQuery::start() {
    if (m_info.state() != QProcess::NotRunning || m_sinks.state() != QProcess::NotRunning) {
        m_again = true;
        return;
    }
    m_info.start("pactl", {"-f", "json", "info"});
}

bool EngineStatus::alive() const { return pid > 0 && QFileInfo::exists(QStringLiteral("/proc/%1").arg(pid)); }

QJsonObject EngineStatus::toJson() const {
    return {{"running", running},
            {"headphones", output.headphones},
            {"sink", output.sink},
            {"description", output.description},
            {"volumeDb", output.volumeDb},
            {"defaultSink", output.defaultSink},
            {"defaultDescription", output.defaultDescription},
            {"speakerProfile", speakerProfile},
            {"error", error},
            {"pid", pid}};
}

EngineStatus EngineStatus::fromJson(const QJsonObject &o) {
    EngineStatus s;
    s.running = o.value("running").toBool();
    s.output.found = !o.value("sink").toString().isEmpty();
    s.output.headphones = o.value("headphones").toBool();
    s.output.sink = o.value("sink").toString();
    s.output.description = o.value("description").toString();
    s.output.volumeDb = o.value("volumeDb").toDouble();
    s.output.defaultSink = o.value("defaultSink").toString();
    s.output.defaultDescription = o.value("defaultDescription").toString();
    s.speakerProfile = o.value("speakerProfile").toString();
    s.error = o.value("error").toString();
    s.pid = qint64(o.value("pid").toDouble());
    return s;
}

EngineStatus EngineStatus::read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return fromJson(QJsonDocument::fromJson(file.readAll()).object());
}

bool EngineStatus::write(const QString &path) const {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Compact));
    return file.commit();
}

namespace service {

QString userUnitPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/systemd/user/" + kUnit;
}

QString packagedUnitPath() { return QStringLiteral("/usr/lib/systemd/user/") + kUnit; }

bool unitInstalled() { return QFileInfo::exists(packagedUnitPath()) || QFileInfo::exists(userUnitPath()); }

QString unitText(const QString &executable) {
    return QStringLiteral(R"(%1 for a build that isn't installed as a package.
[Unit]
Description=Atomic Audio sound enhancement engine
After=pipewire.service pipewire-pulse.service wireplumber.service
BindsTo=pipewire.service

[Service]
Type=simple
ExecStart="%2" engine
Restart=on-failure
RestartSec=2
Slice=session.slice

[Install]
WantedBy=pipewire.service
)")
        .arg(kMarker, executable);
}

bool ensureUserUnit(const QString &executable, bool *changed, QString *error) {
    *changed = false;
    if (QFileInfo::exists(packagedUnitPath()))
        return true;
    QFile existing(userUnitPath());
    if (existing.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(existing.readAll());
        // The user's own unit is theirs; ours is current or gets repointed.
        if (!text.startsWith(kMarker) || text == unitText(executable))
            return true;
    }
    QDir().mkpath(QFileInfo(userUnitPath()).absolutePath());
    QSaveFile file(userUnitPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(unitText(executable).toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    *changed = true;
    return true;
}

void dropStaleUserUnit() {
    if (!QFileInfo::exists(packagedUnitPath()))
        return;
    QFile unit(userUnitPath());
    if (!unit.open(QIODevice::ReadOnly) || !unit.readLine().startsWith(kMarker))
        return;
    unit.close();
    const bool enabled = run({"is-enabled", "--quiet", kUnit});
    const bool active = isActive();
    if (!unit.remove())
        return;
    run({"daemon-reload"});
    // Point the enablement at the packaged unit and restart on its binary.
    if (enabled)
        run({"reenable", kUnit});
    if (active)
        run({"restart", kUnit});
}

bool isActive(int timeoutMs) {
    QProcess p;
    p.start("systemctl", {"--user", "is-active", "--quiet", kUnit});
    return p.waitForFinished(timeoutMs) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool run(const QStringList &args, QString *error, int timeoutMs) {
    QProcess p;
    p.start("systemctl", QStringList{"--user"} + args);
    if (!p.waitForFinished(timeoutMs) || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (error) {
            const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
            *error = err.isEmpty() ? QStringLiteral("systemctl --user %1 failed").arg(args.join(' ')) : err;
        }
        return false;
    }
    return true;
}

} // namespace service
