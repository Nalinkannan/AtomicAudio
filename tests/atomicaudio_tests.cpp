#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <random>
#include <vector>

#include "analyzer.h"
#include "backend.h"
#include "processor.h"
#include "settings.h"
#include "system.h"
#include "theme.h"
#include "translator.h"

using namespace atomic;

namespace {

constexpr double kFs = 48000;

// Every effect off, nothing boosted: the chain should be transparent.
void flatten(Processor &p) {
    for (int i = 0; i < ParamCount; ++i)
        p.setParam(i, 0);
}

// Gain in dB of a steady sine through the processor, measured once settled.
double sineGainDb(Processor &p, double freq, double amp = 0.1, bool antiphase = false) {
    const int block = 512, blocks = 120;
    std::vector<float> l(block), r(block), ol(block), orr(block);
    double phase = 0, in = 0, out = 0;
    for (int b = 0; b < blocks; ++b) {
        for (int i = 0; i < block; ++i) {
            l[i] = float(amp * std::sin(phase));
            r[i] = antiphase ? -l[i] : l[i];
            phase += 2 * M_PI * freq / kFs;
        }
        p.process(l.data(), r.data(), ol.data(), orr.data(), block);
        if (b > blocks / 2)
            for (int i = 0; i < block; ++i) {
                in += double(l[i]) * l[i];
                out += double(ol[i]) * ol[i];
            }
    }
    return 10 * std::log10(out / in);
}

QByteArray sinksJson(const QString &activePort) {
    return QStringLiteral(R"([
      {"name":"alsa_output.hdmi","description":"HDMI","active_port":"hdmi-output-0",
       "ports":[{"name":"hdmi-output-0","type":"HDMI"}]},
      {"name":"alsa_output.pci.analog-stereo","description":"Ryzen HD Audio","active_port":"%1",
       "ports":[{"name":"analog-output-speaker","type":"Speaker"},{"name":"analog-output-headphones","type":"Headphones"}]}
    ])").arg(activePort).toUtf8();
}

void write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(contents);
}

} // namespace

class AtomicAudioTests : public QObject {
    Q_OBJECT

private slots:
    // ---- DSP ----------------------------------------------------------------

    void flatChainIsTransparent() {
        for (double f : {100.0, 1000.0, 10000.0}) {
            Processor p(kFs);
            flatten(p);
            QVERIFY2(std::fabs(sineGainDb(p, f)) < 0.05, qPrintable(QString::number(f)));
        }
    }

    void eqBandHitsItsGain() {
        Processor p(kFs);
        flatten(p);
        p.setParam(Eq1 + 5, 6); // 1 kHz
        QVERIFY(std::fabs(sineGainDb(p, 1000, 0.05) - 6.0) < 0.1);
    }

    void optimizerCutsTheResonanceAndTheUnplayableLows() {
        Processor p(kFs);
        flatten(p);
        p.setParam(Tuning, 1);
        QVERIFY(sineGainDb(p, 630, 0.05) < -6.0);
        QVERIFY(sineGainDb(p, 50, 0.05) < -12.0);
    }

    void headphonesSkipTheSpeakerOptimizer() {
        Processor p(kFs);
        flatten(p);
        p.setParam(Tuning, 1);
        p.setParam(Headphones, 1);
        QVERIFY(std::fabs(sineGainDb(p, 630, 0.05)) < 0.1);
    }

    void virtualBassRebuildsWhatTheSpeakersCantPlay() {
        Processor tuned(kFs);
        flatten(tuned);
        tuned.setParam(Tuning, 1);
        Processor bass(kFs);
        flatten(bass);
        bass.setParam(Tuning, 1);
        bass.setParam(Bass, 1);
        // A 50 Hz tone is mostly gone after the optimizer; the harmonics
        // bring its energy back at frequencies the speakers can play.
        QVERIFY(sineGainDb(bass, 50, 0.1) > sineGainDb(tuned, 50, 0.1) + 10);
    }

    void surroundWidensWithoutThinningTheCenter() {
        Processor p(kFs);
        flatten(p);
        p.setParam(Surround, 1);
        QVERIFY(std::fabs(sineGainDb(p, 1000, 0.05)) < 1.5);
        Processor side(kFs);
        flatten(side);
        side.setParam(Surround, 1);
        QVERIFY(sineGainDb(side, 1000, 0.05, true) > 6.0);
    }

    void limiterNeverPassesTheCeiling() {
        Processor p(kFs);
        for (int i = 0; i < ParamCount; ++i)
            p.setParam(i, kParams[i].max);
        p.setParam(Bypass, 0);
        p.setParam(Headphones, 0);
        p.setParam(Ceiling, -1);
        const double ceiling = std::pow(10.0, -1.0 / 20.0);
        std::mt19937 rng(1);
        std::normal_distribution<float> noise(0, 0.6f);
        std::vector<float> l(333), r(333), ol(333), orr(333);
        double peak = 0;
        for (int b = 0; b < 600; ++b) {
            for (size_t i = 0; i < l.size(); ++i) {
                l[i] = noise(rng);
                r[i] = noise(rng) * 0.3f;
            }
            p.process(l.data(), r.data(), ol.data(), orr.data(), unsigned(l.size()));
            for (size_t i = 0; i < l.size(); ++i) {
                QVERIFY(std::isfinite(ol[i]) && std::isfinite(orr[i]));
                peak = std::max({peak, double(std::fabs(ol[i])), double(std::fabs(orr[i]))});
            }
        }
        QVERIFY(peak <= ceiling + 1e-6);
        QVERIFY(peak > ceiling * 0.9); // It is actually loud.
    }

    void bypassIsTheInputDelayed() {
        Processor p(kFs);
        p.setParam(Bypass, 1);
        p.reset();
        std::mt19937 rng(2);
        std::uniform_real_distribution<float> u(-1, 1);
        std::vector<float> history;
        std::vector<float> l(256), r(256), ol(256), orr(256);
        for (int b = 0; b < 20; ++b) {
            for (int i = 0; i < 256; ++i) {
                l[i] = r[i] = u(rng);
                history.push_back(l[i]);
            }
            p.process(l.data(), r.data(), ol.data(), orr.data(), 256);
            for (int i = 0; i < 256; ++i) {
                const long src = long(b) * 256 + i - p.latency();
                QCOMPARE(ol[i], src >= 0 ? history[src] : 0.0f);
            }
        }
    }

    void inPlaceProcessingWorks() {
        Processor p(kFs);
        flatten(p);
        std::vector<float> l(480), r(480);
        double phase = 0, energy = 0;
        for (int b = 0; b < 100; ++b) {
            for (int i = 0; i < 480; ++i) {
                l[i] = r[i] = float(0.1 * std::sin(phase));
                phase += 2 * M_PI * 1000 / kFs;
            }
            p.process(l.data(), r.data(), l.data(), r.data(), 480);
            if (b >= 50)
                for (float v : l)
                    energy += double(v) * v;
        }
        QVERIFY(std::fabs(10 * std::log10(energy / (50 * 480) / 0.005)) < 0.05);
    }

    void badParametersFallBackToDefaults() {
        Processor p(kFs);
        p.setParam(Bass, NAN);
        QCOMPARE(p.param(Bass), kParams[Bass].def);
        p.setParam(Eq1, 99);
        QCOMPARE(p.param(Eq1), 12.0f);
        p.setParam(-1, 1);
        p.setParam(ParamCount, 1);
    }

    void silenceStaysSilent() {
        Processor p(kFs);
        std::vector<float> z(1024, 0.0f), ol(1024), orr(1024);
        for (int b = 0; b < 50; ++b)
            p.process(z.data(), z.data(), ol.data(), orr.data(), 1024);
        for (float v : ol)
            QCOMPARE(v, 0.0f);
    }

    // ---- Settings -----------------------------------------------------------

    void settingsRoundTripThroughJson() {
        Settings s;
        s.bypass = true;
        s.ceiling = -2;
        Settings::applyMode(s.speakers, "music", false);
        s.speakers.eq[3] = 4.5;
        s.headphones.tuning = false;
        const Settings back = Settings::fromJson(s.toJson());
        QCOMPARE(back.toJson(), s.toJson());
    }

    void garbageSettingsAreClamped() {
        const auto doc = QJsonDocument::fromJson(R"({"ceiling": 40, "speakers": {"mode": "loud", "bass": 9,
            "surround": -3, "eq": [100, -100, "x"], "eqPreset": "nope"}})");
        const Settings s = Settings::fromJson(doc.object());
        QCOMPARE(s.ceiling, 0.0);
        QCOMPARE(s.speakers.mode, QString("dynamic"));
        QCOMPARE(s.speakers.bass, 1.0);
        QCOMPARE(s.speakers.surround, 0.0);
        QCOMPARE(s.speakers.eq[0], 12.0);
        QCOMPARE(s.speakers.eq[1], -12.0);
        QCOMPARE(s.speakers.eq[2], 0.0);
        QCOMPARE(s.speakers.eqPreset, QString("flat"));
    }

    void settingsSaveAndLoad() {
        QTemporaryDir dir;
        const QString path = dir.filePath("nested/settings.json");
        Settings s;
        s.speakers.bass = 0.9;
        QString error;
        QVERIFY(s.save(path, &error));
        QCOMPARE(Settings::load(path).speakers.bass, 0.9);
        write(dir.filePath("broken.json"), "{ nope");
        Settings::load(dir.filePath("broken.json"), &error);
        QVERIFY(!error.isEmpty());
    }

    void modesAndPresetsApply() {
        Profile p;
        QVERIFY(Settings::applyMode(p, "voice", false));
        QCOMPARE(p.dialogue, 0.9);
        QVERIFY(Settings::applyMode(p, "movie", true));
        QVERIFY(p.loudness <= 0.3); // Headphones need less.
        QVERIFY(!Settings::applyMode(p, "nope", false));
        QVERIFY(Settings::applyEqPreset(p, "rich"));
        QCOMPARE(p.eq[1], 3.0);
    }

    void controlsFollowTheOutput() {
        Settings s;
        s.speakers.bass = 0.2;
        s.headphones.bass = 0.8;
        QCOMPARE(s.controls(false)[Bass], 0.2f);
        QCOMPARE(s.controls(true)[Bass], 0.8f);
        QCOMPARE(s.controls(true)[Headphones], 1.0f);
    }

    void engineConfigTargetsTheSpeakers() {
        Settings s;
        s.speakers.tuning = false;
        const QString conf = engineConfig(s.controls(false), "/usr/lib/ladspa/atomicaudio.so", "alsa_output.x");
        QVERIFY(conf.contains("filter.smart.target = { node.name = \"alsa_output.x\" }"));
        QVERIFY(conf.contains("plugin = \"/usr/lib/ladspa/atomicaudio.so\""));
        // Toggles must be numbers in the file, or filter-chain ignores them.
        QVERIFY(conf.contains("\"Tuning\" = 0"));
        QVERIFY(!conf.contains("= false"));
        QVERIFY(!engineConfig(s.controls(false), "/p.so", {}).contains("filter.smart.target"));
    }

    void setParamSendsOnlyChanges() {
        Settings s;
        const Controls before = s.controls(false);
        s.speakers.bass = 0.75;
        s.bypass = true;
        const QString cmd = setParamCommand(s.controls(false), &before);
        QVERIFY(cmd.startsWith("set-param atomicaudio Props"));
        QVERIFY(cmd.contains("\"dsp:Bass\" 0.75"));
        QVERIFY(cmd.contains("\"dsp:Bypass\" true"));
        QVERIFY(!cmd.contains("Surround"));
        QVERIFY(setParamCommand(before, &before).isEmpty());
        QVERIFY(setParamCommand(before, nullptr).contains("EQ10"));
    }

    // ---- System -------------------------------------------------------------

    void findsTheSpeakersAndTheJack() {
        OutputState speakers = parseSinks(sinksJson("analog-output-speaker"));
        QVERIFY(speakers.found);
        QCOMPARE(speakers.sink, QString("alsa_output.pci.analog-stereo"));
        QVERIFY(!speakers.headphones);
        QVERIFY(parseSinks(sinksJson("analog-output-headphones")).headphones);
        QVERIFY(!parseSinks(R"([{"name":"bt","ports":[]}])").found);
        QVERIFY(!parseSinks("not json").found);
    }

    void statusRoundTrips() {
        QTemporaryDir dir;
        EngineStatus s;
        s.running = true;
        s.headphones = true;
        s.sink = "x";
        s.error = "oops";
        QVERIFY(s.write(dir.filePath("run/status.json")));
        QCOMPARE(EngineStatus::read(dir.filePath("run/status.json")).toJson(), s.toJson());
        QVERIFY(!EngineStatus::read(dir.filePath("missing.json")).running);
    }

    void userUnitRunsTheEngine() {
        const QString unit = service::unitText("/opt/my build/atomicaudio");
        QVERIFY(unit.startsWith("# Written by atomicaudio"));
        QVERIFY(unit.contains("ExecStart=\"/opt/my build/atomicaudio\" engine"));
        QVERIFY(unit.contains("WantedBy=pipewire.service"));
    }

    // ---- Backend ------------------------------------------------------------

    void movingASliderMakesTheModeCustom() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("settings.json"), nullptr, false);
        backend.setMode("music");
        QCOMPARE(backend.mode(), QString("music"));
        backend.setAmount("bass", 0.9);
        QCOMPARE(backend.mode(), QString("custom"));
        QCOMPARE(backend.bass(), 0.9);
        QTRY_COMPARE(Settings::load(dir.filePath("settings.json")).speakers.bass, 0.9);
    }

    void undoAndRedo() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("settings.json"), nullptr, false);
        const double original = backend.surround();
        backend.setAmount("surround", 0.1);
        backend.setAmount("surround", 0.2); // Same gesture: one step.
        backend.setMode("voice");
        QVERIFY(backend.canUndo());
        backend.undo();
        QCOMPARE(backend.mode(), QString("custom"));
        QCOMPARE(backend.surround(), 0.2);
        backend.undo();
        QCOMPARE(backend.surround(), original);
        QVERIFY(!backend.canUndo());
        backend.redo();
        QCOMPARE(backend.surround(), 0.2);
        QVERIFY(backend.canRedo());
    }

    void eqBandsSnapClampAndGoCustom() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("settings.json"), nullptr, false);
        backend.setEqBand(4, 3.3);
        QCOMPARE(backend.eq()[4], 3.5);
        QCOMPARE(backend.eqPreset(), QString("custom"));
        backend.setEqBand(4, 40);
        QCOMPARE(backend.eq()[4], 12.0);
        backend.setEqPreset("flat");
        QCOMPARE(backend.eq()[4], 0.0);
        backend.cycleEqPreset();
        QCOMPARE(backend.eqPreset(), QString("open"));
    }

    void speakersAndHeadphonesAreEditedSeparately() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("settings.json"), nullptr, false);
        backend.setAmount("bass", 0.1);
        backend.setEditingHeadphones(true);
        QVERIFY(backend.bass() != 0.1);
        backend.setAmount("bass", 0.7);
        backend.setEditingHeadphones(false);
        QCOMPARE(backend.bass(), 0.1);
        QCOMPARE(backend.settings().headphones.bass, 0.7);
    }

    void eqCurvePeaksWhereTheBandIs() {
        QTemporaryDir dir;
        Backend backend(dir.filePath("settings.json"), nullptr, false);
        backend.setEqBand(5, 6); // 1 kHz
        const int points = 301;
        const auto curve = backend.eqResponse(points);
        int peak = 0;
        for (int i = 1; i < points; ++i)
            if (curve[i] > curve[peak])
                peak = i;
        QVERIFY(std::fabs(Backend::frequencyAt(peak, points) / 1000.0 - 1.0) < 0.05);
        QVERIFY(std::fabs(curve[peak] - 6.0) < 0.1);
    }

    void cliChangesShowUpInTheWindow() {
        QTemporaryDir dir;
        const QString path = dir.filePath("settings.json");
        Backend backend(path, nullptr, false);
        Settings s;
        s.speakers.dialogue = 0.77;
        QVERIFY(s.save(path));
        QTRY_COMPARE(backend.dialogue(), 0.77);
    }

    // ---- Analyzer -----------------------------------------------------------

    void analyzerFindsATone() {
        Analyzer analyzer;
        std::vector<float> buf(2 * Analyzer::kFft);
        for (int i = 0; i < Analyzer::kFft; ++i)
            buf[2 * i] = buf[2 * i + 1] = float(0.25 * std::sin(2 * M_PI * 1000 * i / 48000.0));
        analyzer.feed(buf.data(), Analyzer::kFft);
        analyzer.analyze();
        const auto bands = analyzer.bands();
        int loudest = 0;
        for (int b = 0; b < bands.size(); ++b)
            if (bands[b] > bands[loudest])
                loudest = b;
        const double center = 30 * std::pow(20000.0 / 30, (loudest + 0.5) / Analyzer::kBands);
        QVERIFY2(center > 850 && center < 1180, qPrintable(QString::number(center)));
        QVERIFY(analyzer.receiving());
    }

    // ---- Theme and language -------------------------------------------------

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QString("black"));
        QVERIFY(theme.dark());
    }

    void themeReadsColors() {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml", "accent = \"#112233\"\nbackground = \"#faf4ed\"\nforeground = \"#575279\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QString("#112233"));
        QCOMPARE(theme.accentForeground(), QString("white"));
        QCOMPARE(theme.background(), QString("#faf4ed"));
        QVERIFY(!theme.dark());
    }

    void themeFollowsHyprlandRounding() {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":0}");
        Theme theme(dir.path(), nullptr, command);
        QCOMPARE(theme.radius(), 0);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        write(dir.path() + "/language", "pt-BR\n");
        write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})");
        write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }
};

QTEST_MAIN(AtomicAudioTests)
#include "atomicaudio_tests.moc"
