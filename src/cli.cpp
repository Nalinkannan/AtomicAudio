#include "cli.h"
#include "backend.h"
#include "engine.h"
#include "settings.h"
#include "system.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTextStream>
#include <QTimer>
#include <csignal>
#include <cmath>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

QTextStream &out() {
    static QTextStream s(stdout);
    return s;
}
QTextStream &err() {
    static QTextStream s(stderr);
    return s;
}

const QStringList &kAmounts = Amounts::keys();

int fail(const QString &message) {
    err() << "atomicaudio: " << message << Qt::endl;
    return 1;
}

void usage(QTextStream &s) {
    s << "Atomic Audio — Dolby-style sound for your laptop speakers\n\n"
         "Usage:\n"
         "  atomicaudio [open]                 Open the window\n"
         "  atomicaudio tui                    Control it from the terminal\n"
         "  atomicaudio status [--json]        Show the engine and the current sound\n"
         "  atomicaudio on | off               Start or stop the engine (stays that way across reboots)\n"
         "  atomicaudio mode <name>            dynamic, movie, music, game, voice or custom\n"
         "  atomicaudio set <effect> <0-100>   surround, dialogue, bass, leveler or loudness\n"
         "  atomicaudio set tuning <on|off>    The speaker optimizer\n"
         "  atomicaudio set speakers <curve>   auto, generic or rog-g14-2024 (the optimizer's curve)\n"
         "  atomicaudio eq <preset>            flat, open, rich, focused, custom (your own),\n"
         "                                     or one of the library's (atomicaudio eq list)\n"
         "  atomicaudio eq <10 gains in dB>    31 Hz … 16 kHz, -12 to 12\n"
         "  atomicaudio bypass [on|off]        Hear the original sound (toggles without an argument)\n"
         "  atomicaudio reset                  Back to the defaults\n"
         "  atomicaudio engine                 Run the engine (the systemd service does this)\n\n"
         "Add --speakers or --headphones to change that output's sound; otherwise the one\n"
         "playing now is changed.\n";
    s.flush();
}

// Which profile a command edits: the flag, else the output in use.
bool targetHeadphones(QStringList &args) {
    if (args.removeAll("--headphones"))
        return true;
    if (args.removeAll("--speakers"))
        return false;
    const EngineStatus status = EngineStatus::read(paths::statusFile());
    if (status.running && status.alive())
        return status.output.headphones;
    return queryOutput(2000).headphones;
}

bool parseOnOff(const QString &v, bool *ok) {
    *ok = true;
    if (v == "on" || v == "true" || v == "1" || v == "yes")
        return true;
    if (v == "off" || v == "false" || v == "0" || v == "no")
        return false;
    *ok = false;
    return false;
}

int save(const Settings &s) {
    QString error;
    if (!s.save(paths::settingsFile(), &error))
        return fail(error);
    return 0;
}

QJsonObject statusJson() {
    EngineStatus status = EngineStatus::read(paths::statusFile());
    if (status.running && !status.alive())
        status = EngineStatus();
    const OutputState output = status.running ? status.output : queryOutput(2000);
    const Settings settings = Settings::load(paths::settingsFile());
    QJsonObject o = settings.toJson();
    o.insert("engine", service::isActive());
    o.insert("filterRunning", status.running);
    o.insert("output", output.headphones ? "headphones" : "speakers");
    o.insert("sink", output.sink);
    o.insert("playingElsewhere", output.elsewhere());
    if (output.elsewhere())
        o.insert("defaultSink", output.defaultSink);
    o.insert("optimizerCurve", atomic::kSpeakerProfiles[settings.resolveSpeakerProfile(productName())].id);
    if (!status.error.isEmpty())
        o.insert("error", status.error);
    return o;
}

int status(bool json) {
    const QJsonObject o = statusJson();
    if (json) {
        out() << QJsonDocument(o).toJson(QJsonDocument::Indented);
        out().flush();
        return 0;
    }
    const bool hp = o.value("output").toString() == "headphones";
    const QJsonObject p = o.value(hp ? "headphones" : "speakers").toObject();
    const int curve = atomic::profileIndex(o.value("optimizerCurve").toString().toUtf8().constData());
    out() << "Engine:    " << (o.value("engine").toBool() ? (o.value("filterRunning").toBool() ? "on" : "starting")
                                                           : "off")
          << (o.value("bypass").toBool() ? " (bypassed)" : "") << "\n";
    out() << "Output:    " << (hp ? "headphones" : "speakers") << "  " << o.value("sink").toString() << "\n";
    if (o.value("playingElsewhere").toBool())
        out() << "           sound is going to " << o.value("defaultSink").toString()
              << ", which Atomic Audio doesn't process\n";
    out() << "Mode:      " << p.value("mode").toString() << "\n";
    for (const QString &k : kAmounts)
        out() << QStringLiteral("%1 %2%\n").arg(k.left(1).toUpper() + k.mid(1) + ":", -10).arg(percent(p.value(k).toDouble()));
    out() << "Optimizer: " << (p.value("tuning").toBool() ? "on" : "off") << " ("
          << (curve >= 0 ? atomic::kSpeakerProfiles[curve].name : "?") << ")\n";
    out() << "EQ:        " << p.value("eqPreset").toString() << "\n";
    if (o.contains("error"))
        out() << "Error:     " << o.value("error").toString() << "\n";
    out().flush();
    return 0;
}

int onOff(bool on) {
    QString error;
    bool changed = false;
    if (on && !service::ensureUserUnit(QCoreApplication::applicationFilePath(), &changed, &error))
        return fail(error);
    if (changed)
        service::run({"daemon-reload"});
    if (!service::run({on ? "enable" : "disable", "--now", service::kUnit}, &error))
        return fail(error);
    out() << "Atomic Audio is " << (on ? "on" : "off") << Qt::endl;
    return 0;
}

void quitOnSignals(QObject *owner);

int runEngine() {
    // SIGTERM from systemd quits the event loop, so the child filter stops too.
    quitOnSignals(qApp);

    Engine engine;
    QString error;
    if (!engine.start(&error))
        return fail(error);
    const int code = QCoreApplication::exec();
    engine.stop();
    return code;
}

// ---- Terminal UI -----------------------------------------------------------

class Tui : public QObject {
public:
    explicit Tui(Backend &backend) : m_backend(backend), m_notifier(STDIN_FILENO, QSocketNotifier::Read) {
        tcgetattr(STDIN_FILENO, &m_saved);
        termios raw = m_saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        out() << "\x1b[?1049h\x1b[?25l";
        connect(&m_notifier, &QSocketNotifier::activated, this, &Tui::onInput);
        connect(&m_backend, &Backend::profileChanged, this, &Tui::render);
        connect(&m_backend, &Backend::engineChanged, this, &Tui::render);
        render();
    }
    ~Tui() override {
        out() << "\x1b[?25h\x1b[?1049l";
        out().flush();
        tcsetattr(STDIN_FILENO, TCSANOW, &m_saved);
    }

private:
    static QString bar(double v, int width) {
        const int filled = int(std::round(v * width));
        return QString(filled, QChar(0x2588)) + QString(width - filled, QChar(0x2591));
    }

    void render() {
        const auto &b = m_backend;
        QString s = "\x1b[H\x1b[2J";
        s += "\x1b[1m  ATOMIC AUDIO\x1b[0m   ";
        s += b.engineOn() ? (b.filterRunning() ? "\x1b[32m● on\x1b[0m" : "\x1b[33m● starting\x1b[0m") : "\x1b[90m○ off\x1b[0m";
        if (b.bypass())
            s += "  \x1b[33m[comparing: original sound]\x1b[0m";
        s += QStringLiteral("   output: %1   editing: %2\r\n\r\n")
                 .arg(b.headphonesActive() ? "headphones" : "speakers", b.editingHeadphones() ? "headphones" : "speakers");

        s += "  ";
        const QStringList modes = b.modeIds();
        for (int i = 0; i < modes.size(); ++i) {
            const bool on = modes[i] == b.mode();
            s += QStringLiteral("%1%2 %3%4  ").arg(on ? "\x1b[7m" : "\x1b[90m").arg(i + 1).arg(modes[i], on ? "\x1b[0m" : "\x1b[0m");
        }
        s += "\r\n\r\n";

        const double values[] = {b.surround(), b.dialogue(), b.bass(), b.leveler(), b.loudness()};
        const char *labels[] = {"Surround", "Dialogue", "Bass", "Leveler", "Loudness"};
        for (int i = 0; i < 5; ++i) {
            const bool sel = i == m_row;
            s += QStringLiteral("  %1%2 %3 %4%\x1b[0m\r\n")
                     .arg(sel ? "\x1b[1m> " : "  ")
                     .arg(QString(labels[i]).leftJustified(10))
                     .arg(bar(values[i], 32))
                     .arg(percent(values[i]), 3);
        }
        s += QStringLiteral("  %1Optimizer  %2\x1b[0m\r\n\r\n")
                 .arg(m_row == 5 ? "\x1b[1m> " : "  ", b.tuning() ? "on" : "off");

        // EQ as ten vertical bars, one row per 3 dB, each band rounded to the
        // nearest row so a gentle +2 dB still shows.
        const QList<qreal> eq = b.eq();
        s += QStringLiteral("  EQ: %1%2%3\x1b[0m\r\n")
                 .arg(m_row == 6 ? "\x1b[1m" : "", b.eqPreset(),
                      m_row == 6 ? QStringLiteral("   band %1: %2 dB  (tab/,. pick, ←→ adjust)")
                                       .arg(int(atomic::kEqFreqs[m_band]))
                                       .arg(eq.value(m_band), 0, 'f', 1)
                                 : QString());
        for (int level = 12; level >= -12; level -= 3) {
            s += "  ";
            for (int band = 0; band < eq.size(); ++band) {
                const double g = eq[band];
                const bool fill = level > 0 ? g >= level - 1.5 : (level < 0 ? g <= level + 1.5 : true);
                const bool sel = m_row == 6 && band == m_band;
                s += QString(sel ? "\x1b[1;33m" : "") + (level == 0 ? "─────" : fill ? " ███ " : "     ") + "\x1b[0m";
            }
            s += "\r\n";
        }
        s += "   31   62  125  250  500   1k   2k   4k   8k  16k\r\n\r\n";
        s += "\x1b[90m  ↑↓ select  ←→ adjust  1-6 mode  e EQ preset  a compare  h speakers/headphones\r\n"
             "  space on/off  u undo  r redo  q quit\x1b[0m\r\n";
        if (b.playingElsewhere())
            s += "\r\n  \x1b[33mSound is going to " + b.elsewhereDescription() +
                 ", which Atomic Audio doesn't process.\x1b[0m\r\n";
        if (!b.error().isEmpty())
            s += "\r\n  \x1b[31m" + b.error() + "\x1b[0m\r\n";
        out() << s;
        out().flush();
    }

    void adjust(int dir) {
        if (m_row < 5)
            m_backend.nudge(kAmounts[m_row], 0.05 * dir);
        else if (m_row == 5)
            m_backend.setTuning(dir > 0);
        else
            m_backend.setEqBand(m_band, m_backend.eq().value(m_band) + dir);
    }

    void onInput() {
        char buf[64];
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            if (c == '\x1b' && i + 2 < n && buf[i + 1] == '[') {
                const char k = buf[i + 2];
                i += 2;
                if (k == 'A')
                    m_row = (m_row + 6) % 7;
                else if (k == 'B')
                    m_row = (m_row + 1) % 7;
                else if (k == 'C')
                    adjust(+1);
                else if (k == 'D')
                    adjust(-1);
                continue;
            }
            switch (c) {
            case 'q': case 'Q': QCoreApplication::quit(); return;
            case ' ': m_backend.toggleEngine(); break;
            case 'a': case 'A': m_backend.toggleBypass(); break;
            case 'e': case 'E': m_backend.cycleEqPreset(); break;
            case 'h': case 'H': m_backend.setEditingHeadphones(!m_backend.editingHeadphones()); break;
            case 'u': m_backend.undo(); break;
            case 'r': m_backend.redo(); break;
            case '\t': if (m_row == 6) m_band = (m_band + 1) % 10; break;
            case 'k': m_row = (m_row + 6) % 7; break;
            case 'j': m_row = (m_row + 1) % 7; break;
            case 'l': adjust(+1); break;
            default:
                if (c >= '1' && c <= '6')
                    m_backend.setMode(m_backend.modeIds().value(c - '1'));
                else if (m_row == 6 && c == ',')
                    m_band = (m_band + 9) % 10;
                else if (m_row == 6 && c == '.')
                    m_band = (m_band + 1) % 10;
            }
        }
        render();
    }

    Backend &m_backend;
    QSocketNotifier m_notifier;
    termios m_saved{};
    int m_row = 0;
    int m_band = 0;
};

// Signals quit the event loop instead of killing the process, so the
// terminal always gets its settings back.
void quitOnSignals(QObject *owner) {
    static int pipeFds[2] = {-1, -1};
    if (pipeFds[0] < 0 && ::pipe(pipeFds) != 0)
        return;
    auto handler = [](int) {
        const char c = 1;
        [[maybe_unused]] auto r = ::write(pipeFds[1], &c, 1);
    };
    std::signal(SIGTERM, handler);
    std::signal(SIGINT, handler);
    std::signal(SIGHUP, handler);
    auto *notifier = new QSocketNotifier(pipeFds[0], QSocketNotifier::Read, owner);
    QObject::connect(notifier, &QSocketNotifier::activated, qApp, &QCoreApplication::quit);
}

int runTui() {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return fail("tui needs a terminal");
    Backend backend;
    Tui tui(backend);
    quitOnSignals(&tui);
    return QCoreApplication::exec();
}

} // namespace

namespace cli {

bool handles(const QStringList &args) {
    static const QStringList commands = {"status", "on", "off", "mode", "set", "eq", "bypass", "reset",
                                         "engine", "tui", "help", "--help", "-h", "--version"};
    return !args.isEmpty() && commands.contains(args.first());
}

int run(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("atomicaudio");
    app.setApplicationVersion(APP_VERSION);

    QStringList args = app.arguments().mid(1);
    const QString command = args.takeFirst();

    if (command == "help" || command == "--help" || command == "-h") {
        usage(out());
        return 0;
    }
    if (command == "--version") {
        out() << "atomicaudio " << app.applicationVersion() << Qt::endl;
        return 0;
    }
    if (command == "engine")
        return runEngine();
    if (command == "tui")
        return runTui();
    if (command == "status")
        return status(args.contains("--json"));
    if (command == "on" || command == "off")
        return onOff(command == "on");

    const bool headphones = targetHeadphones(args);
    QString loadError;
    Settings s = Settings::load(paths::settingsFile(), &loadError);
    if (!loadError.isEmpty())
        return fail(loadError);
    Profile &p = s.profile(headphones);

    if (command == "mode") {
        if (args.size() != 1 || !Settings::applyMode(p, args.first(), headphones))
            return fail("mode must be one of dynamic, movie, music, game, voice, custom");
        return save(s);
    }
    if (command == "set") {
        if (args.size() != 2)
            return fail("usage: atomicaudio set <effect> <0-100>");
        const QString key = args[0];
        if (key == "tuning" || key == "optimizer") {
            bool ok;
            p.tuning = parseOnOff(args[1], &ok);
            return ok ? save(s) : fail("tuning is on or off");
        }
        if (key == "speakers" || key == "curve") {
            if (args[1] != "auto" && atomic::profileIndex(args[1].toUtf8().constData()) < 0)
                return fail("speakers is auto, generic or rog-g14-2024");
            s.speakerProfile = args[1];
            return save(s);
        }
        bool ok;
        const double v = args[1].toDouble(&ok);
        if (!ok || v < 0 || v > 100 || !kAmounts.contains(key))
            return fail("usage: atomicaudio set <surround|dialogue|bass|leveler|loudness> <0-100>");
        Settings::setAmount(p, key, v / 100.0);
        return save(s);
    }
    if (command == "eq") {
        if (args == QStringList{"list"}) {
            out() << Settings::allEqPresetIds().join("\n") << Qt::endl;
            return 0;
        }
        if (args.size() == 1) {
            if (!Settings::applyEqPreset(p, args.first()))
                return fail("unknown eq preset; `atomicaudio eq list` shows them all");
            return save(s);
        }
        if (args.size() != atomic::EqBands)
            return fail("eq takes a preset name or 10 gains in dB");
        for (int i = 0; i < atomic::EqBands; ++i) {
            bool ok;
            const double g = args[i].toDouble(&ok);
            if (!ok || g < -12 || g > 12)
                return fail(QStringLiteral("gain %1 must be between -12 and 12 dB").arg(args[i]));
            Settings::setEqBand(p, i, g);
        }
        return save(s);
    }
    if (command == "bypass") {
        if (args.isEmpty()) {
            s.bypass = !s.bypass;
        } else {
            bool ok;
            s.bypass = parseOnOff(args.first(), &ok);
            if (!ok)
                return fail("bypass is on or off");
        }
        return save(s);
    }
    if (command == "reset") {
        p = Settings::defaultProfile(headphones);
        return save(s);
    }
    return fail("unknown command " + command);
}

} // namespace cli
