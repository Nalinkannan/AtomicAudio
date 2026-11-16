// The engine's controls, shared by the LADSPA plugin (as its control ports)
// and the app (which writes them into PipeWire's filter-chain), so the two can
// never disagree about a name, range, or order.
#pragma once

namespace atomic {

enum Param : int {
    Bypass,
    Headphones,
    Leveler,
    Dialogue,
    Surround,
    Bass,
    Loudness,
    Tuning,
    Ceiling,
    Eq1, Eq2, Eq3, Eq4, Eq5, Eq6, Eq7, Eq8, Eq9, Eq10,
    // Which speaker optimizer curve (index into kSpeakerProfiles).
    SpeakerCurve,
    // The output's volume in dB (0 = full); quieter listening gets more bass
    // and treble back, the way hearing needs it.
    Volume,
    ParamCount
};

constexpr int EqBands = 10;

struct ParamInfo {
    const char *name;
    float min;
    float max;
    float def;
    bool toggle;
};

inline constexpr ParamInfo kParams[ParamCount] = {
    {"Bypass", 0, 1, 0, true},
    {"Headphones", 0, 1, 0, true},
    {"Leveler", 0, 1, 0.4f, false},
    {"Dialogue", 0, 1, 0.3f, false},
    {"Surround", 0, 1, 0.45f, false},
    {"Bass", 0, 1, 0.55f, false},
    {"Loudness", 0, 1, 0.5f, false},
    {"Tuning", 0, 1, 1, true},
    {"Ceiling", -6, 0, -1, false},
    {"EQ1", -12, 12, 0, false},
    {"EQ2", -12, 12, 0, false},
    {"EQ3", -12, 12, 0, false},
    {"EQ4", -12, 12, 0, false},
    {"EQ5", -12, 12, 0, false},
    {"EQ6", -12, 12, 0, false},
    {"EQ7", -12, 12, 0, false},
    {"EQ8", -12, 12, 0, false},
    {"EQ9", -12, 12, 0, false},
    {"EQ10", -12, 12, 0, false},
    {"Profile", 0, 1, 0, false},
    {"Volume", -90, 0, 0, false},
};

inline constexpr double kEqFreqs[EqBands] = {31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
inline constexpr double kEqQ = 1.1;

// The node name the plugin gets inside the filter graph; filter-chain exposes
// each control as "<node>:<port>", e.g. "dsp:Bass".
inline constexpr const char *kGraphNode = "dsp";

} // namespace atomic
