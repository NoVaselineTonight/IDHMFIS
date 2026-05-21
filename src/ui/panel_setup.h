#pragma once
#include "ui_state.h"
#include "layout.h"

namespace idhmfis {
// Renders the tab content without a surrounding window — call from any host.
void panel_setup_content(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Floating popup variant (File > Setup... menu item).
void panel_setup_window(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
// Safety view — scan-fail, emergency shutoff, BAM enable, blackout zones.
// Rendered inline as the full Safety view content (no surrounding tab bar).
void panel_safety_content(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs);
} // namespace idhmfis
