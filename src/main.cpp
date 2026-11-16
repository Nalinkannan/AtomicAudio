// atomicaudio — Dolby-style sound enhancement for laptop speakers on PipeWire.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "analyzer.h"
#include "backend.h"
#include "cli.h"
#include "system.h"
#include "theme.h"
#include "translator.h"

int main(int argc, char *argv[]) {
    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);
    if (cli::handles(args))
        return cli::run(argc, argv);
    if (!args.isEmpty() && args.first() != "open") {
        fprintf(stderr, "atomicaudio: unknown command %s (try atomicaudio --help)\n", argv[1]);
        return 1;
    }

    QGuiApplication app(argc, argv);
    app.setApplicationName("atomicaudio");
    app.setApplicationDisplayName("Atomic Audio");
    app.setApplicationVersion(APP_VERSION);

    // Matches the window to atomicaudio.desktop, so the compositor (Wayland
    // app_id) and the launcher pick up the installed icon.
    app.setDesktopFileName("atomicaudio");
    app.setWindowIcon(QIcon::fromTheme("atomicaudio"));

    QQuickStyle::setStyle("Material");

    // Installed before anything is translated, and every qsTr() binding is
    // re-evaluated when the Omarchy language changes.
    Translator translator;
    app.installTranslator(&translator);

    service::dropStaleUserUnit();

    Theme theme;
    Backend backend;
    // What the speakers play (after processing), and what apps send in.
    Analyzer output, input;

    QQmlApplicationEngine engine;
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    engine.rootContext()->setContextProperty("translator", &translator);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("outputAnalyzer", &output);
    engine.rootContext()->setContextProperty("inputAnalyzer", &input);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return 1;

    return app.exec();
}
