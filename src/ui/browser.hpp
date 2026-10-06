// Sample browser. Milestone 1 lists the sample bank (the generated kit);
// USB folder browsing and WAV/ADP import land in Milestone 2 on top of the
// same list and the Storage/wav modules that already exist.
#pragma once

#include "ui/view.hpp"

class BrowserView : public View {
public:
    const char* tabName() const override { return "BROWSER"; }
    const char* hint() const override;
    void update(const InputState& in, UiContext& ctx) override;
    void draw(Gfx& g, UiContext& ctx) override;

private:
    int sel_ = 0;
    int first_ = 0;
};
