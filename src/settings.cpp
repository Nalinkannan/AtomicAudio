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

QJsonObject profileToJson(const Profile &p) {
    QJsonArray eq;
    for (double g : p.eq)
        eq.append(g);
    return {
        {"mode", p.mode},         {"leveler", p.leveler},   {"dialogue", p.dialogue}, {"surround", p.surround},
        {"bass", p.bass},         {"loudness", p.loudness}, {"tuning", p.tuning},     {"eqPreset", p.eqPreset},
        {"eq", eq},
    };
}

Profile profileFromJson(const QJsonValue &value, const Profile &fallback) {
    if (!value.isObject())
        return fallback;
    const QJsonObject o = value.toObject();
    Profile p = fallback;
    if (Settings::findMode(o.value("mode").toString()) || o.value("mode").toString() == "custom")
        p.mode = o.value("mode").toString();
    p.leveler = clamp01(o.value("leveler").toDouble(p.leveler));
    p.dialogue = clamp01(o.value("dialogue").toDouble(p.dialogue));
    p.surround = clamp01(o.value("surround").toDouble(p.surround));
    p.bass = clamp01(o.value("bass").toDouble(p.bass));
    p.loudness = clamp01(o.value("loudness").toDouble(p.loudness));
    p.tuning = o.value("tuning").toBool(p.tuning);
    const QString preset = o.value("eqPreset").toString();
    if (Settings::findEqPreset(preset) || preset == "custom")
        p.eqPreset = preset;
    const QJsonArray eq = o.value("eq").toArray();
    for (int i = 0; i < EqBands && i < eq.size(); ++i) {
        const double g = eq.at(i).toDouble(0);
        p.eq[i] = std::isfinite(g) ? std::clamp(g, -12.0, 12.0) : 0.0;
    }
    return p;
}

} // namespace

bool Profile::operator==(const Profile &o) const {
    return mode == o.mode && leveler == o.leveler && dialogue == o.dialogue && surround == o.surround && bass == o.bass &&
           loudness == o.loudness && tuning == o.tuning && eqPreset == o.eqPreset && eq == o.eq;
}

// Modes follow Dolby Access's: what each one turns up is what that kind of
// content needs from laptop speakers.
const QList<Mode> &Settings::modes() {
    static const QList<Mode> list = {
        {"dynamic", 0.40, 0.30, 0.45, 0.55, 0.50},
        {"movie", 0.50, 0.55, 0.80, 0.65, 0.55},
        {"music", 0.15, 0.00, 0.40, 0.60, 0.45},
        {"game", 0.30, 0.25, 0.90, 0.50, 0.55},
        {"voice", 0.70, 0.90, 0.10, 0.10, 0.50},
    };
    return list;
}

const QList<EqPreset> &Settings::eqPresets() {
    static const QList<EqPreset> list = {
        {"flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {"open", {0, 0, 0, -0.5, -0.5, 0, 0.5, 1.5, 2.5, 3.0}},
        {"rich", {2.0, 3.0, 2.5, 1.0, 0, -0.5, 0, 0.5, 1.0, 1.0}},
        {"focused", {-2.0, -1.5, -1.0, 0, 1.0, 2.0, 2.5, 1.5, 0, -1.0}},
    };
    return list;
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

Profile Settings::defaultProfile(bool forHeadphones) {
    Profile p;
    applyMode(p, "dynamic", forHeadphones);
    return p;
}

bool Settings::applyMode(Profile &profile, const QString &id, bool forHeadphones) {
    if (id == "custom") {
        profile.mode = id;
        return true;
    }
    const Mode *m = findMode(id);
    if (!m)
        return false;
    profile.mode = id;
    profile.leveler = m->leveler;
    profile.dialogue = m->dialogue;
    profile.surround = m->surround;
    profile.bass = m->bass;
    // Headphones need far less help to be loud.
    profile.loudness = forHeadphones ? std::min(m->loudness, 0.3) : m->loudness;
    return true;
}

bool Settings::applyEqPreset(Profile &profile, const QString &id) {
    if (id == "custom") {
        profile.eqPreset = id;
        return true;
    }
    const EqPreset *p = findEqPreset(id);
    if (!p)
        return false;
    profile.eqPreset = id;
    profile.eq = p->gains;
    return true;
}

QJsonObject Settings::toJson() const {
    return {
        {"version", 1},
        {"bypass", bypass},
        {"ceiling", ceiling},
        {"speakers", profileToJson(speakers)},
        {"headphones", profileToJson(headphones)},
    };
}

Settings Settings::fromJson(const QJsonObject &o) {
    Settings s;
    s.bypass = o.value("bypass").toBool(false);
    const double ceiling = o.value("ceiling").toDouble(-1.0);
    s.ceiling = std::isfinite(ceiling) ? std::clamp(ceiling, -6.0, 0.0) : -1.0;
    s.speakers = profileFromJson(o.value("speakers"), defaultProfile(false));
    s.headphones = profileFromJson(o.value("headphones"), defaultProfile(true));
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

Controls Settings::controls(bool forHeadphones) const {
    const Profile &p = profile(forHeadphones);
    Controls c{};
    c[Bypass] = bypass ? 1.0f : 0.0f;
    c[Headphones] = forHeadphones ? 1.0f : 0.0f;
    c[Leveler] = float(p.leveler);
    c[Dialogue] = float(p.dialogue);
    c[Surround] = float(p.surround);
    c[Bass] = float(p.bass);
    c[Loudness] = float(p.loudness);
    c[Tuning] = p.tuning ? 1.0f : 0.0f;
    c[Ceiling] = float(ceiling);
    for (int i = 0; i < EqBands; ++i)
        c[Eq1 + i] = float(p.eq[i]);
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
