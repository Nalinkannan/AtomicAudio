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
    m_outputDebounce.setInterval(120);
    connect(&m_outputDebounce, &QTimer::timeout, &m_query, &OutputQuery::start);
    connect(&m_query, &OutputQuery::finished, this, &Engine::onOutput);
    connect(&m_query, &OutputQuery::failed, this, [](const QString &error) { log(error); });

    // At login the sound server can still be starting; keep looking for an
    // output until one shows up.
    m_outputRetry.setInterval(3000);
    connect(&m_outputRetry, &QTimer::timeout, &m_query, &OutputQuery::start);

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
    connect(&m_subscribe, &QProcess::started, &m_query, &OutputQuery::start);
    connect(&m_subscribe, &QProcess::finished, this, [this] {
        if (!m_stopping)
            QTimer::singleShot(2000, this, &Engine::startSubscribe);
    });
}

Engine::~Engine() { stop(); }

Controls Engine::currentControls() const {
    // A muted output tells nothing about how loud you listen; keep the contour.
    const double volume = m_output.found && !m_output.muted ? m_output.volumeDb : 0.0;
    const Controls fromSettings = m_settings.controls(m_output.kind, m_profile, volume);
    if (m_output.muted && m_haveSent) {
        Controls c = fromSettings;
        c[atomic::Volume] = m_sent[atomic::Volume];
        return c;
    }
    return fromSettings;
}

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
    m_product = productName();
    m_profile = m_settings.resolveSpeakerProfile(m_product);
    log(tr("Speaker optimizer: %1").arg(atomic::kSpeakerProfiles[m_profile].name));

    QString queryError;
    m_output = queryOutput(3000, &queryError);
    if (!queryError.isEmpty())
        log(queryError);
    if (!m_output.found) {
        log(tr("No sound output found yet; filtering the default output until one shows up."));
        m_outputRetry.start();
    }

    watchSettings();
    startPipewire();
    startSubscribe();
    ensurePwcli();
    return true;
}

void Engine::stop() {
    m_stopping = true;
    for (QTimer *t : {&m_restart, &m_outputRetry, &m_outputDebounce, &m_settingsDebounce, &m_resend, &m_pwcliWarmup})
        t->stop();
    for (QProcess *p : {&m_pipewire, &m_pwcli, &m_subscribe}) {
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
    const Controls controls = currentControls();
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

void Engine::restartPipewire() {
    m_restart.stop();
    m_stopping = true;
    m_pipewire.terminate();
    if (!m_pipewire.waitForFinished(2000))
        m_pipewire.kill();
    m_pipewire.waitForFinished(500);
    m_stopping = false;
    startPipewire();
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
    const int profile = m_settings.resolveSpeakerProfile(m_product);
    if (profile != m_profile) {
        m_profile = profile;
        log(tr("Speaker optimizer: %1").arg(atomic::kSpeakerProfiles[m_profile].name));
        writeStatus();
    }
    applyControls();
}

void Engine::onOutput(const OutputState &state) {
    if (!state.found) {
        // Keep what we had; the output may just be between card profiles.
        if (!m_outputRetry.isActive())
            m_outputRetry.start();
        return;
    }
    m_outputRetry.stop();
    const bool sinkChanged = state.sink != m_runningSink;
    const bool kindChanged = state.kind != m_output.kind;
    m_output = state;
    if (sinkChanged) {
        // Another output became the default (Bluetooth connected, HDMI
        // picked), a new card profile renamed the sink, or at login the
        // output only now appeared. The smart filter's target is fixed in the
        // config, so restart the filter in front of the new one.
        log(tr("Filtering %1 (%2).").arg(state.description.isEmpty() ? state.sink : state.description,
                                         outputKindId(state.kind)));
        restartPipewire();
    } else {
        if (kindChanged)
            log(state.headphones() ? tr("Headphones plugged in.") : tr("Back on speakers."));
        applyControls();
    }
    writeStatus();
}

void Engine::applyControls() {
    if (!ensurePwcli())
        return; // Runs again once pw-cli is ready.
    const Controls controls = currentControls();
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
    status.output = m_output;
    status.speakerProfile = atomic::kSpeakerProfiles[m_profile].id;
    status.error = m_error;
    status.pid = QCoreApplication::applicationPid();
    status.write(paths::statusFile());
}
