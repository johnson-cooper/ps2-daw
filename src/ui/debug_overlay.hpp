// Toggleable performance/diagnostics overlay (R3, or PROJECT > Debug overlay).
#pragma once

#include "ui/view.hpp"

namespace ui {
void drawDebugOverlay(Gfx& g, const UiContext& ctx, uint32_t fpsTimes10, uint32_t frameUs);
}
