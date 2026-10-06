#include "platform/ps2_graphics.hpp"

#include <dmaKit.h>
#include <gsKit.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ui/font.hpp"

namespace {

GSGLOBAL* G(void* p) { return static_cast<GSGLOBAL*>(p); }

uint64_t rgba(uint32_t rgb, uint8_t a)
{
    return GS_SETREG_RGBAQ((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, a, 0x00);
}

GSTEXTURE g_fontTex;

} // namespace

bool Gfx::init(StatusLog& log)
{
    GSGLOBAL* gs = gsKit_init_global();
    if (!gs) {
        log.fail(Subsystem::Graphics, "gsKit_init_global returned NULL");
        return false;
    }
    gs_ = gs;

    // 2D UI: no Z buffer (saves ~1.1 MB of the GS's 4 MB), double buffered.
    gs->PSM = GS_PSM_CT24;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    pal_ = gs->Mode == GS_MODE_PAL;
    originY_ = (gs->Height - kHeight) / 2;
    if (originY_ < 0)
        originY_ = 0;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);

    // Expand the 1-bit glyphs into a 32-bit atlas. Ink texels are RGB 0x80
    // with alpha 0x80, background texels are fully transparent. The GS
    // MODULATE function computes Cv = Cf * Ct >> 7, so 0x80 (= 1.0) passes
    // the vertex colour through unchanged; 0xFF would double it.
    memset(&g_fontTex, 0, sizeof(g_fontTex));
    g_fontTex.Width = font::kAtlasW;
    g_fontTex.Height = font::kAtlasH;
    g_fontTex.PSM = GS_PSM_CT32;
    g_fontTex.Filter = GS_FILTER_NEAREST;
    const uint32_t texBytes = gsKit_texture_size_ee(font::kAtlasW, font::kAtlasH, GS_PSM_CT32);
    u32* pixels = (u32*)memalign(128, texBytes);
    if (!pixels) {
        log.fail(Subsystem::Graphics, "no memory for font atlas");
        return false;
    }
    memset(pixels, 0, texBytes);
    for (int g = 0; g < font::kGlyphCount; ++g) {
        const int cx = (g % font::kAtlasCols) * font::kCellW;
        const int cy = (g / font::kAtlasCols) * font::kCellH;
        for (int row = 0; row < font::kGlyphRows; ++row) {
            const uint8_t bits = font::kGlyphs[g][row];
            for (int col = 0; col < font::kGlyphCols; ++col)
                if (bits & (0x10 >> col))
                    pixels[(cy + row) * font::kAtlasW + cx + col] = 0x80808080u; // A,B,G,R = 0x80
        }
    }
    g_fontTex.Mem = pixels;
    g_fontTex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(font::kAtlasW, font::kAtlasH, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
    if (g_fontTex.Vram == GSKIT_ALLOC_ERROR) {
        log.fail(Subsystem::Graphics, "out of GS VRAM for font");
        return false;
    }
    gsKit_texture_upload(gs, &g_fontTex);
    free(pixels); // the GS keeps its own copy in VRAM
    g_fontTex.Mem = nullptr;
    font_ = &g_fontTex;

    log.set(Subsystem::Graphics, Health::Ok, "%s 640x%d interlaced, font in VRAM", pal_ ? "PAL" : "NTSC", gs->Height);
    return true;
}

void Gfx::beginFrame(uint32_t clearRgb)
{
    GSGLOBAL* gs = G(gs_);
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    gsKit_clear(gs, rgba(clearRgb, 0x80));
    // Alpha blend equation for translucent prims: Cs*As + Cd*(1-As).
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
}

void Gfx::endFrame()
{
    GSGLOBAL* gs = G(gs_);
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
    ++frames_;
}

void Gfx::fillRect(int x, int y, int w, int h, uint32_t rgb, uint8_t alpha)
{
    if (w <= 0 || h <= 0)
        return;
    GSGLOBAL* gs = G(gs_);
    // Blend only when needed: opaque fills skip the framebuffer read.
    gs->PrimAlphaEnable = alpha < 0x80 ? GS_SETTING_ON : GS_SETTING_OFF;
    const float y0 = (float)(y + originY_);
    gsKit_prim_sprite(gs, (float)x, y0, (float)(x + w), y0 + (float)h, 0, rgba(rgb, alpha));
}

void Gfx::frameRect(int x, int y, int w, int h, uint32_t rgb, int t)
{
    fillRect(x, y, w, t, rgb);
    fillRect(x, y + h - t, w, t, rgb);
    fillRect(x, y + t, t, h - 2 * t, rgb);
    fillRect(x + w - t, y + t, t, h - 2 * t, rgb);
}

int Gfx::textWidth(const char* s, int scaleX)
{
    int n = 0;
    while (s && *s++)
        ++n;
    return n * font::kCellW * scaleX;
}

int Gfx::text(int x, int y, const char* s, uint32_t rgb, int scaleX, int scaleY)
{
    if (!s || !font_)
        return 0;
    GSGLOBAL* gs = G(gs_);
    const GSTEXTURE* tex = static_cast<const GSTEXTURE*>(font_);
    gs->PrimAlphaEnable = GS_SETTING_ON; // glyph background texels have alpha 0
    const uint64_t color = rgba(rgb, 0x80);
    const float cw = (float)(font::kCellW * scaleX), ch = (float)(font::kCellH * scaleY);
    float px = (float)x;
    const float py = (float)(y + originY_);
    for (const unsigned char* c = (const unsigned char*)s; *c; ++c) {
        const int g = (int)*c - font::kFirstCode;
        if (g > 0 && g < font::kGlyphCount) { // g == 0 is space: skip the sprite
            const float u = (float)((g % font::kAtlasCols) * font::kCellW);
            const float v = (float)((g / font::kAtlasCols) * font::kCellH);
            gsKit_prim_sprite_texture(gs, tex, px, py, u, v, px + cw, py + ch, u + font::kCellW, v + font::kCellH, 0, color);
        }
        px += cw;
    }
    gs->PrimAlphaEnable = GS_SETTING_OFF;
    return (int)(px - (float)x);
}

int Gfx::textf(int x, int y, uint32_t rgb, const char* fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text(x, y, buf, rgb);
}
