// Project / settings: tempo, pattern, levels, audio latency, hardware
// self-tests, USB save/load, and the boot health table.
#pragma once

#include "project/slot_store.hpp"
#include "ui/view.hpp"

class ProjectView : public View {
public:
    const char* tabName() const override { return "PROJECT"; }
    const char* hint() const override;
    void onEnter(UiContext& ctx) override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum Row {
        RowName,
        RowTempo,
        RowSwing,
        RowMetronome,
        RowPattern,
        RowLength,
        RowMaster,
        RowLatency,
        RowSlot,
        RowMissing,
        RowSave,
        RowLoad,
        RowNew,
        RowExport,
        RowRecover,
        RowToneSw,
        RowToneSpu,
        RowOverlay,
        RowClearError,
        RowCount
    };
    void activate(int row, UiContext& ctx);
    void adjust(int row, int dir, bool fine, UiContext& ctx);
    void save(UiContext& ctx);
    void load(UiContext& ctx, int slot);
    void exportWav(UiContext& ctx);

    bool modified(UiContext& ctx);
    void refreshSlotInfo(UiContext& ctx);
    void editName(const InputState& in, UiContext& ctx);

    int row_ = 0;
    int scroll_ = 0;
    int slot_ = 1;
    bool confirmNew_ = false;
    bool confirmLoad_ = false;
    bool nameEdit_ = false;
    int nameCursor_ = 0;
    uint32_t savedCrc_ = 0;      // fingerprint of the project as last saved/loaded
    bool haveSavedCrc_ = false;
    slotstore::Info slotInfo_ = {};
    slotstore::Info autoInfo_ = {};
    bool autoInfoValid_ = false;
    int slotInfoFor_ = 0;        // 0 = stale
};
