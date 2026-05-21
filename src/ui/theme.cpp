// IDHMFIS — Obsidian Laser theme implementation
// Palette targets a professional broadcast / laser programming tool:
// near-black backgrounds to let bright laser previews pop, cyan accent
// mirrors the default laser beam colour, warm orange for warnings/hot cues,
// and red for hard stop / danger actions.

#include "theme.h"
#include "imgui.h"
#include <cmath>

namespace idhmfis::theme {

// ─────────────────────────────────────────────────────────────────────────────
//  Hex → ImVec4 helper (compile-time capable)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr ImVec4 hex(unsigned int rgb, float a = 1.f) {
    return ImVec4(
        ((rgb >> 16) & 0xFF) / 255.f,
        ((rgb >>  8) & 0xFF) / 255.f,
        ( rgb        & 0xFF) / 255.f,
        a
    );
}

// ─────────────────────────────────────────────────────────────────────────────
//  Colour token definitions
// ─────────────────────────────────────────────────────────────────────────────
ImVec4 background()     { return hex(0x0D0F12); }
ImVec4 surface()        { return hex(0x161921); }
ImVec4 surface2()       { return hex(0x1E2230); }
ImVec4 border_color()   { return hex(0x2A3044); }

ImVec4 accent()         { return hex(0x00E5FF); }
ImVec4 accent_warm()    { return hex(0xFF6B35); }
ImVec4 accent_hot()     { return hex(0xFF2D55); }

ImVec4 text_primary()   { return hex(0xE8EAF0); }
ImVec4 text_secondary() { return hex(0x7B8499); }
ImVec4 text_disabled()  { return hex(0x6B7595); }

ImVec4 success()        { return hex(0x00E676); }
ImVec4 warning()        { return hex(0xFFB300); }

ImVec4 accent_alpha(float a)      { auto c = accent();      c.w = a; return c; }
ImVec4 accent_warm_alpha(float a) { auto c = accent_warm(); c.w = a; return c; }

// ─────────────────────────────────────────────────────────────────────────────
//  Dark theme — primary
// ─────────────────────────────────────────────────────────────────────────────
void apply_dark() {
    ImGuiStyle& s = ImGui::GetStyle();

    // --- Shape — tight Resolume Arena-like sizing ----------------------------
    s.WindowRounding      = 2.f;
    s.ChildRounding       = 2.f;
    s.FrameRounding       = 2.f;
    s.PopupRounding       = 2.f;
    s.ScrollbarRounding   = 2.f;
    s.GrabRounding        = 2.f;
    s.TabRounding         = 2.f;
    s.WindowBorderSize    = kBorderSize;
    s.FrameBorderSize     = 1.f;
    s.PopupBorderSize     = kBorderSize;
    s.TabBorderSize       = 0.f;
    s.ScrollbarSize       = 10.f;
    s.GrabMinSize         = 8.f;
    s.WindowPadding       = { 8.f, 6.f };
    s.FramePadding        = { 6.f, 4.f };
    s.ItemSpacing         = { 6.f, 5.f };
    s.ItemInnerSpacing    = { 4.f, 4.f };
    s.IndentSpacing       = 14.f;
    s.CellPadding         = { 4.f, 2.f };
    s.TabBarBorderSize    = 1.f;
    s.DockingSeparatorSize = 2.f;

    // --- Colours — Resolume Arena dark palette, Obsidian Laser accents -------
    ImVec4* c = s.Colors;

    auto acc  = accent();      // #00E5FF  laser cyan — preserved
    auto awm  = accent_warm(); // #FF6B35  hot orange — preserved
    auto aht  = accent_hot();  // #FF2D55  danger red — preserved
    auto tp   = text_primary();
    auto ts   = text_secondary();
    auto td   = text_disabled();
    auto suc  = success();

    // Windows — slightly warmer near-black
    c[ImGuiCol_WindowBg]            = ImVec4(0.08f, 0.09f, 0.10f, 1.0f);
    c[ImGuiCol_ChildBg]             = ImVec4(0.06f, 0.07f, 0.08f, 1.0f);
    c[ImGuiCol_PopupBg]             = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);

    // Borders — subtle but visible
    c[ImGuiCol_Border]              = ImVec4(0.20f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_BorderShadow]        = ImVec4(0.0f,  0.0f,  0.0f,  0.0f);

    // Frame (input, sliders, etc.)
    c[ImGuiCol_FrameBg]             = ImVec4(0.12f, 0.13f, 0.16f, 1.0f);
    c[ImGuiCol_FrameBgHovered]      = ImVec4(0.18f, 0.20f, 0.24f, 1.0f);
    c[ImGuiCol_FrameBgActive]       = ImVec4(0.22f, 0.25f, 0.30f, 1.0f);

    // Title
    c[ImGuiCol_TitleBg]             = ImVec4(0.06f, 0.07f, 0.08f, 1.0f);
    c[ImGuiCol_TitleBgActive]       = ImVec4(0.10f, 0.12f, 0.15f, 1.0f);
    c[ImGuiCol_TitleBgCollapsed]    = ImVec4(0.06f, 0.07f, 0.08f, 0.80f);

    // Menu bar
    c[ImGuiCol_MenuBarBg]           = ImVec4(0.06f, 0.07f, 0.08f, 1.0f);

    // Scrollbar — thin and unobtrusive
    c[ImGuiCol_ScrollbarBg]         = ImVec4(0.06f, 0.07f, 0.08f, 1.0f);
    c[ImGuiCol_ScrollbarGrab]       = ImVec4(0.22f, 0.24f, 0.28f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]= ImVec4(0.30f, 0.33f, 0.38f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = acc;

    // Check/radio — cyan accent
    c[ImGuiCol_CheckMark]           = ImVec4(0.00f, 0.85f, 1.00f, 1.0f);

    // Sliders — cyan accent
    c[ImGuiCol_SliderGrab]          = ImVec4(0.00f, 0.70f, 0.90f, 1.0f);
    c[ImGuiCol_SliderGrabActive]    = ImVec4(0.00f, 0.85f, 1.00f, 1.0f);

    // Buttons
    c[ImGuiCol_Button]              = ImVec4(0.20f, 0.22f, 0.28f, 1.0f);
    c[ImGuiCol_ButtonHovered]       = ImVec4(0.28f, 0.32f, 0.40f, 1.0f);
    c[ImGuiCol_ButtonActive]        = ImVec4(0.00f, 0.65f, 0.85f, 1.0f);

    // Header (collapsing / selectable)
    c[ImGuiCol_Header]              = ImVec4(0.18f, 0.21f, 0.26f, 1.0f);
    c[ImGuiCol_HeaderHovered]       = ImVec4(0.26f, 0.30f, 0.38f, 1.0f);
    c[ImGuiCol_HeaderActive]        = ImVec4(0.00f, 0.60f, 0.80f, 1.0f);

    // Separator
    c[ImGuiCol_Separator]           = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_SeparatorHovered]    = acc;
    c[ImGuiCol_SeparatorActive]     = acc;

    // Resize grip
    c[ImGuiCol_ResizeGrip]          = ImVec4(0.20f, 0.22f, 0.26f, 0.40f);
    c[ImGuiCol_ResizeGripHovered]   = acc;
    c[ImGuiCol_ResizeGripActive]    = ImVec4(0.00f, 0.85f, 1.00f, 1.0f);

    // Tabs
    c[ImGuiCol_Tab]                 = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
    c[ImGuiCol_TabHovered]          = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);
    c[ImGuiCol_TabSelected]         = ImVec4(0.14f, 0.16f, 0.20f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = acc;
    c[ImGuiCol_TabDimmed]           = ImVec4(0.08f, 0.09f, 0.11f, 1.0f);
    c[ImGuiCol_TabDimmedSelected]   = ImVec4(0.12f, 0.14f, 0.17f, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.20f, 0.22f, 0.26f, 1.0f);

    // Docking
    c[ImGuiCol_DockingPreview]      = ImVec4(0.00f, 0.898f, 1.0f, 0.25f);
    c[ImGuiCol_DockingEmptyBg]      = ImVec4(0.06f, 0.07f, 0.08f, 1.0f);

    // Plot
    c[ImGuiCol_PlotLines]           = ts;
    c[ImGuiCol_PlotLinesHovered]    = awm;
    c[ImGuiCol_PlotHistogram]       = acc;
    c[ImGuiCol_PlotHistogramHovered]= ImVec4(0.00f, 0.85f, 1.00f, 1.0f);

    // Tables
    c[ImGuiCol_TableHeaderBg]       = ImVec4(0.10f, 0.11f, 0.13f, 1.0f);
    c[ImGuiCol_TableBorderStrong]   = ImVec4(0.20f, 0.22f, 0.25f, 1.0f);
    c[ImGuiCol_TableBorderLight]    = ImVec4(0.14f, 0.16f, 0.18f, 1.0f);
    c[ImGuiCol_TableRowBg]          = ImVec4(0.0f,  0.0f,  0.0f,  0.0f);
    c[ImGuiCol_TableRowBgAlt]       = ImVec4(1.0f,  1.0f,  1.0f,  0.03f);

    // Text & misc
    c[ImGuiCol_Text]                = tp;
    c[ImGuiCol_TextDisabled]        = td;
    c[ImGuiCol_TextSelectedBg]      = ImVec4(0.00f, 0.898f, 1.0f, 0.25f);
    c[ImGuiCol_DragDropTarget]      = awm;
    c[ImGuiCol_NavHighlight]        = acc;
    c[ImGuiCol_NavWindowingHighlight]= ImVec4(1.0f, 1.0f, 1.0f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]   = ImVec4(0.8f, 0.8f, 0.8f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]    = ImVec4(0.0f, 0.0f, 0.0f, 0.60f);

    // Suppress unused-variable warnings
    (void)ts; (void)td; (void)suc; (void)aht;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Light theme — minimal override, keep accent palette
// ─────────────────────────────────────────────────────────────────────────────
void apply_light() {
    ImGui::StyleColorsLight();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding  = kRounding;
    s.FrameRounding   = kRounding;
    s.GrabRounding    = kRounding;
    s.ScrollbarSize   = kScrollbarSize;
    // Reapply accent colours
    ImVec4* c = s.Colors;
    c[ImGuiCol_SliderGrab]  = accent();
    c[ImGuiCol_CheckMark]   = accent();
    c[ImGuiCol_ButtonActive]= hex(0x00B8CC, 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  High-contrast theme — WCAG AA-compliant
// ─────────────────────────────────────────────────────────────────────────────
void apply_high_contrast() {
    apply_dark();
    ImVec4* c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]        = ImVec4(1.f, 1.f, 1.f, 1.f);
    c[ImGuiCol_TextDisabled]= ImVec4(0.6f, 0.6f, 0.6f, 1.f);
    c[ImGuiCol_WindowBg]    = ImVec4(0.f, 0.f, 0.f, 1.f);
    c[ImGuiCol_ChildBg]     = ImVec4(0.f, 0.f, 0.f, 1.f);
    c[ImGuiCol_Border]      = ImVec4(1.f, 1.f, 1.f, 0.5f);
    c[ImGuiCol_FrameBg]     = ImVec4(0.06f, 0.06f, 0.06f, 1.f);
    c[ImGuiCol_Button]      = ImVec4(0.1f, 0.1f, 0.1f, 1.f);
    c[ImGuiCol_SliderGrab]  = ImVec4(0.f, 1.f, 1.f, 1.f);
    c[ImGuiCol_CheckMark]   = ImVec4(0.f, 1.f, 1.f, 1.f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  apply() — canonical entry point, delegates to apply_dark()
// ─────────────────────────────────────────────────────────────────────────────
void apply() {
    apply_dark();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Push/pop helpers for semantic button styles
// ─────────────────────────────────────────────────────────────────────────────
void push_accent_button() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.898f, 1.f, 0.25f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.f, 0.898f, 1.f, 0.40f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.f, 0.898f, 1.f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.f, 0.898f, 1.f, 1.f));
}
void pop_accent_button()  { ImGui::PopStyleColor(4); }

void push_danger_button() {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.906f, 0.298f, 0.235f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.906f, 0.298f, 0.235f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.65f,  0.18f,  0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.f, 1.f, 1.f, 1.f));
}
void pop_danger_button()  { ImGui::PopStyleColor(4); }

void push_record_button(bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.78f, 0.08f, 0.08f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.55f, 0.05f, 0.05f, 1.f));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.07f, 0.07f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.10f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.05f, 0.05f, 1.f));
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
}
void pop_record_button()  { ImGui::PopStyleColor(4); }

} // namespace idhmfis::theme
