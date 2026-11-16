#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Where things live. ATOMICAUDIO_CONFIG_DIR and ATOMICAUDIO_RUNTIME_DIR
// override the XDG locations, which keeps tests off the real files.
namespace paths {
QString configDir();
QString settingsFile();
QString runtimeDir();
QString engineConfigFile();
QString statusFile();
// The LADSPA plugin: beside a development build, else the installed one.
QString pluginPath();
} // namespace paths

// The laptop's own output: the sink that has a speaker port, and whether
// that sink is currently playing through its headphone jack.
struct OutputState {
    bool found = false;
    QString sink;
    QString description;
    bool headphones = false;
};

OutputState parseSinks(const QByteArray &pactlJson);
// Runs `pactl -f json list sinks`, waiting at most `timeoutMs`.
OutputState queryOutput(int timeoutMs = 3000, QString *error = nullptr);

// What the engine reports to the app and the CLI.
struct EngineStatus {
    bool running = false;
    bool headphones = false;
    QString sink;
    QString description;
    QString error;
    qint64 pid = 0;

    QJsonObject toJson() const;
    static EngineStatus fromJson(const QJsonObject &o);
    static EngineStatus read(const QString &path);
    bool write(const QString &path) const;
};

// The systemd user service that keeps the engine running.
namespace service {
constexpr const char *kUnit = "atomicaudio.service";
QString userUnitPath();
// True when a packaged or user unit exists.
bool unitInstalled();
// Writes a user unit that starts `executable engine`, for builds that
// weren't installed as a package.
bool writeUserUnit(const QString &executable, QString *error = nullptr);
QString unitText(const QString &executable);
// Once the package is installed, a user unit we wrote for a development
// build would shadow it; remove ours (never one the user wrote).
void dropStaleUserUnit();
bool isActive(int timeoutMs = 2000);
// Runs systemctl --user synchronously; for the CLI.
bool run(const QStringList &args, QString *error = nullptr, int timeoutMs = 15000);
} // namespace service
