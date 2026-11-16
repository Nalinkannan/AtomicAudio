QT += core gui qml quick quickcontrols2

CONFIG += c++17 release
TARGET = atomicaudio
TEMPLATE = app

# The app's version; keep pkgver in pkgbuild/PKGBUILD in step.
VERSION = 0.2.0
DEFINES += APP_VERSION=\\\"$$VERSION\\\"

INCLUDEPATH += dsp

HEADERS += \
    dsp/biquad.h \
    dsp/params.h \
    dsp/processor.h \
    dsp/profiles.h \
    src/analyzer.h \
    src/backend.h \
    src/cli.h \
    src/engine.h \
    src/settings.h \
    src/system.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/analyzer.cpp \
    src/backend.cpp \
    src/cli.cpp \
    src/engine.cpp \
    src/settings.cpp \
    src/system.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
