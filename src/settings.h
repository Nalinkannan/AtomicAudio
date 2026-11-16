#pragma once

#include "params.h"
#include "profiles.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <array>

// The five effect amounts a mode sets.
struct Amounts {
    double leveler = 0.4;
    double dialogue = 0.3;
    double surround = 0.45;
    double bass = 0.55;
    double loudness = 0.5;

    bool operator==(const Amounts &o) const;
    // The amount for a key ("bass", ...), or nullptr for an unknown key.
    double *find(const QString &key);
    static const QStringList &keys();
};

// One sound setup. Speakers and headphones each keep their own, like Dolby
// does, and the engine switches between them when you plug in. Custom mode
// and the custom EQ keep their own values, so trying a preset and coming
// back never loses your mix.
struct Profile {
    QString mode = "dynamic";
    Amounts amounts;
    Amounts customAmounts;
    bool tuning = true;
    QString eqPreset = "flat";
    std::array<double, atomic::EqBands> eq{};
    std::array<double, atomic::EqBands> customEq{};

    bool operator==(const Profile &o) const;
    bool operator!=(const Profile &o) const { return !(*this == o); }
};

struct Mode {
    QString id;
    Amounts amounts;
};

// Atomic Audio's own curves are the chips over the equalizer; the library
// curves (tone fixes and music genres) sit in the menu beside them.
enum class EqGroup { House, Tone, Genre };

struct EqPreset {
    QString id;
    std::array<double, atomic::EqBands> gains;
    EqGroup group = EqGroup::House;
};

using Controls = std::array<float, atomic::ParamCount>;

class Settings {
public:
    bool bypass = false;
    double ceiling = -1.0;
    // "auto" picks the optimizer curve from the laptop model; otherwise an id
    // from kSpeakerProfiles.
    QString speakerProfile = "auto";
    Profile speakers = defaultProfile(false);
    Profile headphones = defaultProfile(true);

    Profile &profile(bool forHeadphones) { return forHeadphones ? headphones : speakers; }
    const Profile &profile(bool forHeadphones) const { return forHeadphones ? headphones : speakers; }

    static const QList<Mode> &modes();
    static const QList<EqPreset> &eqPresets();
    static const Mode *findMode(const QString &id);
    static const EqPreset *findEqPreset(const QString &id);
    // Preset ids in display order: one group's, or every preset then "custom".
    static QStringList eqPresetIds(EqGroup group);
    static QStringList allEqPresetIds();
    static Profile defaultProfile(bool forHeadphones);

    // Sets the effect amounts of a mode; "custom" brings back your own.
    static bool applyMode(Profile &profile, const QString &id, bool forHeadphones);
    // Sets an EQ preset; "custom" brings back your own curve.
    static bool applyEqPreset(Profile &profile, const QString &id);
    // Changes one amount by hand, which makes the mix custom.
    static bool setAmount(Profile &profile, const QString &key, double value);
    // Changes one EQ band by hand, which makes the curve custom.
    static bool setEqBand(Profile &profile, int band, double db);

    // The optimizer curve to use on a laptop with this DMI product name.
    int resolveSpeakerProfile(const QString &productName) const;

    QJsonObject toJson() const;
    static Settings fromJson(const QJsonObject &object);
    static Settings load(const QString &path, QString *error = nullptr);
    bool save(const QString &path, QString *error = nullptr) const;

    // What the plugin's controls should be for this output, optimizer curve
    // and output volume (dB, 0 = full).
    Controls controls(bool forHeadphones, int speakerProfile = atomic::kGenericProfile, double volumeDb = 0) const;
};

// The PipeWire config the engine runs: a filter-chain smart filter that
// WirePlumber slots in front of `targetSink` (or the default sink when empty).
QString engineConfig(const Controls &controls, const QString &pluginPath, const QString &targetSink);

// A pw-cli line that sets every control in `controls` that differs from
// `previous` (all of them when previous is null); empty when nothing changed.
QString setParamCommand(const Controls &controls, const Controls *previous);

// The level a slider shows for a stored amount, and back (0-100).
inline int percent(double amount) { return int(amount * 100.0 + 0.5); }
