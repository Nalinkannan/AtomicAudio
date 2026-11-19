#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <cmath>

using namespace atomic;

namespace {

double clamp01(double v) { return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0; }
double clampDb(double v) { return std::isfinite(v) ? std::clamp(v, -12.0, 12.0) : 0.0; }

QJsonObject amountsToJson(const Amounts &a) {
    return {{"leveler", a.leveler}, {"dialogue", a.dialogue}, {"surround", a.surround},
            {"bass", a.bass},       {"loudness", a.loudness}};
}

Amounts amountsFromJson(const QJsonObject &o, Amounts a) {
    for (const QString &key : Amounts::keys())
        *a.find(key) = clamp01(o.value(key).toDouble(*a.find(key)));
    return a;
}

QJsonArray eqToJson(const std::array<double, EqBands> &eq) {
    QJsonArray array;
    for (double g : eq)
        array.append(g);
    return array;
}

std::array<double, EqBands> eqFromJson(const QJsonValue &value, std::array<double, EqBands> eq) {
    const QJsonArray array = value.toArray();
    for (int i = 0; i < EqBands && i < array.size(); ++i)
        eq[i] = clampDb(array.at(i).toDouble(0));
    return eq;
}

QJsonObject profileToJson(const Profile &p) {
    // The live amounts sit at the top level, where people editing the file
    // by hand expect them.
    QJsonObject o = amountsToJson(p.amounts);
    o.insert("mode", p.mode);
    o.insert("custom", amountsToJson(p.customAmounts));
    o.insert("tuning", p.tuning);
    o.insert("eqPreset", p.eqPreset);
    o.insert("eq", eqToJson(p.eq));
    o.insert("customEq", eqToJson(p.customEq));
    return o;
}

Profile profileFromJson(const QJsonValue &value, const Profile &fallback) {
    if (!value.isObject())
        return fallback;
    const QJsonObject o = value.toObject();
    Profile p = fallback;
    const QString mode = o.value("mode").toString();
    if (Settings::findMode(mode) || mode == "custom")
        p.mode = mode;
    p.amounts = amountsFromJson(o, p.amounts);
    // Files from before custom slots existed: a custom mix is its own slot.
    p.customAmounts = o.value("custom").isObject() ? amountsFromJson(o.value("custom").toObject(), p.customAmounts)
                      : p.mode == "custom"           ? p.amounts
                                                     : p.customAmounts;
    p.tuning = o.value("tuning").toBool(p.tuning);
    const QString preset = o.value("eqPreset").toString();
    if (Settings::findEqPreset(preset) || preset == "custom")
        p.eqPreset = preset;
    p.eq = eqFromJson(o.value("eq"), p.eq);
    p.customEq = o.contains("customEq") ? eqFromJson(o.value("customEq"), p.customEq)
                 : p.eqPreset == "custom" ? p.eq
                                          : p.customEq;
    return p;
}

} // namespace

QString outputKindId(OutputKind kind) {
    switch (kind) {
    case OutputKind::Headphones: return QStringLiteral("headphones");
    case OutputKind::External: return QStringLiteral("external");
    case OutputKind::Speakers: break;
    }
    return QStringLiteral("speakers");
}

OutputKind outputKindFromId(const QString &id, bool *ok) {
    if (ok)
        *ok = true;
    for (OutputKind kind : kOutputKinds)
        if (outputKindId(kind) == id)
            return kind;
    if (ok)
        *ok = false;
    return OutputKind::Speakers;
}

bool Amounts::operator==(const Amounts &o) const {
    return leveler == o.leveler && dialogue == o.dialogue && surround == o.surround && bass == o.bass &&
           loudness == o.loudness;
}

double *Amounts::find(const QString &key) {
    return key == "leveler"    ? &leveler
           : key == "dialogue" ? &dialogue
           : key == "surround" ? &surround
           : key == "bass"     ? &bass
           : key == "loudness" ? &loudness
                               : nullptr;
}

const QStringList &Amounts::keys() {
    static const QStringList list = {"surround", "dialogue", "bass", "leveler", "loudness"};
    return list;
}

bool Profile::operator==(const Profile &o) const {
    return mode == o.mode && amounts == o.amounts && customAmounts == o.customAmounts && tuning == o.tuning &&
           eqPreset == o.eqPreset && eq == o.eq && customEq == o.customEq;
}

// Each mode turns up what that kind of content needs from laptop speakers.
const QList<Mode> &Settings::modes() {
    static const QList<Mode> list = {
        {"dynamic", {0.40, 0.30, 0.45, 0.55, 0.50}},
        {"movie", {0.50, 0.55, 0.80, 0.65, 0.55}},
        {"music", {0.15, 0.00, 0.40, 0.60, 0.45}},
        {"game", {0.30, 0.25, 0.90, 0.50, 0.55}},
        {"voice", {0.70, 0.90, 0.10, 0.10, 0.50}},
    };
    return list;
}

// Our own curves first, gentle enough to leave on all day. Then a library of
// well-known curves from open-source players, kept as published; they're
// bolder, the way those players ship them. Both libraries use the same ten
// octave bands as ours (31 Hz … 16 kHz):
//   - iQualize, Copyright (c) 2026 Darius, MIT License
//     (https://github.com/dariuscorvus/iqualize, Sources/iQualize/EQModels.swift):
//     the tone curves and the electronic and rap genres.
//   - GStreamer's 10-band equalizer presets, LGPL-2.1-or-later
//     (gst-plugins-good/gst/equalizer/GstIirEqualizer10Bands.prs): the other
//     genres. Its bands sit about a tenth of an octave lower than ours.
const QList<EqPreset> &Settings::eqPresets() {
    using G = EqGroup;
    static const QList<EqPreset> list = {
        {"flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"open", {0, 0, 0, -0.5, -0.5, 0, 0.5, 1.5, 2.5, 3.0}},
        {"rich", {2.0, 3.0, 2.5, 1.0, 0, -0.5, 0, 0.5, 1.0, 1.0}},
        {"focused", {-2.0, -1.5, -1.0, 0, 1.0, 2.0, 2.5, 1.5, 0, -1.0}},

        {"bass-boost", {10, 10, 8, 4, 0, 0, 0, 0, 0, 0}, G::Tone},
        {"treble-boost", {0, 0, 0, 0, 0, 2, 4, 6, 8, 10}, G::Tone},
        {"vocal-clarity", {-6, -6, -4, 0, 0, 6, 6, 4, 0, 0}, G::Tone},
        {"podcast", {-8, -4, -2, 0, 2, 4, 6, 4, 2, 0}, G::Tone},
        {"loudness-curve", {8, 6, 0, -2, -4, -2, 0, 2, 4, 6}, G::Tone},

        {"american-rap", {10, 8, 4, 0, -2, -2, 2, 4, 6, 4}, G::Genre},
        {"ballad", {4, 3.75, 2.5, 0, -4, -6, -3, 0, 2.5, 9}, G::Genre},
        {"classical", {0, 0, 0, 0, 0, 0, -6, -7, -7, -9.5}, G::Genre},
        {"club", {0, 0, 8, 6, 5.5, 5, 3, 0, 0, 0}, G::Genre},
        {"dance", {9.6, 7, 2.5, 0, 0, -5.6, -7, -7, 0, 0}, G::Genre},
        {"deep-house", {6, 10, 8, 2, -2, -4, -2, 0, 2, 4}, G::Genre},
        {"german-rap", {6, 8, 6, 2, -2, 0, 4, 4, 2, 2}, G::Genre},
        {"hard-techno", {10, 10, 6, 0, -4, -2, 2, 6, 8, 10}, G::Genre},
        {"minimal", {4, 6, 4, 0, -2, -2, 0, 2, 4, 2}, G::Genre},
        {"party", {7, 7, 0, 0, 0, 0, 0, 0, 7, 7}, G::Genre},
        {"pop", {-1.6, 4.5, 7, 8, 5.6, 0, -2.5, -2, -1.6, -1.5}, G::Genre},
        {"reggae", {0, 0, 0, -5.5, 0, 6.5, 6.5, 0, 0, 0}, G::Genre},
        {"rock", {8, 5, -5.5, -8, -3, 4, 8, 11, 11, 11.5}, G::Genre},
        {"ska", {-2.5, -5, -4, 0, 4, 5.5, 8, 9, 11, 9}, G::Genre},
        {"soft", {5, 1.5, 0, -2.5, 0, 4, 8, 9, 11, 12}, G::Genre},
        {"techno", {8, 8, 4, -2, -4, -2, 0, 4, 6, 8}, G::Genre},
    };
    return list;
}

QStringList Settings::eqPresetIds(EqGroup group) {
    QStringList ids;
    for (const auto &p : eqPresets())
        if (p.group == group)
            ids << p.id;
    return ids;
}

QStringList Settings::allEqPresetIds() {
    QStringList ids;
    for (const auto &p : eqPresets())
        ids << p.id;
    return ids << "custom";
}

const Mode *Settings::findMode(const QString &id) {
    for (const auto &m : modes())
        if (m.id == id)
            return &m;
    return nullptr;
}

const EqPreset *Settings::findEqPreset(const QString &id) {
    for (const auto &p : eqPresets())
        if (p.id == id)
            return &p;
    return nullptr;
}

Profile &Settings::profile(OutputKind kind) {
    return kind == OutputKind::Headphones ? headphones : kind == OutputKind::External ? external : speakers;
}

const Profile &Settings::profile(OutputKind kind) const {
    return kind == OutputKind::Headphones ? headphones : kind == OutputKind::External ? external : speakers;
}

Profile Settings::defaultProfile(OutputKind kind) {
    Profile p;
    applyMode(p, "dynamic", kind);
    return p;
}

bool Settings::applyMode(Profile &profile, const QString &id, OutputKind kind) {
    if (id == "custom") {
        profile.mode = id;
        profile.amounts = profile.customAmounts;
        return true;
    }
    const Mode *m = findMode(id);
    if (!m)
        return false;
    profile.mode = id;
    profile.amounts = m->amounts;
    // Headphones need far less help to be loud, and speakers bigger than a
    // laptop's need less too.
    if (kind == OutputKind::Headphones)
        profile.amounts.loudness = std::min(profile.amounts.loudness, 0.3);
    else if (kind == OutputKind::External)
        profile.amounts.loudness = std::min(profile.amounts.loudness, 0.4);
    return true;
}

bool Settings::applyEqPreset(Profile &profile, const QString &id) {
    if (id == "custom") {
        profile.eqPreset = id;
        profile.eq = profile.customEq;
        return true;
    }
    const EqPreset *p = findEqPreset(id);
    if (!p)
        return false;
    profile.eqPreset = id;
    profile.eq = p->gains;
    return true;
}

bool Settings::setAmount(Profile &profile, const QString &key, double value) {
    double *slot = profile.amounts.find(key);
    if (!slot)
        return false;
    *slot = clamp01(value);
    profile.mode = "custom";
    profile.customAmounts = profile.amounts;
    return true;
}

bool Settings::setEqBand(Profile &profile, int band, double db) {
    if (band < 0 || band >= EqBands)
        return false;
    // Half-decibel steps: finer than anyone hears, coarse enough to land on 0.
    profile.eq[band] = clampDb(std::round(db * 2.0) / 2.0);
    profile.eqPreset = "custom";
    profile.customEq = profile.eq;
    return true;
}

int Settings::resolveSpeakerProfile(const QString &productName) const {
    if (speakerProfile != "auto") {
        const int chosen = profileIndex(speakerProfile.toUtf8().constData());
        if (chosen >= 0)
            return chosen;
    }
    return detectProfile(productName.toUtf8().constData());
}

QJsonObject Settings::toJson() const {
    return {
        {"version", 2},
        {"bypass", bypass},
        {"ceiling", ceiling},
        {"speakerProfile", speakerProfile},
        {"speakers", profileToJson(speakers)},
        {"headphones", profileToJson(headphones)},
        {"external", profileToJson(external)},
    };
}

Settings Settings::fromJson(const QJsonObject &o) {
    Settings s;
    s.bypass = o.value("bypass").toBool(false);
    const double ceiling = o.value("ceiling").toDouble(-1.0);
    s.ceiling = std::isfinite(ceiling) ? std::clamp(ceiling, -6.0, 0.0) : -1.0;
    const QString speakerProfile = o.value("speakerProfile").toString("auto");
    s.speakerProfile = profileIndex(speakerProfile.toUtf8().constData()) >= 0 ? speakerProfile : QString("auto");
    s.speakers = profileFromJson(o.value("speakers"), defaultProfile(OutputKind::Speakers));
    s.headphones = profileFromJson(o.value("headphones"), defaultProfile(OutputKind::Headphones));
    s.external = profileFromJson(o.value("external"), defaultProfile(OutputKind::External));
    return s;
}

Settings Settings::load(const QString &path, QString *error) {
    QFile file(path);
    if (!file.exists())
        return {};
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = file.errorString();
        return {};
    }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, parse.errorString());
        return {};
    }
    return fromJson(doc.object());
}

bool Settings::save(const QString &path, QString *error) const {
    QDir().mkpath(QFileInfo(path).absolutePath());
    // QSaveFile writes beside the target and renames, so the engine never
    // reads a half-written file.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}

Controls Settings::controls(OutputKind kind, int speakerProfile, double volumeDb) const {
    const Profile &p = profile(kind);
    Controls c{};
    c[Bypass] = bypass ? 1.0f : 0.0f;
    c[Headphones] = kind == OutputKind::Headphones ? 1.0f : 0.0f;
    c[External] = kind == OutputKind::External ? 1.0f : 0.0f;
    c[Leveler] = float(p.amounts.leveler);
    c[Dialogue] = float(p.amounts.dialogue);
    c[Surround] = float(p.amounts.surround);
    c[Bass] = float(p.amounts.bass);
    c[Loudness] = float(p.amounts.loudness);
    // The optimizer's curves are measured for the laptop's own speakers.
    c[Tuning] = p.tuning && kind == OutputKind::Speakers ? 1.0f : 0.0f;
    c[Ceiling] = float(ceiling);
    for (int i = 0; i < EqBands; ++i)
        c[Eq1 + i] = float(p.eq[i]);
    c[SpeakerCurve] = float(std::clamp(speakerProfile, 0, int(kSpeakerProfiles.size()) - 1));
    // Whole decibels: finer steps would only flood pw-cli while the volume
    // slider moves.
    c[Volume] = float(std::clamp(std::round(std::isfinite(volumeDb) ? volumeDb : 0.0), -90.0, 0.0));
    return c;
}

namespace {

// pw-cli takes booleans for toggled controls; the config file only reads
// numbers for them (a `false` there silently falls back to the default).
QString controlValue(int index, float value, bool forConfig = false) {
    if (kParams[index].toggle) {
        const bool on = value > 0.5f;
        if (forConfig)
            return on ? QStringLiteral("1") : QStringLiteral("0");
        return on ? QStringLiteral("true") : QStringLiteral("false");
    }
    return QString::number(double(value), 'g', 6);
}

QString quoted(const QString &s) {
    QString escaped = s;
    escaped.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + escaped + '"';
}

} // namespace

QString engineConfig(const Controls &controls, const QString &pluginPath, const QString &targetSink) {
    QString control;
    for (int i = 0; i < ParamCount; ++i)
        control += QStringLiteral("                            %1 = %2\n").arg(quoted(kParams[i].name), controlValue(i, controls[i], true));

    const QString target = targetSink.isEmpty()
                               ? QString()
                               : QStringLiteral("            filter.smart.target = { node.name = %1 }\n").arg(quoted(targetSink));

    return QStringLiteral(R"(# Generated by atomicaudio; edits are overwritten.
context.properties = {
    log.level = 0
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-rt args = { } flags = [ ifexists nofail ] }
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-filter-chain
        args = {
            node.description = "Atomic Audio"
            media.name       = "Atomic Audio"
            filter.graph = {
                nodes = [
                    {
                        type   = ladspa
                        name   = %1
                        plugin = %2
                        label  = atomicaudio
                        control = {
%3                        }
                    }
                ]
            }
            audio.channels = 2
            audio.position = [ FL FR ]
            capture.props = {
                node.name        = "atomicaudio"
                node.description = "Atomic Audio"
                media.class      = Audio/Sink
                filter.smart      = true
                filter.smart.name = "atomicaudio"
%4            }
            playback.props = {
                node.name         = "atomicaudio.output"
                node.description  = "Atomic Audio output"
                node.passive      = true
                stream.dont-remix = true
            }
        }
    }
]
)")
        .arg(kGraphNode, quoted(pluginPath), control, target);
}

QString setParamCommand(const Controls &controls, const Controls *previous) {
    QStringList params;
    for (int i = 0; i < ParamCount; ++i)
        if (!previous || (*previous)[i] != controls[i])
            params << QStringLiteral("\"%1:%2\" %3").arg(kGraphNode, kParams[i].name, controlValue(i, controls[i]));
    if (params.isEmpty())
        return {};
    return QStringLiteral("set-param atomicaudio Props { params = [ %1 ] }").arg(params.join(' '));
}
