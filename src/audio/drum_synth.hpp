// Programmatically generated built-in sound kit.
//
// Lets the DAW make real sound on first boot with no files on any device,
// and avoids shipping any third-party sample content. All synthesis is in
// single-precision float (the EE has no hardware double support) and runs
// once at boot, never in the audio thread.
#pragma once

#include "audio/sample.hpp"

namespace drumsynth {

enum class Kind : uint8_t {
    Kick,
    Snare,
    ClosedHat,
    OpenHat,
    Clap,
    Tom,
    Rim,
    Bass,
    TestTone,
    Count
};

const char* name(Kind k);

// Generates every built-in sound into the bank, in Kind order, so that slot
// index == (int)Kind for the built-ins. Returns the number generated.
int generateKit(SampleBank& bank);

} // namespace drumsynth
