#pragma once
// IDHMFIS — Quick Show cue grid panel
// 10×6 buttons per page, 32 pages.

#include "ui_state.h"
#include "layout.h"

namespace idhmfis {

void panel_quickshow(UIState& state, LayoutCallbacks& cbs, bool* p_open);

} // namespace idhmfis
