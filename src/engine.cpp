#include "engine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>

namespace {

void log(const QString &line) {
    QTextStream err(stderr);
    err << line << Qt::endl;
}

} // namespace

Engine::Engine(QObject *parent) : QObject(parent) {
    m_settingsDebounce.setSingleShot(true);
    m_settingsDebounce.setInterval(15);
    connect(&m_settingsDebounce, &QTimer::timeout, this, &Engine::reloadSettings);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_settingsDebounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_settingsDebounce, qOverload<>(&QTimer::start));

    m_outputDebounce.setSingleShot(true);
    m_outputDebounce.setInterval(150);
    connect(&m_outputDebounce, &QTimer::timeout, this, &Engine::refreshOutput);

    m_restart.setSingleShot(true);
    connect(&m_restart, &QTimer::timeout, this, &Engine::startPipewire);

    m_pipewire.setProcessChannelMode(QProcess::ForwardedChannels);
    connect(&m_pipewire, &QProcess::started, this, [this] {
        m_error.clear();
        writeStatus();
    });
    connect(&m_pipewire, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            m_error = tr("Couldn't start pipewire: %1").arg(m_pipewire.errorString());
            log(m_error);
            writeStatus();
        }
    });
    connect(&m_pipewire, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        if (m_stopping)
            return;
        ++m_failures;
        m_error = tr("The audio filter stopped (exit %1); restarting.").arg(code);
        log(m_error);
        writeStatus();
        m_restart.start(std::min(30000, 1000 * m_failures));
    });

    // pw-cli keeps one connection open, so a slider drag costs a line of text
    // rather than a new process per step.
    connect(&m_pwcli, &QProcess::readyReadStandardOutput, this, [this] { m_pwcli.readAllStandardOutput(); });
    connect(&m_pwcli, &QProcess::readyReadStandardError, this, [this] {
        const auto lines = QString::fromUtf8(m_pwcli.readAllStandardError()).split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            // The filter isn't there (yet, or any more): send everything once
            // it is, rather than losing the change.
            if (line.contains("unknown global"))
                m_resend.start();
            else if (!line.contains("unsupported type"))
                log("pw-cli: " + line.trimmed());
        }
    });
    connect(&m_pwcli, &QProcess::finished, this, [this] { m_pwcliReady = false; });

    // A fresh pw-cli needs a moment to learn the graph before it can find the
    // filter by name.
    m_pwcliWarmup.setSingleShot(true);
    m_pwcliWarmup.setInterval(500);
    connect(&m_pwcliWarmup, &QTimer::timeout, this, [this] {
        m_pwcliReady = true;
        applyControls();
    });
    m_resend.setSingleShot(true);
    m_resend.setInterval(700);
    connect(&m_resend, &QTimer::timeout, this, [this] {
        m_haveSent = false;
        applyControls();
    });

    connect(&m_subscribe, &QProcess::readyReadStandardOutput, this, [this] {
        const QByteArray data = m_subscribe.readAllStandardOutput();
        if (data.contains("sink") || data.contains("card") || data.contains("server"))
            m_outputDebounce.start();
    });
    connect(&m_subscribe, &QProcess::finished, this, [this] {
        if (!m_stopping)
            QTimer::singleShot(2000, this, &Engine::startSubscribe);
    });

    connect(&m_query, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (code == 0 && status == QProcess::NormalExit)
            onOutput(parseSinks(m_query.readAllStandardOutput()));
    });
}

Engine::~Engine() { stop(); }

bool Engine::start(QString *error) {
    if (!QDir().mkpath(paths::runtimeDir())) {
        *error = tr("Can't create %1").arg(paths::runtimeDir());
        return false;
    }
    if (!QFileInfo::exists(paths::pluginPath())) {
        *error = tr("The Atomic Audio DSP plugin is missing at %1").arg(paths::pluginPath());
        return false;
    }
    QString settingsError;
    m_settings = Settings::load(paths::settingsFile(), &settingsError);
    if (!settingsError.isEmpty())
        log(settingsError);

    QString queryError;
    m_output = queryOutput(3000, &queryError);
    if (!queryError.isEmpty())
        log(queryError);
    if (!m_output.found)
        log(tr("No built-in speakers found; filtering the default output instead."));

    watchSettings();
    startPipewire();
    startSubscribe();
    ensurePwcli();
    return true;
}

void Engine::stop() {
    m_stopping = true;
    m_restart.stop();
    for (QProcess *p : {&m_pipewire, &m_pwcli, &m_subscribe, &m_query}) {
        if (p->state() == QProcess::NotRunning)
            continue;
        p->terminate();
        if (!p->waitForFinished(2000))
            p->kill();
        p->waitForFinished(500);
    }
    QFile::remove(paths::statusFile());
}

void Engine::startPipewire() {
    if (m_stopping || m_pipewire.state() != QProcess::NotRunning)
        return;
    const Controls controls = m_settings.controls(m_output.headphones);
    QSaveFile conf(paths::engineConfigFile());
    if (!conf.open(QIODevice::WriteOnly) ||
        conf.write(engineConfig(controls, paths::pluginPath(), m_output.sink).toUtf8()) < 0 || !conf.commit()) {
        m_error = tr("Can't write %1").arg(paths::engineConfigFile());
        log(m_error);
        writeStatus();
        return;
    }
    // The config already carries every control, so nothing needs sending
    // until something changes.
    m_sent = controls;
    m_haveSent = true;
    m_runningSink = m_output.sink;
    m_pipewire.start("pipewire", {"-c", paths::engineConfigFile()});
    if (m_pipewire.waitForStarted(3000))
        QTimer::singleShot(30000, this, [this] {
            if (m_pipewire.state() == QProcess::Running)
                m_failures = 0;
        });
}

void Engine::startSubscribe() {
    if (m_stopping || m_subscribe.state() != QProcess::NotRunning)
        return;
    m_subscribe.start("pactl", {"subscribe"});
}

void Engine::watchSettings() {
    const auto watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    // Saves replace the file, so watch the directory too and re-arm each time.
    QDir().mkpath(paths::configDir());
    m_watcher.addPath(paths::configDir());
    if (QFileInfo::exists(paths::settingsFile()))
        m_watcher.addPath(paths::settingsFile());
}

void Engine::reloadSettings() {
    watchSettings();
    QString error;
    const Settings next = Settings::load(paths::settingsFile(), &error);
    if (!error.isEmpty()) {
        log(error);
        return;
    }
    m_settings = next;
    applyControls();
}

void Engine::refreshOutput() {
    if (m_query.state() != QProcess::NotRunning)
        return;
    m_query.start("pactl", {"-f", "json", "list", "sinks"});
}

void Engine::onOutput(const OutputState &state) {
    const bool sinkChanged = state.found && state.sink != m_runningSink;
    const bool modeChanged = state.headphones != m_output.headphones;
    if (state.found)
        m_output = state;
    if (sinkChanged) {
        // A new card profile renames the sink; the smart filter's target is
        // fixed in the config, so restart the filter against the new name.
        log(tr("Output is now %1; restarting the filter.").arg(state.sink));
        m_restart.stop();
        m_stopping = true;
        m_pipewire.terminate();
        m_pipewire.waitForFinished(2000);
        m_stopping = false;
        startPipewire();
    } else if (modeChanged) {
        log(state.headphones ? tr("Headphones plugged in.") : tr("Back on speakers."));
        applyControls();
    }
    writeStatus();
}

void Engine::applyControls() {
    if (!ensurePwcli())
        return; // Runs again once pw-cli is ready.
    const Controls controls = m_settings.controls(m_output.headphones);
    const QString command = setParamCommand(controls, m_haveSent ? &m_sent : nullptr);
    if (command.isEmpty())
        return;
    send(command);
    m_sent = controls;
    m_haveSent = true;
}

bool Engine::ensurePwcli() {
    if (m_pwcli.state() == QProcess::NotRunning && !m_stopping) {
        m_pwcliReady = false;
        m_pwcli.start("pw-cli", QStringList{});
        if (!m_pwcli.waitForStarted(2000)) {
            m_error = tr("Couldn't start pw-cli: %1").arg(m_pwcli.errorString());
            log(m_error);
            writeStatus();
            return false;
        }
        m_pwcliWarmup.start();
    }
    return m_pwcliReady;
}

void Engine::send(const QString &command) { m_pwcli.write(command.toUtf8() + '\n'); }

void Engine::writeStatus() {
    EngineStatus status;
    status.running = m_pipewire.state() == QProcess::Running;
    status.headphones = m_output.headphones;
    status.sink = m_output.sink;
    status.description = m_output.description;
    status.error = m_error;
    status.pid = QCoreApplication::applicationPid();
    status.write(paths::statusFile());
}
