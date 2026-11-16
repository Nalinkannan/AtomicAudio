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
    const QString beside = QCoreApplication::applicationDirPath() + "/dsp/atomicaudio.so";
    if (QFileInfo::exists(beside))
        return beside;
    return "/usr/lib/ladspa/atomicaudio.so";
}

} // namespace paths

OutputState parseSinks(const QByteArray &pactlJson) {
    OutputState state;
    const QJsonArray sinks = QJsonDocument::fromJson(pactlJson).array();
    for (const auto &value : sinks) {
        const QJsonObject sink = value.toObject();
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
        break;
    }
    return state;
}

OutputState queryOutput(int timeoutMs, QString *error) {
    QProcess pactl;
    pactl.start("pactl", {"-f", "json", "list", "sinks"});
    if (!pactl.waitForFinished(timeoutMs) || pactl.exitCode() != 0) {
        if (error)
            *error = pactl.error() == QProcess::FailedToStart
                         ? QCoreApplication::translate("System", "pactl is missing; install libpulse.")
                         : QString::fromUtf8(pactl.readAllStandardError()).trimmed();
        pactl.kill();
        return {};
    }
    return parseSinks(pactl.readAllStandardOutput());
}

QJsonObject EngineStatus::toJson() const {
    return {{"running", running}, {"headphones", headphones}, {"sink", sink},
            {"description", description}, {"error", error}, {"pid", pid}};
}

EngineStatus EngineStatus::fromJson(const QJsonObject &o) {
    EngineStatus s;
    s.running = o.value("running").toBool();
    s.headphones = o.value("headphones").toBool();
    s.sink = o.value("sink").toString();
    s.description = o.value("description").toString();
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

bool unitInstalled() {
    return QFileInfo::exists(QStringLiteral("/usr/lib/systemd/user/") + kUnit) || QFileInfo::exists(userUnitPath());
}

QString unitText(const QString &executable) {
    return QStringLiteral(R"(# Written by atomicaudio for a build that isn't installed as a package.
[Unit]
Description=Atomic Audio sound enhancement engine
After=pipewire.service wireplumber.service
BindsTo=pipewire.service

[Service]
Type=simple
ExecStart="%1" engine
Restart=on-failure
RestartSec=2
Slice=session.slice

[Install]
WantedBy=pipewire.service
)")
        .arg(executable);
}

bool writeUserUnit(const QString &executable, QString *error) {
    QDir().mkpath(QFileInfo(userUnitPath()).absolutePath());
    QSaveFile file(userUnitPath());
    if (!file.open(QIODevice::WriteOnly) || file.write(unitText(executable).toUtf8()) < 0 || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

void dropStaleUserUnit() {
    if (!QFileInfo::exists(QStringLiteral("/usr/lib/systemd/user/") + kUnit))
        return;
    QFile unit(userUnitPath());
    if (!unit.open(QIODevice::ReadOnly) || !unit.readLine().startsWith("# Written by atomicaudio"))
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
