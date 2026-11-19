#pragma once

#include "settings.h"
#include "system.h"

#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QTimer>

// `atomicaudio engine`, run by the systemd user service. It hosts the
// filter-chain in a child `pipewire -c` process, applies settings changes
// live through pw-cli, follows the output in use (the laptop's speakers,
// the headphone jack, Bluetooth, HDMI, USB) with that output's settings, and
// feeds the output volume to the loudness contour.
class Engine : public QObject {
    Q_OBJECT

public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;

    bool start(QString *error);
    void stop();

private:
    Controls currentControls() const;
    void reloadSettings();
    void onOutput(const OutputState &state);
    void applyControls();
    void startPipewire();
    void restartPipewire();
    void startSubscribe();
    bool ensurePwcli();
    void send(const QString &command);
    void writeStatus();
    void watchSettings();

    Settings m_settings;
    QString m_product;
    int m_profile = atomic::kGenericProfile;
    OutputState m_output;
    QString m_runningSink;
    Controls m_sent{};
    bool m_haveSent = false;
    bool m_stopping = false;
    bool m_pwcliReady = false;
    QString m_error;
    int m_failures = 0;

    QProcess m_pipewire;
    QProcess m_pwcli;
    QProcess m_subscribe;
    OutputQuery m_query;
    QFileSystemWatcher m_watcher;
    QTimer m_settingsDebounce;
    QTimer m_outputDebounce;
    QTimer m_outputRetry;
    QTimer m_restart;
    QTimer m_pwcliWarmup;
    QTimer m_resend;
};
