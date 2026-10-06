// Minimal GS renderer on gsKit, sized for SD televisions.
//
// Logical canvas: 640x448 (NTSC). On PAL the 640x512 framebuffer is used and
// the canvas is centred vertically, so UI code never needs to know.
// Everything is queued into gsKit's one-shot DMA queue and sent to the GS in
// one transfer per frame. The only texture is the 32 KiB font atlas,
// uploaded once at init; no per-frame texture uploads.
#pragma once

#include <stdint.h>

#include "core/status_log.hpp"

class Gfx {
public:
    static constexpr int kWidth = 640;
    static constexpr int kHeight = 448;

    bool init(StatusLog& log);

    void beginFrame(uint32_t clearRgb);
    void endFrame(); // executes the queue and waits for vsync + flip

    // Colours are 0xRRGGBB; alpha 0x80 = opaque (GS convention), lower = blended.
    void fillRect(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha = 0x80);
    // Outline with 2-px horizontal edges so it does not flicker when interlaced.
    void frameRect(int x, int y, int w, int h, uint32_t rgb, int thickness = 2);

    // Text with integer scale; returns the advance width in pixels.
    int text(int x, int y, const char* s, uint32_t rgb, int scaleX = 2, int scaleY = 2);
    int textf(int x, int y, uint32_t rgb, const char* fmt, ...) __attribute__((format(printf, 5, 6)));
    static int textWidth(const char* s, int scaleX = 2);
    static int lineHeight(int scaleY = 2) { return 8 * scaleY; }

    bool isPal() const { return pal_; }
    uint32_t frameCount() const { return frames_; }

private:
    void* gs_ = nullptr; // GSGLOBAL*
    void* font_ = nullptr; // GSTEXTURE*
    int originY_ = 0;
    bool pal_ = false;
    uint32_t frames_ = 0;
};
