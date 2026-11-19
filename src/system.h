#pragma once

#include "settings.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
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

// The laptop model, e.g. "ROG Zephyrus G14 GA403UM_GA403UM"; ATOMICAUDIO_PRODUCT
// overrides it for tests.
QString productName();

// The output Atomic Audio processes: the sound device the system plays on
// (the default sink), or the laptop's own speakers while the default is
// something it can't sit in front of, like another app's virtual sink. Also
// its volume, and which sink the system is actually sending sound to.
struct OutputState {
    bool found = false;
    QString sink;
    QString description;
    OutputKind kind = OutputKind::Speakers;
    double volumeDb = 0;
    bool muted = false;
    QString defaultSink;
    QString defaultDescription;

    bool headphones() const { return kind == OutputKind::Headphones; }
    // Sound goes somewhere Atomic Audio doesn't process (a virtual sink).
    bool elsewhere() const { return found && !defaultSink.isEmpty() && defaultSink != sink; }
};

// `sinksJson` is `pactl -f json list sinks`; `defaultSink` the default's name.
OutputState parseSinks(const QByteArray &sinksJson, const QString &defaultSink = {});
// The default sink's name from `pactl -f json info`.
QString parseDefaultSink(const QByteArray &infoJson);
// Runs pactl and waits at most `timeoutMs` for each call.
OutputState queryOutput(int timeoutMs = 3000, QString *error = nullptr);

// The same query without blocking. A start() while one is running isn't
// dropped: it runs again as soon as the current one ends, so the answer is
// never older than the latest request.
class OutputQuery : public QObject {
    Q_OBJECT
public:
    explicit OutputQuery(QObject *parent = nullptr);
    void start();

signals:
    void finished(const OutputState &state);
    void failed(const QString &error);

private:
    QProcess m_info;
    QProcess m_sinks;
    QString m_default;
    bool m_again = false;
};

// What the engine reports to the app and the CLI.
struct EngineStatus {
    bool running = false;
    OutputState output;
    QString speakerProfile;
    QString error;
    qint64 pid = 0;

    // The engine process is still there; a crash leaves a stale file behind.
    bool alive() const;

    QJsonObject toJson() const;
    static EngineStatus fromJson(const QJsonObject &o);
    static EngineStatus read(const QString &path);
    bool write(const QString &path) const;
};

// The systemd user service that keeps the engine running.
namespace service {
constexpr const char *kUnit = "atomicaudio.service";
constexpr const char *kMarker = "# Written by atomicaudio";
QString userUnitPath();
QString packagedUnitPath();
// True when a packaged or user unit exists.
bool unitInstalled();
// Makes sure a unit runs `executable`: nothing to do with a packaged unit or
// one the user wrote; otherwise writes ours, or repoints ours if it runs a
// different binary. Sets `changed` when systemd needs a daemon-reload.
bool ensureUserUnit(const QString &executable, bool *changed, QString *error = nullptr);
QString unitText(const QString &executable);
// Once the package is installed, a user unit we wrote for a development
// build would shadow it; remove ours (never one the user wrote).
void dropStaleUserUnit();
bool isActive(int timeoutMs = 2000);
// Runs systemctl --user synchronously; for the CLI.
bool run(const QStringList &args, QString *error = nullptr, int timeoutMs = 15000);
} // namespace service
