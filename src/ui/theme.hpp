// Visual design tokens. Original palette: charcoal panels with a warm orange
// for active steps and a green "alive" accent; selection is a bright cyan
// outline that reads clearly on composite video.
#pragma once

#include <stdint.h>

namespace theme {

constexpr uint32_t kBackground = 0x1a1c20;
constexpr uint32_t kPanel = 0x24272d;
constexpr uint32_t kPanelDark = 0x15171a;
constexpr uint32_t kPanelHeader = 0x30343c;
constexpr uint32_t kBorder = 0x3d424b;

constexpr uint32_t kCellOff = 0x3a3f48;     // step off, beat groups 1 and 3
constexpr uint32_t kCellOffAlt = 0x2d3138;  // step off, beat groups 2 and 4
constexpr uint32_t kCellOn = 0xf0902a;      // step on (warm orange)
constexpr uint32_t kCellOnDim = 0xa86420;   // step on, low velocity
constexpr uint32_t kPlayhead = 0xfff1c4;

constexpr uint32_t kAccent = 0xf0902a;
constexpr uint32_t kAlive = 0x8ed44a;       // LEDs, play state, unmuted
constexpr uint32_t kAliveDim = 0x34481f;
constexpr uint32_t kSelect = 0x6fdcff;      // selection outline
constexpr uint32_t kEdit = 0xffd84a;        // value being edited
constexpr uint32_t kMute = 0xd84a4a;
constexpr uint32_t kSolo = 0xe8c63a;
constexpr uint32_t kHardware = 0x9a7cf0;    // SPU2 voice marker

constexpr uint32_t kText = 0xe8e8ea;
constexpr uint32_t kTextDim = 0x8c939e;
constexpr uint32_t kTextDark = 0x15171a;
constexpr uint32_t kError = 0xff6060;
constexpr uint32_t kWarning = 0xffc04a;
constexpr uint32_t kOk = 0x8ed44a;

constexpr uint32_t kMeterLow = 0x58c048;
constexpr uint32_t kMeterMid = 0xe8c63a;
constexpr uint32_t kMeterHigh = 0xe04848;

// Overscan-safe working area for SD TVs (~5% horizontal, ~4% vertical).
constexpr int kSafeLeft = 24;
constexpr int kSafeRight = 616;
constexpr int kSafeTop = 14;
constexpr int kSafeBottom = 432;

} // namespace theme
