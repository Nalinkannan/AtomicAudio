#pragma once

#include "params.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <array>

// One sound setup. Speakers and headphones each keep their own, like Dolby
// does, and the engine switches between them when you plug in.
struct Profile {
    QString mode = "dynamic";
    double leveler = 0.4;
    double dialogue = 0.3;
    double surround = 0.45;
    double bass = 0.55;
    double loudness = 0.5;
    bool tuning = true;
    QString eqPreset = "flat";
    std::array<double, atomic::EqBands> eq{};

    bool operator==(const Profile &o) const;
    bool operator!=(const Profile &o) const { return !(*this == o); }
};

struct Mode {
    QString id;
    double leveler, dialogue, surround, bass, loudness;
};

struct EqPreset {
    QString id;
    std::array<double, atomic::EqBands> gains;
};

using Controls = std::array<float, atomic::ParamCount>;

class Settings {
public:
    bool bypass = false;
    double ceiling = -1.0;
    Profile speakers = defaultProfile(false);
    Profile headphones = defaultProfile(true);

    Profile &profile(bool forHeadphones) { return forHeadphones ? headphones : speakers; }
    const Profile &profile(bool forHeadphones) const { return forHeadphones ? headphones : speakers; }

    static const QList<Mode> &modes();
    static const QList<EqPreset> &eqPresets();
    static const Mode *findMode(const QString &id);
    static const EqPreset *findEqPreset(const QString &id);
    static Profile defaultProfile(bool forHeadphones);

    // Sets the effect amounts of a built-in mode; "custom" keeps them.
    static bool applyMode(Profile &profile, const QString &id, bool forHeadphones);
    static bool applyEqPreset(Profile &profile, const QString &id);

    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &object);
    static Settings load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;

    Controls controls(bool forHeadphones) const;
};

// The PipeWire config the engine runs: a filter-chain smart filter that
// WirePlumber slots in front of `targetSink` (or the default sink when empty).
QString engineConfig(const Controls &controls, const QString &pluginPath, const QString &targetSink);

// A pw-cli line that sets every control in `controls` that differs from
// `previous` (all of them when previous is null); empty when nothing changed.
QString setParamCommand(const Controls &controls, const Controls *previous);

// The level a slider shows for a stored amount, and back (0-100).
inline int percent(double amount) { return int(amount * 100.0 + 0.5); }
