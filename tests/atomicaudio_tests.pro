QT += core gui quick quickcontrols2 testlib
CONFIG += c++17 testcase
TARGET = atomicaudio_tests
TEMPLATE = app

INCLUDEPATH += ../src ../dsp

HEADERS += ../dsp/profiles.h ../src/analyzer.h ../src/backend.h ../src/settings.h ../src/system.h ../src/theme.h ../src/translator.h \
           ../dsp/processor.h ../dsp/biquad.h ../dsp/params.h
SOURCES += atomicaudio_tests.cpp ../src/analyzer.cpp ../src/backend.cpp ../src/settings.cpp ../src/system.cpp \
           ../src/theme.cpp ../src/translator.cpp ../dsp/processor.cpp
