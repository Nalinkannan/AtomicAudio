// Speaker optimizer curves. Each laptop's speakers have their own problems
// (where they stop playing, which cavity rings), so the curve is per model,
// matched on the DMI product name; everything else gets a gentle generic one.
// Shared by the plugin (which applies the curve picked by the "Profile"
// control) and the app (which detects the model and draws the curve).
#pragma once

#include <array>
#include <cstring>

namespace atomic {

struct TuningBand {
    enum Kind { Peak, HighShelf } kind;
    double freq, q, db;
};

struct SpeakerProfile {
    const char *id;
    const char *name;
    // Substring of /sys/class/dmi/id/product_name; empty matches nothing.
    const char *dmiMatch;
    // Fourth-order high-pass: content below this only wastes amplifier headroom.
    double highpass;
    std::array<TuningBand, 5> bands;
};

inline constexpr std::array<SpeakerProfile, 2> kSpeakerProfiles = {{
    // Laptop speakers in general roll off below ~100 Hz, sound boxy around
    // 400-500 Hz and dull above 8 kHz; correct each only a little.
    {"generic", "Laptop speakers", "", 70,
     {{{TuningBand::Peak, 180, 0.9, 1.5},
       {TuningBand::Peak, 450, 1.2, -2.0},
       {TuningBand::Peak, 1000, 1.0, 0.0},
       {TuningBand::Peak, 3000, 1.2, 1.5},
       {TuningBand::HighShelf, 10000, 0.7, 1.5}}}},
    // ASUS ROG Zephyrus G14 (2024, GA403), measured with its own microphone:
    // the CS35L56 amps cut everything under ~95 Hz, the woofer cavity rings at
    // 630 Hz (+12 dB, Q ~4), 1-1.25 kHz is hot and the woofer/tweeter
    // handover leaves a dip around 2.5 kHz.
    {"rog-g14-2024", "ROG Zephyrus G14", "GA403", 80,
     {{{TuningBand::Peak, 150, 0.9, 2.0},
       {TuningBand::Peak, 630, 4.0, -7.0},
       {TuningBand::Peak, 1200, 1.2, -2.0},
       {TuningBand::Peak, 2500, 1.5, 2.5},
       {TuningBand::HighShelf, 11000, 0.7, 1.5}}}},
}};

inline constexpr int kGenericProfile = 0;

inline int profileIndex(const char *id) {
    for (size_t i = 0; i < kSpeakerProfiles.size(); ++i)
        if (std::strcmp(kSpeakerProfiles[i].id, id) == 0)
            return int(i);
    return -1;
}

// The profile for a DMI product name, or the generic one.
inline int detectProfile(const char *productName) {
    for (size_t i = 0; i < kSpeakerProfiles.size(); ++i) {
        const char *match = kSpeakerProfiles[i].dmiMatch;
        if (match[0] && productName && std::strstr(productName, match))
            return int(i);
    }
    return kGenericProfile;
}

} // namespace atomic
