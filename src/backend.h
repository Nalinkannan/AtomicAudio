#pragma once

#include "settings.h"
#include "system.h"

#include <QElapsedTimer>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVector>

// The app's state: the settings being edited, saved for the engine to pick
// up, plus the engine service's state. Exposed to QML as `backend`.
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool engineOn READ engineOn NOTIFY engineChanged)
    Q_PROPERTY(bool engineBusy READ engineBusy NOTIFY engineChanged)
    Q_PROPERTY(bool filterRunning READ filterRunning NOTIFY engineChanged)
    Q_PROPERTY(bool headphonesActive READ headphonesActive NOTIFY engineChanged)
    Q_PROPERTY(QString outputSink READ outputSink NOTIFY engineChanged)
    Q_PROPERTY(QString outputDescription READ outputDescription NOTIFY engineChanged)
    Q_PROPERTY(QString error READ error NOTIFY engineChanged)
    Q_PROPERTY(bool editingHeadphones READ editingHeadphones WRITE setEditingHeadphones NOTIFY profileChanged)
    Q_PROPERTY(QString mode READ mode NOTIFY profileChanged)
    Q_PROPERTY(double leveler READ leveler NOTIFY profileChanged)
    Q_PROPERTY(double dialogue READ dialogue NOTIFY profileChanged)
    Q_PROPERTY(double surround READ surround NOTIFY profileChanged)
    Q_PROPERTY(double bass READ bass NOTIFY profileChanged)
    Q_PROPERTY(double loudness READ loudness NOTIFY profileChanged)
    Q_PROPERTY(bool tuning READ tuning NOTIFY profileChanged)
    Q_PROPERTY(QString eqPreset READ eqPreset NOTIFY profileChanged)
    Q_PROPERTY(QList<qreal> eq READ eq NOTIFY profileChanged)
    Q_PROPERTY(bool bypass READ bypass NOTIFY profileChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY historyChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY historyChanged)
    Q_PROPERTY(QStringList modeIds READ modeIds CONSTANT)
    Q_PROPERTY(QStringList eqPresetIds READ eqPresetIds CONSTANT)
    Q_PROPERTY(QList<qreal> eqFrequencies READ eqFrequencies CONSTANT)

public:
    // `manageService` off keeps tests away from systemctl.
    explicit Backend(const QString &settingsPath = {}, QObject *parent = nullptr, bool manageService = true);

    bool engineOn() const { return m_engineOn; }
    bool engineBusy() const { return m_busy; }
    bool filterRunning() const { return m_status.running; }
    bool headphonesActive() const { return m_status.headphones; }
    QString outputSink() const { return m_status.sink; }
    QString outputDescription() const { return m_status.description; }
    QString error() const { return m_error.isEmpty() ? m_status.error : m_error; }

    bool editingHeadphones() const { return m_editHeadphones; }
    void setEditingHeadphones(bool headphones);

    QString mode() const { return profile().mode; }
    double leveler() const { return profile().leveler; }
    double dialogue() const { return profile().dialogue; }
    double surround() const { return profile().surround; }
    double bass() const { return profile().bass; }
    double loudness() const { return profile().loudness; }
    bool tuning() const { return profile().tuning; }
    QString eqPreset() const { return profile().eqPreset; }
    QList<qreal> eq() const;
    bool bypass() const { return m_settings.bypass; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QStringList modeIds() const;
    QStringList eqPresetIds() const;
    QList<qreal> eqFrequencies() const;

    const Settings &settings() const { return m_settings; }

    Q_INVOKABLE void toggleEngine();
    Q_INVOKABLE void setMode(const QString &id);
    Q_INVOKABLE void setAmount(const QString &key, double value);
    Q_INVOKABLE void nudge(const QString &key, double delta);
    Q_INVOKABLE void setTuning(bool on);
    Q_INVOKABLE void setEqBand(int band, double db);
    Q_INVOKABLE void setEqPreset(const QString &id);
    Q_INVOKABLE void cycleEqPreset();
    Q_INVOKABLE void toggleBypass();
    Q_INVOKABLE void resetProfile();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void refresh();
    // Magnitude in dB at `points` log-spaced frequencies from 20 Hz to 20 kHz.
    Q_INVOKABLE QList<qreal> eqResponse(int points) const;
    Q_INVOKABLE QList<qreal> tuningResponse(int points) const;

    static double frequencyAt(int index, int points);

signals:
    void engineChanged();
    void profileChanged();
    void historyChanged();

private:
    Profile &profile() { return m_settings.profile(m_editHeadphones); }
    const Profile &profile() const { return m_settings.profile(m_editHeadphones); }
    void remember(const QString &key);
    void changed();
    void restore(const QJsonObject &state);
    void loadFromDisk();
    void watch();
    void readStatus();
    void runSystemctl(const QStringList &args);

    QString m_path;
    bool m_manageService;
    Settings m_settings;
    bool m_editHeadphones = false;
    bool m_editPinned = false;
    QVector<QJsonObject> m_undo, m_redo;
    QString m_lastKey;
    QElapsedTimer m_lastChange;

    EngineStatus m_status;
    bool m_engineOn = false;
    bool m_busy = false;
    QString m_error;

    QTimer m_save;
    QTimer m_poll;
    QTimer m_reload;
    QFileSystemWatcher m_watcher;
    QProcess m_isActive;
    QProcess m_systemctl;
};
