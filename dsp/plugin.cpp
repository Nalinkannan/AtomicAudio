// LADSPA glue so PipeWire's filter-chain can host the processor. The control
// ports come straight from params.h; audio ports follow them.
#include "ladspa.h"
#include "params.h"
#include "processor.h"

#include <new>

namespace {

using namespace atomic;

enum : unsigned long {
    PortInL = ParamCount,
    PortInR,
    PortOutL,
    PortOutR,
    PortCount
};

struct Instance {
    explicit Instance(double fs) : proc(fs) {}
    Processor proc;
    LADSPA_Data *ports[PortCount] = {};
};

LADSPA_Handle instantiate(const LADSPA_Descriptor *, unsigned long sampleRate) {
    return new (std::nothrow) Instance(double(sampleRate));
}

void connectPort(LADSPA_Handle handle, unsigned long port, LADSPA_Data *data) {
    if (port < PortCount)
        static_cast<Instance *>(handle)->ports[port] = data;
}

void activate(LADSPA_Handle handle) {
    auto *inst = static_cast<Instance *>(handle);
    for (int i = 0; i < ParamCount; ++i)
        if (inst->ports[i])
            inst->proc.setParam(i, *inst->ports[i]);
    inst->proc.reset();
}

void run(LADSPA_Handle handle, unsigned long frames) {
    auto *inst = static_cast<Instance *>(handle);
    for (int i = 0; i < ParamCount; ++i)
        if (inst->ports[i])
            inst->proc.setParam(i, *inst->ports[i]);
    if (!inst->ports[PortInL] || !inst->ports[PortInR] || !inst->ports[PortOutL] || !inst->ports[PortOutR] || frames == 0)
        return;
    inst->proc.process(inst->ports[PortInL], inst->ports[PortInR], inst->ports[PortOutL], inst->ports[PortOutR],
                       unsigned(frames));
}

void cleanup(LADSPA_Handle handle) { delete static_cast<Instance *>(handle); }

struct Descriptor {
    LADSPA_PortDescriptor portDescriptors[PortCount];
    const char *portNames[PortCount];
    LADSPA_PortRangeHint hints[PortCount];
    LADSPA_Descriptor d{};

    Descriptor() {
        for (int i = 0; i < ParamCount; ++i) {
            const auto &p = kParams[i];
            portDescriptors[i] = LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
            portNames[i] = p.name;
            int hint = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
            if (p.toggle)
                hint = LADSPA_HINT_TOGGLED | (p.def > 0.5f ? LADSPA_HINT_DEFAULT_1 : LADSPA_HINT_DEFAULT_0);
            else if (p.def == 0.0f)
                hint |= LADSPA_HINT_DEFAULT_0;
            else
                hint |= LADSPA_HINT_DEFAULT_MIDDLE;
            hints[i] = {hint, p.min, p.max};
        }
        const char *audioNames[] = {"InL", "InR", "OutL", "OutR"};
        for (unsigned long i = PortInL; i < PortCount; ++i) {
            portDescriptors[i] = (i < PortOutL ? LADSPA_PORT_INPUT : LADSPA_PORT_OUTPUT) | LADSPA_PORT_AUDIO;
            portNames[i] = audioNames[i - PortInL];
            hints[i] = {0, 0, 0};
        }
        d.UniqueID = 0x41544f4d; // "ATOM"
        d.Label = "atomicaudio";
        d.Properties = LADSPA_PROPERTY_REALTIME | LADSPA_PROPERTY_HARD_RT_CAPABLE;
        d.Name = "Atomic Audio";
        d.Maker = "Atomic Audio";
        d.Copyright = "MIT";
        d.PortCount = PortCount;
        d.PortDescriptors = portDescriptors;
        d.PortNames = portNames;
        d.PortRangeHints = hints;
        d.instantiate = instantiate;
        d.connect_port = connectPort;
        d.activate = activate;
        d.run = run;
        d.cleanup = cleanup;
    }
};

} // namespace

extern "C" __attribute__((visibility("default"))) const LADSPA_Descriptor *ladspa_descriptor(unsigned long index) {
    static Descriptor descriptor;
    return index == 0 ? &descriptor.d : nullptr;
}
