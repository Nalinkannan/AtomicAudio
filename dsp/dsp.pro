# The DSP engine as a LADSPA plugin for PipeWire's filter-chain. No Qt.
QT =
CONFIG += c++17 release plugin no_plugin_name_prefix
CONFIG -= qt
TEMPLATE = lib
TARGET = atomicaudio

QMAKE_CXXFLAGS_RELEASE += -O3 -fvisibility=hidden
QMAKE_LFLAGS += -Wl,--no-undefined

HEADERS += ladspa.h params.h biquad.h processor.h
SOURCES += processor.cpp plugin.cpp
