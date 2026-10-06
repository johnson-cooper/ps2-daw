// IOP bring-up and embedded module loading.
//
// Every IRX the program needs is embedded in the ELF by ps2build
// (embed_irx in ps2.yaml), so the program does not depend on files next to
// the ELF or on whatever the launcher left loaded on the IOP.
#pragma once

#include <stdint.h>

#include "core/status_log.hpp"

namespace ps2sys {

// Result of loading one module, kept for the debug overlay.
struct ModuleRecord {
    const char* name;
    int id;      // SifExecModuleBuffer return (< 0 = load error)
    int result;  // module start result
    bool ok;
};

constexpr int kMaxModules = 12;

// Resets the IOP to a clean state and applies the LoadModuleBuffer patch.
// Must run before any other SIF/RPC user.
bool resetIop(StatusLog& log);

// Loads an embedded IRX image. Returns true on success.
bool loadModule(const char* name, const void* image, unsigned size);

int moduleCount();
const ModuleRecord& module(int i);

// Microsecond clock for profiling and UI timing (EE timer, bus clock based).
uint64_t timeUs();

// Thread helpers.
void sleepUs(int us);
void setMainThreadPriority(int priority);

// Approximate heap use (newlib mallinfo) in bytes.
uint32_t heapUsed();

} // namespace ps2sys
