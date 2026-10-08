#include "ui/widgets.hpp"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/strutil.hpp"
#include "ui/font.hpp"
#include "ui/theme.hpp"

namespace ui {

void panel(Gfx& g, int x, int y, int w, int h, const char* title)
{
    g.fillRect(x, y, w, h, theme::kPanel);
    if (title) {
        g.fillRect(x, y, w, 22, theme::kPanelHeader);
        g.text(x + 8, y + 3, title, theme::kText);
    }
}

void label(Gfx& g, int x, int y, const char* text, uint32_t color, int scale)
{
    g.text(x, y, text, color, scale, 2);
}

void selectOutline(Gfx& g, int x, int y, int w, int h, uint32_t color)
{
    g.frameRect(x - 3, y - 3, w + 6, h + 6, color, 2);
}

void button(Gfx& g, int x, int y, int w, int h, const char* text, bool active, bool selected, uint32_t activeColor)
{
    g.fillRect(x, y, w, h, active ? activeColor : theme::kCellOffAlt);
    if (text) {
        const int tw = Gfx::textWidth(text);
        g.text(x + (w - tw) / 2, y + (h - 16) / 2, text, active ? theme::kTextDark : theme::kTextDim);
    }
    if (selected)
        selectOutline(g, x, y, w, h, theme::kSelect);
}

void led(Gfx& g, int x, int y, int size, bool lit, uint32_t color)
{
    g.fillRect(x, y, size, size, lit ? color : theme::kPanelDark);
}

void stepCell(Gfx& g, int x, int y, int w, int h, int velocity, bool altGroup, bool playhead, bool selected)
{
    uint32_t c;
    if (velocity > 0)
        c = velocity >= 80 ? theme::kCellOn : theme::kCellOnDim;
    else
        c = altGroup ? theme::kCellOffAlt : theme::kCellOff;
    g.fillRect(x, y, w, h, c);
    if (playhead) {
        // Bright bar along the bottom so the step colour stays readable.
        g.fillRect(x, y + h - 6, w, 6, theme::kPlayhead);
    }
    if (selected)
        selectOutline(g, x, y, w, h, theme::kSelect);
}

void hslider(Gfx& g, int x, int y, int w, int h, int value, int max, bool bipolar, bool selected, bool editing)
{
    g.fillRect(x, y, w, h, theme::kPanelDark);
    const uint32_t fill = editing ? theme::kEdit : theme::kAccent;
    if (max > 0) {
        if (bipolar) {
            const int mid = x + w / 2;
            const int px = x + (value + max) * w / (2 * max);
            if (px >= mid)
                g.fillRect(mid, y, px - mid + 1, h, fill);
            else
                g.fillRect(px, y, mid - px, h, fill);
            g.fillRect(mid, y, 1, h, theme::kTextDim);
        } else {
            g.fillRect(x, y, value * w / max, h, fill);
        }
    }
    if (selected)
        selectOutline(g, x, y, w, h, editing ? theme::kEdit : theme::kSelect);
}

void vfader(Gfx& g, int x, int y, int w, int h, int value, int max, bool selected)
{
    g.fillRect(x, y, w, h, theme::kPanelDark);
    const int fh = max > 0 ? value * h / max : 0;
    g.fillRect(x, y + h - fh, w, fh, theme::kAccent);
    if (selected)
        selectOutline(g, x, y, w, h, theme::kSelect);
}

int levelToDb(uint16_t level)
{
    if (level < 8)
        return -96;
    return (int)(20.0f * log10f((float)level / 32767.0f));
}

void vmeter(Gfx& g, int x, int y, int w, int h, uint16_t level)
{
    g.fillRect(x, y, w, h, theme::kPanelDark);
    // -48..0 dBFS mapped linearly in dB over the meter height.
    int db = levelToDb(level);
    if (db < -48)
        db = -48;
    const int fh = (db + 48) * h / 48;
    if (fh <= 0)
        return;
    // Three coloured zones: < -12 dB green, -12..-3 yellow, > -3 red.
    const int yGreen = (48 - 12) * h / 48, yYellow = (48 - 3) * h / 48;
    const int g1 = fh < yGreen ? fh : yGreen;
    g.fillRect(x, y + h - g1, w, g1, theme::kMeterLow);
    if (fh > yGreen) {
        const int y1 = (fh < yYellow ? fh : yYellow) - yGreen;
        g.fillRect(x, y + h - yGreen - y1, w, y1, theme::kMeterMid);
    }
    if (fh > yYellow)
        g.fillRect(x, y + h - fh, w, fh - yYellow, theme::kMeterHigh);
}

void scrollbar(Gfx& g, int x, int y, int w, int h, int first, int visible, int total)
{
    g.fillRect(x, y, w, h, theme::kPanelDark);
    if (total <= 0 || visible >= total) {
        g.fillRect(x, y, w, h, theme::kBorder);
        return;
    }
    const int th = h * visible / total;
    const int ty = y + h * first / total;
    g.fillRect(x, ty, w, th < 6 ? 6 : th, theme::kTextDim);
}

void listRow(Gfx& g, int x, int y, int w, int h, bool selected)
{
    g.fillRect(x, y, w, h, selected ? theme::kPanelHeader : theme::kPanel);
    if (selected)
        g.fillRect(x, y, 4, h, theme::kSelect);
}

void tabBar(Gfx& g, int x, int y, const char* const* labels, int count, int active)
{
    int cx = x;
    for (int i = 0; i < count; ++i) {
        const int w = Gfx::textWidth(labels[i]) + 20;
        const bool on = i == active;
        g.fillRect(cx, y, w, 20, on ? theme::kAccent : theme::kPanelHeader);
        g.text(cx + 10, y + 2, labels[i], on ? theme::kTextDark : theme::kTextDim);
        cx += w + 4;
    }
}

void paramRow(Gfx& g, int x, int y, int w, const char* name, const char* value, int frac, bool selected, bool editing)
{
    g.fillRect(x, y, w, 20, selected ? theme::kPanelHeader : theme::kPanel);
    if (selected)
        g.fillRect(x, y, 4, 20, editing ? theme::kEdit : theme::kSelect);
    g.text(x + 10, y + 2, name, selected ? theme::kText : theme::kTextDim, 1, 2);
    const int bx = x + w - 150, bw = 60;
    g.fillRect(bx, y + 7, bw, 6, theme::kPanelDark);
    if (frac > 0)
        g.fillRect(bx, y + 7, bw * (frac > 1000 ? 1000 : frac) / 1000, 6, editing ? theme::kEdit : theme::kAccent);
    g.text(bx + bw + 8, y + 2, value, selected ? theme::kText : theme::kTextDim, 1, 2);
}

void hmeter(Gfx& g, int x, int y, int w, int h, uint16_t level, bool clipped)
{
    g.fillRect(x, y, w, h, theme::kPanelDark);
    int db = levelToDb(level);
    if (db < -48)
        db = -48;
    const int fw = (db + 48) * (w - 6) / 48;
    if (fw > 0) {
        const int yellow = (48 - 12) * (w - 6) / 48, red = (48 - 3) * (w - 6) / 48;
        g.fillRect(x, y, fw < yellow ? fw : yellow, h, theme::kMeterLow);
        if (fw > yellow)
            g.fillRect(x + yellow, y, (fw < red ? fw : red) - yellow, h, theme::kMeterMid);
        if (fw > red)
            g.fillRect(x + red, y, fw - red, h, theme::kMeterHigh);
    }
    g.fillRect(x + w - 5, y, 5, h, clipped ? theme::kError : theme::kCellOffAlt);
}

void envelopeGraph(Gfx& g, int x, int y, int w, int h, const int16_t* env, bool enabled)
{
    g.fillRect(x, y, w, h, theme::kPanelDark);
    const float a = (float)env[1], hold = (float)env[2], d = (float)env[3], r = (float)env[5];
    const float sus = env[4] / 100.0f;
    // Time axis: attack + hold + decay, then a fixed sustain stretch, then the release.
    const float decayShown = d > 0 ? d : 1.0f;
    const float head = a + hold + decayShown;
    const float sustainShown = head * 0.4f + 40.0f;
    const float total = head + sustainShown + r;
    const uint32_t col = enabled ? theme::kAccent : theme::kTextDim;
    int prevY = y + h;
    for (int px = 0; px < w; ++px) {
        const float t = total * (float)px / (float)w;
        float level;
        if (t < a)
            level = a > 0 ? t / a : 1.0f;
        else if (t < a + hold)
            level = 1.0f;
        else if (t < head)
            level = d > 0 ? sus + (1.0f - sus) * powf(0.001f, (t - a - hold) / d) : sus;
        else if (t < head + sustainShown)
            level = sus;
        else
            level = r > 0 ? sus * powf(0.001f, (t - head - sustainShown) / r) : 0.0f;
        if (!enabled)
            level = t < head + sustainShown ? 1.0f : 0.0f;
        const int top = y + h - 2 - (int)(level * (float)(h - 6));
        // fill under the curve with a faint column, bright cap
        g.fillRect(x + px, top, 1, y + h - top, 0x3b2a14);
        const int lo = top < prevY ? top : prevY, hi = top < prevY ? prevY : top;
        g.fillRect(x + px, lo, 2, hi - lo + 2, col);
        prevY = top;
    }
    // Stage markers
    const int xa = x + (int)(a / total * (float)w), xh = x + (int)((a + hold) / total * (float)w), xd = x + (int)(head / total * (float)w),
              xs = x + (int)((head + sustainShown) / total * (float)w);
    const int marks[4] = {xa, xh, xd, xs};
    for (int mx : marks)
        g.fillRect(mx, y + h - 4, 1, 4, theme::kTextDim);
}

// ---- ContextMenu -----------------------------------------------------------

void ContextMenu::open(const char* title)
{
    str::copy(title_, sizeof(title_), title);
    count_ = 0;
    sel_ = 0;
    top_ = 0;
    open_ = true;
}

void ContextMenu::add(int id, const char* label, bool enabled)
{
    if (count_ >= kMaxItems)
        return;
    // Start on the first usable entry rather than a greyed-out one.
    if (enabled && count_ > 0 && !items_[sel_].enabled && sel_ == 0 && !items_[0].enabled)
        sel_ = count_;
    Item& it = items_[count_++];
    it.id = id;
    str::copy(it.label, sizeof(it.label), label);
    it.enabled = enabled;
}

int ContextMenu::update(const InputState& in)
{
    if (!open_ || count_ == 0)
        return kNone;
    if (in.rep(btn::Up))
        sel_ = (sel_ + count_ - 1) % count_;
    if (in.rep(btn::Down))
        sel_ = (sel_ + 1) % count_;
    if (in.hit(btn::Circle) || in.hit(btn::Triangle)) {
        open_ = false;
        return kNone;
    }
    if (in.hit(btn::Cross) && items_[sel_].enabled) {
        open_ = false;
        return items_[sel_].id;
    }
    return kNone;
}

void ContextMenu::draw(Gfx& g) const
{
    if (!open_)
        return;
    const int rowH = 22;
    const int w = 500;
    const int rows = count_ < kRows ? count_ : kRows;
    if (sel_ < top_)
        top_ = sel_;
    if (sel_ >= top_ + rows)
        top_ = sel_ - rows + 1;
    const int h = 30 + rows * rowH + 26;
    const int x = (Gfx::kWidth - w) / 2;
    const int y = (Gfx::kHeight - h) / 2;
    g.fillRect(0, 0, Gfx::kWidth, Gfx::kHeight, 0x000000, 0x50); // dim the screen behind
    g.fillRect(x - 2, y - 2, w + 4, h + 4, theme::kAccent);
    panel(g, x, y, w, h, title_);
    const int maxChars = (w - 36) / 12; // never draw past the box
    for (int r = 0; r < rows; ++r) {
        const int i = top_ + r;
        const int ry = y + 28 + r * rowH;
        listRow(g, x + 4, ry, w - 8, rowH - 2, i == sel_);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*s", maxChars, items_[i].label);
        g.text(x + 14, ry + 2, buf, items_[i].enabled ? theme::kText : theme::kTextDim);
    }
    if (count_ > rows) { // scroll hints on the right edge of the title bar
        char pos[32];
        snprintf(pos, sizeof(pos), "%d/%d", sel_ + 1, count_);
        g.text(x + w - 12 - Gfx::textWidth(pos), y + 3, pos, theme::kTextDim);
    }
    const char* hint = G_CROSS " SELECT   " G_CIRCLE " BACK";
    g.text(x + 14, y + h - 22, hint, theme::kTextDim);
}

} // namespace ui
