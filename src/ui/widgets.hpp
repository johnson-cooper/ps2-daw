// Small immediate-mode widget set drawn with Gfx primitives.
//
// Widgets are stateless draw helpers; interaction state (selection, edit
// mode, scroll position) lives in the views that own it. ContextMenu is the
// one retained widget because it is modal and shared by every view.
#pragma once

#include <stdint.h>

#include "platform/ps2_graphics.hpp"
#include "platform/ps2_input.hpp"

namespace ui {

void panel(Gfx& g, int x, int y, int w, int h, const char* title);
void label(Gfx& g, int x, int y, const char* text, uint32_t color, int scale = 2);
void button(Gfx& g, int x, int y, int w, int h, const char* text, bool active, bool selected, uint32_t activeColor);
void led(Gfx& g, int x, int y, int size, bool lit, uint32_t color);
void stepCell(Gfx& g, int x, int y, int w, int h, int velocity, bool altGroup, bool playhead, bool selected);
// value in [0, max]; bipolar sliders fill from the centre.
void hslider(Gfx& g, int x, int y, int w, int h, int value, int max, bool bipolar, bool selected, bool editing);
void vfader(Gfx& g, int x, int y, int w, int h, int value, int max, bool selected);
void vmeter(Gfx& g, int x, int y, int w, int h, uint16_t level);
void scrollbar(Gfx& g, int x, int y, int w, int h, int first, int visible, int total);
void listRow(Gfx& g, int x, int y, int w, int h, bool selected);
void tabBar(Gfx& g, int x, int y, const char* const* labels, int count, int active);
void selectOutline(Gfx& g, int x, int y, int w, int h, uint32_t color);

// Modal list dialog: title + up to kMaxItems rows.
class ContextMenu {
public:
    static constexpr int kMaxItems = 14;
    static constexpr int kNone = -1;

    void open(const char* title);
    void add(int id, const char* label, bool enabled = true);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }

    // Returns the chosen item id on CROSS, kNone otherwise. CIRCLE closes.
    int update(const InputState& in);
    void draw(Gfx& g) const;

private:
    struct Item {
        int id;
        char label[40];
        bool enabled;
    };
    char title_[40] = "";
    Item items_[kMaxItems];
    int count_ = 0;
    int sel_ = 0;
    bool open_ = false;
};

// Converts a meter level (0..32767) to an approximate dBFS for display.
int levelToDb(uint16_t level);

} // namespace ui
