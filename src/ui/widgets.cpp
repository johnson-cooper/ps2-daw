#include "ui/widgets.hpp"

#include <math.h>
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

// ---- ContextMenu -----------------------------------------------------------

void ContextMenu::open(const char* title)
{
    str::copy(title_, sizeof(title_), title);
    count_ = 0;
    sel_ = 0;
    open_ = true;
}

void ContextMenu::add(int id, const char* label, bool enabled)
{
    if (count_ >= kMaxItems)
        return;
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
    const int w = 360;
    const int h = 30 + count_ * rowH + 26;
    const int x = (Gfx::kWidth - w) / 2;
    const int y = (Gfx::kHeight - h) / 2;
    g.fillRect(0, 0, Gfx::kWidth, Gfx::kHeight, 0x000000, 0x50); // dim the screen behind
    g.fillRect(x - 2, y - 2, w + 4, h + 4, theme::kAccent);
    panel(g, x, y, w, h, title_);
    for (int i = 0; i < count_; ++i) {
        const int ry = y + 28 + i * rowH;
        listRow(g, x + 4, ry, w - 8, rowH - 2, i == sel_);
        g.text(x + 14, ry + 2, items_[i].label, items_[i].enabled ? theme::kText : theme::kTextDim);
    }
    const char* hint = G_CROSS " SELECT   " G_CIRCLE " BACK";
    g.text(x + 14, y + h - 22, hint, theme::kTextDim);
}

} // namespace ui
