#pragma once
#include "ui_state.h"

namespace idhmfis {
    // Call each frame to render the floating 3D preview window.
    // p_open: optional bool* for window close button (nullptr = no close button shown)
    void panel_3d_preview(UIState& state, bool* p_open = nullptr);
}
