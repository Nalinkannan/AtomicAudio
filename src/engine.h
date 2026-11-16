#pragma once

#include "settings.h"
#include "system.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>

// `atomicaudio engine`, run by the systemd user service. It hosts the
// filter-chain in a child `pipewire -c` process, applies settings changes
// live through pw-cli, and switches between the speaker and headphone
// profiles when the jack is used.
class Engine : public QObject {
    Q_OBJECT

public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    bool start(QString *error);
    void stop();

private:
    void reloadSettings();
    void refreshOutput();
    void onOutput(const OutputState &state);
    void applyControls();
    void startPipewire();
    void startSubscribe();
    bool ensurePwcli();
    void send(const QString &command);
    void writeStatus();
    void watchSettings();

    Settings m_settings;
    OutputState m_output;
    QString m_runningSink;
    Controls m_sent{};
    bool m_haveSent = false;
    bool m_stopping = false;
    QString m_error;
    int m_failures = 0;

    QProcess m_pipewire;
    QProcess m_pwcli;
    QProcess m_subscribe;
    QProcess m_query;
    QFileSystemWatcher m_watcher;
    QTimer m_settingsDebounce;
    QTimer m_outputDebounce;
    QTimer m_restart;
    QTimer m_pwcliWarmup;
    QTimer m_resend;
    bool m_pwcliReady = false;
};
