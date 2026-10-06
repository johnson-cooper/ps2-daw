// Project / settings: tempo, pattern, levels, audio latency, hardware
// self-tests, USB save/load, and the boot health table.
#pragma once

#include "ui/view.hpp"

class ProjectView : public View {
public:
    const char* tabName() const override { return "PROJECT"; }
    const char* hint() const override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum Row {
        RowTempo,
        RowPattern,
        RowLength,
        RowMaster,
        RowLatency,
        RowSlot,
        RowSave,
        RowLoad,
        RowNew,
        RowToneSw,
        RowToneSpu,
        RowOverlay,
        RowClearError,
        RowCount
    };
    void activate(int row, UiContext& ctx);
    void adjust(int row, int dir, bool fine, UiContext& ctx);
    void save(UiContext& ctx);
    void load(UiContext& ctx);

    int row_ = 0;
    int slot_ = 1;
    bool confirmNew_ = false;
};
