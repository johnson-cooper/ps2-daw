#include "platform/ps2_input.hpp"

#include <libpad.h>
#include <string.h>

#include "padman_irx.h"
#include "sio2man_irx.h"

#include "platform/ps2_system.hpp"

namespace {

// libpad's DMA area for one port: 256 bytes, 64-byte aligned (required).
char g_padArea[256] __attribute__((aligned(64)));

struct Map {
    uint16_t pad;
    uint32_t ours;
};
const Map kMap[] = {
    {PAD_UP, btn::Up},         {PAD_DOWN, btn::Down},     {PAD_LEFT, btn::Left},   {PAD_RIGHT, btn::Right},
    {PAD_CROSS, btn::Cross},   {PAD_CIRCLE, btn::Circle}, {PAD_SQUARE, btn::Square}, {PAD_TRIANGLE, btn::Triangle},
    {PAD_L1, btn::L1},         {PAD_R1, btn::R1},         {PAD_L2, btn::L2},       {PAD_R2, btn::R2},
    {PAD_START, btn::Start},   {PAD_SELECT, btn::Select}, {PAD_L3, btn::L3},       {PAD_R3, btn::R3},
};

int axis(uint8_t raw, int deadzone)
{
    int v = (int)raw - 128;
    if (v > -deadzone && v < deadzone)
        return 0;
    // Rescale the live range so it still reaches +/-127 past the deadzone.
    const int sign = v < 0 ? -1 : 1;
    int mag = (v < 0 ? -v : v) - deadzone;
    mag = mag * 127 / (128 - deadzone);
    return sign * (mag > 127 ? 127 : mag);
}

int bitIndex(uint32_t b)
{
    int i = 0;
    while (b > 1) {
        b >>= 1;
        ++i;
    }
    return i;
}

} // namespace

bool InputState::heldFor(uint32_t b, uint32_t ms) const
{
    return (held & b) && heldMs[bitIndex(b)] >= ms;
}

bool Ps2Input::init(StatusLog& log)
{
    // Load our own SIO2 and pad drivers rather than rom0:SIO2MAN/PADMAN: the
    // ROM versions speak an older RPC protocol than the libpad we link, and
    // the IOP was just reset so nothing else owns the SIO2 bus.
    if (!ps2sys::loadModule("sio2man", sio2man_irx, size_sio2man_irx)) {
        log.fail(Subsystem::Controller, "sio2man.irx failed to load");
        return false;
    }
    if (!ps2sys::loadModule("padman", padman_irx, size_padman_irx)) {
        log.fail(Subsystem::Controller, "padman.irx failed to load");
        return false;
    }
    // padInit() returns a driver-defined status for the current padman;
    // only negative values are bind failures (wLaunchELF ignores it).
    const int initResult = padInit(0);
    if (initResult < 0) {
        log.fail(Subsystem::Controller, "padInit failed (%d)", initResult);
        return false;
    }
    if (padPortOpen(0, 0, g_padArea) == 0) {
        log.fail(Subsystem::Controller, "padPortOpen(0,0) failed");
        return false;
    }
    opened_ = true;
    phase_ = Phase::WaitStable;
    log.set(Subsystem::Controller, Health::Warning, "port 1 open, waiting for controller");
    return true;
}

const char* Ps2Input::stateName() const
{
    switch (phase_) {
    case Phase::Closed: return "closed";
    case Phase::WaitStable: return state_.connected ? "starting" : "no controller";
    case Phase::Configuring: return "configuring";
    case Phase::Ready: return state_.analog ? "DualShock (analog)" : "digital";
    }
    return "?";
}

void Ps2Input::update(uint32_t nowMs, StatusLog& log)
{
    const uint32_t dt = lastMs_ ? nowMs - lastMs_ : 0;
    lastMs_ = nowMs;

    uint32_t now = 0;
    state_.lx = state_.ly = state_.rx = state_.ry = 0;

    if (opened_) {
        padState_ = padGetState(0, 0);
        const bool readable = padState_ == PAD_STATE_STABLE || padState_ == PAD_STATE_FINDCTP1;
        state_.connected = padState_ != PAD_STATE_DISCONN;

        if (padState_ == PAD_STATE_DISCONN) {
            if (phase_ == Phase::Ready) {
                log.set(Subsystem::Controller, Health::Warning, "controller disconnected");
                loggedConnect_ = false;
            }
            phase_ = Phase::WaitStable;
        } else if (phase_ == Phase::WaitStable && padState_ == PAD_STATE_STABLE) {
            // Lock DualShock (analog) mode when the controller supports it,
            // so the sticks work without pressing ANALOG. Digital pads stay
            // fully usable: nothing requires the sticks.
            const int modes = padInfoMode(0, 0, PAD_MODETABLE, -1);
            bool dualshock = false;
            for (int i = 0; i < modes; ++i)
                if (padInfoMode(0, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK)
                    dualshock = true;
            if (dualshock)
                padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK);
            phase_ = Phase::Configuring;
        } else if (phase_ == Phase::Configuring && padState_ == PAD_STATE_STABLE) {
            phase_ = Phase::Ready;
        }

        if (readable && phase_ != Phase::WaitStable) {
            padButtonStatus data;
            if (padRead(0, 0, &data) != 0) {
                const uint16_t pressedWire = (uint16_t)(0xffff ^ data.btns); // libpad is active-low
                for (const Map& m : kMap)
                    if (pressedWire & m.pad)
                        now |= m.ours;
                // High nibble 7 = DualShock analog mode; sticks are valid.
                state_.analog = (data.mode >> 4) == 0x7;
                if (state_.analog) {
                    state_.lx = axis(data.ljoy_h, kDeadzone);
                    state_.ly = axis(data.ljoy_v, kDeadzone);
                    state_.rx = axis(data.rjoy_h, kDeadzone);
                    state_.ry = axis(data.rjoy_v, kDeadzone);
                }
                if (phase_ == Phase::Ready && !loggedConnect_) {
                    log.set(Subsystem::Controller, Health::Ok, "%s", state_.analog ? "DualShock 2, analog locked" : "digital controller");
                    loggedConnect_ = true;
                }
            }
        }
    }

    state_.pressed = now & ~state_.held;
    state_.released = state_.held & ~now;
    state_.held = now;
    state_.repeat = state_.pressed;
    for (int i = 0; i < 16; ++i) {
        const uint32_t bit = 1u << i;
        if (!(now & bit)) {
            state_.heldMs[i] = 0;
            continue;
        }
        if (state_.pressed & bit) {
            state_.heldMs[i] = 0;
            lastRepeatMs_[i] = 0;
            continue;
        }
        state_.heldMs[i] += dt;
        if (state_.heldMs[i] >= kRepeatDelayMs && state_.heldMs[i] - lastRepeatMs_[i] >= kRepeatRateMs) {
            // First repeat fires at the delay; later ones at the rate.
            if (lastRepeatMs_[i] == 0)
                lastRepeatMs_[i] = kRepeatDelayMs;
            else
                lastRepeatMs_[i] += kRepeatRateMs;
            state_.repeat |= bit;
        }
    }
}
