// DualShock 2 input with edge detection and key repeat.
//
// The pad driver is polled once per UI frame. Port setup is a small state
// machine advanced every poll, so a missing or re-plugged controller never
// blocks the program (unlike the classic "wait until stable" loops).
#pragma once

#include <stdint.h>

#include "core/status_log.hpp"

namespace btn {
// Our own bit layout, independent of libpad's active-low wire format.
enum : uint32_t {
    Up = 1u << 0,
    Down = 1u << 1,
    Left = 1u << 2,
    Right = 1u << 3,
    Cross = 1u << 4,
    Circle = 1u << 5,
    Square = 1u << 6,
    Triangle = 1u << 7,
    L1 = 1u << 8,
    R1 = 1u << 9,
    L2 = 1u << 10,
    R2 = 1u << 11,
    Start = 1u << 12,
    Select = 1u << 13,
    L3 = 1u << 14,
    R3 = 1u << 15,
    Dpad = Up | Down | Left | Right,
};
} // namespace btn

struct InputState {
    uint32_t held = 0;      // currently down
    uint32_t pressed = 0;   // went down this frame
    uint32_t released = 0;  // went up this frame
    uint32_t repeat = 0;    // pressed, or auto-repeat tick while held
    uint32_t heldMs[16] = {};
    // Analog sticks, -127..127 after deadzone; 0 when digital-only.
    int lx = 0, ly = 0, rx = 0, ry = 0;
    bool connected = false;
    bool analog = false;

    bool down(uint32_t b) const { return (held & b) != 0; }
    bool hit(uint32_t b) const { return (pressed & b) != 0; }
    bool rep(uint32_t b) const { return (repeat & b) != 0; }
    bool heldFor(uint32_t b, uint32_t ms) const;
};

class Ps2Input {
public:
    // Loads sio2man + padman (embedded) and opens port 0.
    bool init(StatusLog& log);
    // Polls the pad; `nowMs` is a monotonic clock used for repeat timing.
    void update(uint32_t nowMs, StatusLog& log);
    const InputState& state() const { return state_; }
    const char* stateName() const;

    static constexpr uint32_t kRepeatDelayMs = 320;
    static constexpr uint32_t kRepeatRateMs = 70;
    static constexpr int kDeadzone = 40;

private:
    enum class Phase : uint8_t { Closed, WaitStable, Configuring, Ready };

    InputState state_;
    Phase phase_ = Phase::Closed;
    uint32_t lastRepeatMs_[16] = {};
    uint32_t lastMs_ = 0;
    int padState_ = -1;
    bool opened_ = false;
    bool loggedConnect_ = false;
};
