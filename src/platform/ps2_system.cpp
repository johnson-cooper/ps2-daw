#include "platform/ps2_system.hpp"

#include <iopcontrol.h>
#include <kernel.h>
#include <delaythread.h>
#include <loadfile.h>
#include <malloc.h>
#include <sbv_patches.h>
#include <sifrpc.h>
#include <timer.h>

namespace ps2sys {
namespace {

ModuleRecord g_modules[kMaxModules];
int g_moduleCount = 0;

} // namespace

bool resetIop(StatusLog& log)
{
    // Launchers (uLaunchELF, OPL, FMCB, ps2link...) leave arbitrary modules on
    // the IOP. Rebooting it with the BIOS default image gives us a known
    // state; this is the same sequence wLaunchELF uses.
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {
    }
    while (!SifIopSync()) {
    }
    SifInitRpc(0);
    SifLoadFileInit();

    // Retail BIOS IOP kernels cannot load a module from an EE memory buffer;
    // this patch adds that (required for every SifExecModuleBuffer below).
    const int lmb = sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
    if (lmb != 0) {
        log.fail(Subsystem::Iop, "sbv_patch_enable_lmb failed (%d)", lmb);
        return false;
    }
    log.set(Subsystem::Iop, Health::Ok, "IOP reset, LMB patch applied");
    return true;
}

bool loadModule(const char* name, const void* image, unsigned size)
{
    int result = 0;
    const int id = SifExecModuleBuffer(const_cast<void*>(image), size, 0, nullptr, &result);
    // result 1 = NO_RESIDENT_END: the module ran and unloaded itself, which
    // for a driver means it refused to start.
    const bool ok = id >= 0 && result >= 0 && result != 1;
    if (g_moduleCount < kMaxModules)
        g_modules[g_moduleCount++] = ModuleRecord{name, id, result, ok};
    return ok;
}

int moduleCount() { return g_moduleCount; }
const ModuleRecord& module(int i) { return g_modules[i]; }

uint64_t timeUs()
{
    // GetTimerSystemTime counts bus clock ticks: 147.456 MHz = 18432/125 per us.
    return GetTimerSystemTime() * 125u / 18432u;
}

void sleepUs(int us)
{
    DelayThread(us);
}

void setMainThreadPriority(int priority)
{
    ChangeThreadPriority(GetThreadId(), priority);
}

uint32_t heapUsed()
{
    struct mallinfo mi = mallinfo();
    return (uint32_t)mi.uordblks;
}

} // namespace ps2sys
