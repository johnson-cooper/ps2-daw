// Instrument settings for the selected rack channel: sampler or native
// synthesizer, AHDSR envelope, presets and the mixer track the channel plays
// into. Everything is a row of "name / bar / value" that the D-pad steps.
#pragma once

#include "ui/view.hpp"

class InstrumentView : public View {
public:
    const char* tabName() const override { return "INST"; }
    const char* hint() const override;
    void onEnter(UiContext& ctx) override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    enum RowType : uint8_t { RowKind, RowRoute, RowPreset, RowSynth, RowEnv };
    struct Row {
        RowType type;
        uint8_t index;
    };
    enum MenuId {
        MenuKind = 1,
        MenuSynthPreset,
        MenuEnvPreset,
        MenuReset,
        MenuRoute,
        MenuAuditionUp,
        MenuAuditionDown,
        MenuSynthPresetBase = 100,
        MenuEnvPresetBase = 200,
        MenuRouteBase = 300,
    };
    static constexpr int kVisibleRows = 11;
    int buildRows(const UiContext& ctx, Row* rows) const;
    void adjust(const Row& row, int direction, int multiplier, UiContext& ctx);
    void resetRow(const Row& row, UiContext& ctx);
    void valueText(const Row& row, const UiContext& ctx, char* out, size_t cap, int* frac) const;
    void openMenu(UiContext& ctx);
    void handleMenu(int id, UiContext& ctx);

    int sel_ = 0;
    int scroll_ = 0;
    int auditionPitch_ = 60;
    int8_t presetIdx_[cfg::kMaxChannels] = {-1, -1, -1, -1, -1, -1, -1, -1}; // last synth preset loaded per channel
};
