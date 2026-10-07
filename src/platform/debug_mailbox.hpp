// Debug mailbox: a fixed, symbol-addressable block of RAM.
//
// It exists so a debugger (the PCSX2 MCP, or a hardware debugger) can drive
// and observe the program without a controller: write `inputMask` to inject a
// button press, write `command` to start a built-in self-test, and read the
// `telemetry` words that the app refreshes every frame. It carries no
// behaviour a user can reach and costs a few stores per frame.
//
// Locate it with the `g_debugMailbox` symbol in the unstripped ELF
// (build/obj/ps2daw/ps2daw.unstripped.elf), or by searching RAM for the magic.
#pragma once

#include <stdint.h>

struct DebugMailbox {
    static constexpr uint32_t kMagic = 0x44443250; // "P2DD" little-endian

    volatile uint32_t magic;
    // Debugger -> app. Bits are btn:: values; held for one frame, then cleared.
    volatile uint32_t inputMask;
    // Debugger -> app. Nonzero starts a command; cleared when it was accepted.
    volatile uint32_t command;
    volatile uint32_t arg;
    // App -> debugger, refreshed every frame (see App::publishTelemetry()).
    volatile uint32_t telemetry[32];
};

enum DebugCommand : uint32_t {
    DbgNone = 0,
    DbgSampleSelfTest = 1,     // load synthetic WAVs through the real library path
    DbgReleaseUnreferenced = 2,
    DbgSwitchView = 3,         // arg = ViewId
};

extern DebugMailbox g_debugMailbox;
