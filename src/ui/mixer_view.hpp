// Mixer: eight channel strips plus master. Meters are the real decaying peak
// levels measured inside the software mixer; SPU2-voiced channels bypass the
// software bus, so their strips say so instead of showing a fake meter.
#pragma once

#include "ui/view.hpp"

class MixerView : public View {
public:
    const char* tabName() const override { return "MIXER"; }
    const char* hint() const override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    int strip_ = 0; // 0..7 channels, 8 = master
};
