// Built-in bitmap font: original 5x7 glyphs in 6x8 cells.
//
// The glyphs are expanded once at boot into a single 128x64 32-bit texture
// (32 KiB of GS VRAM) and drawn as scaled sprites; the default text scale is
// 2x (12x16 px cells), which stays readable on interlaced SD televisions.
#pragma once

#include <stdint.h>

namespace font {

constexpr int kFirstCode = 32;
constexpr int kGlyphCount = 95 + 14;
constexpr int kGlyphRows = 7;
constexpr int kGlyphCols = 5;
constexpr int kCellW = 6;
constexpr int kCellH = 8;
constexpr int kAtlasCols = 16;
constexpr int kAtlasW = 128;
constexpr int kAtlasH = 64;

// Symbol codes usable inside strings ("\x7f" etc.).
enum : uint8_t {
    Play = 127,
    Stop,
    Pause,
    Record,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    Note,
    Cross,
    Circle,
    Square,
    Triangle,
    Block,
};

extern const uint8_t kGlyphs[kGlyphCount][kGlyphRows];

} // namespace font

// String-literal forms for building text with adjacent-literal
// concatenation, e.g. G_CROSS " SELECT". (Using a separate literal avoids the
// C rule that a \x escape swallows any following hex digits.)
#define G_PLAY "\x7f"
#define G_STOP "\x80"
#define G_PAUSE "\x81"
#define G_RECORD "\x82"
#define G_UP "\x83"
#define G_DOWN "\x84"
#define G_LEFT "\x85"
#define G_RIGHT "\x86"
#define G_NOTE "\x87"
#define G_CROSS "\x88"
#define G_CIRCLE "\x89"
#define G_SQUARE "\x8a"
#define G_TRIANGLE "\x8b"
#define G_BLOCK "\x8c"

namespace font {

} // namespace font
