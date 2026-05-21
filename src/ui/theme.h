#pragma once
// IDHMFIS — Design token system for the "Obsidian Laser" dark theme.
// All colours are expressed as ImVec4 (linear sRGB, 0–1).
// Call apply_dark() once after ImGui context creation.

#include "imgui.h"

namespace idhmfis::theme {

// ─────────────────────────────────────────────────────────────────────────────
//  D-018 "Obsidian Laser" design tokens (constexpr palette)
// ─────────────────────────────────────────────────────────────────────────────

// --- Background ramp (kBg0 = darkest / deepest, kBg4 = lightest surface) ----
constexpr ImVec4 kBg0       = {0.051f, 0.059f, 0.071f, 1.f};  // #0D0F12 deepest bg
constexpr ImVec4 kBg1       = {0.082f, 0.094f, 0.114f, 1.f};  // #151820 panel bg
constexpr ImVec4 kBg2       = {0.114f, 0.133f, 0.161f, 1.f};  // #1D2229 widget bg
constexpr ImVec4 kBg3       = {0.161f, 0.184f, 0.220f, 1.f};  // #292F38 hover bg
constexpr ImVec4 kBg4       = {0.204f, 0.231f, 0.271f, 1.f};  // #343B45 lightest surface

// --- Accent — laser cyan (#00E5FF) with interaction states ------------------
constexpr ImVec4 kAccent       = {0.000f, 0.898f, 1.000f, 1.f};   // #00E5FF base cyan
constexpr ImVec4 kAccentHover  = {0.200f, 0.933f, 1.000f, 1.f};   // #33EEFF brightened hover
constexpr ImVec4 kAccentActive = {0.000f, 0.780f, 0.902f, 1.f};   // #00C7E6 saturated press
constexpr ImVec4 kAccentDim    = {0.000f, 0.898f, 1.000f, 0.18f}; // 18 % alpha — borders/outlines
constexpr ImVec4 kAccentHot    = {0.000f, 0.898f, 1.000f, 0.35f}; // 35 % alpha — legacy hover fill

// --- Status / semantic colours (de-saturated, broadcast-grade) --------------
constexpr ImVec4 kGreen     = {0.180f, 0.800f, 0.443f, 1.f};  // #2ECC71 active / go
constexpr ImVec4 kYellow    = {0.953f, 0.612f, 0.071f, 1.f};  // #F39C12 amber — next cue
constexpr ImVec4 kRed       = {0.906f, 0.298f, 0.235f, 1.f};  // #E74C3C record / stop
constexpr ImVec4 kWarm      = {0.878f, 0.467f, 0.161f, 1.f};  // #E07729 muted orange warning

// --- Text hierarchy ---------------------------------------------------------
constexpr ImVec4 kText          = {0.871f, 0.894f, 0.925f, 1.f};  // #DDE4EC (unchanged)
constexpr ImVec4 kTextDim       = {0.502f, 0.549f, 0.608f, 1.f};  // #808C9B (unchanged)
constexpr ImVec4 kTextPrimary   = {0.871f, 0.894f, 0.925f, 1.f};  // #DDE4EC alias for kText
constexpr ImVec4 kTextSecondary = {0.502f, 0.549f, 0.608f, 1.f};  // #808C9B alias for kTextDim
constexpr ImVec4 kTextDisabled  = {0.263f, 0.290f, 0.337f, 1.f};  // #434A56 inactive labels

// --- Borders / separators ---------------------------------------------------
constexpr ImVec4 kBorder       = {0.216f, 0.247f, 0.294f, 1.f};  // #373F4B (unchanged)
constexpr ImVec4 kBorderSubtle = {1.000f, 1.000f, 1.000f, 0.10f}; // ~10 % white — faint dividers
constexpr ImVec4 kBorderStrong = {1.000f, 1.000f, 1.000f, 0.25f}; // ~25 % white — prominent edges

// --- Surface variants -------------------------------------------------------
constexpr ImVec4 kHeaderBg  = {0.122f, 0.141f, 0.169f, 1.f};  // #1F2430 group box / header row

// --- Semantic aliases -------------------------------------------------------
constexpr ImVec4 kColorActive  = kGreen;
constexpr ImVec4 kColorNext    = kYellow;
constexpr ImVec4 kColorRecord  = kRed;
constexpr ImVec4 kColorWarning = kWarm;

// ─────────────────────────────────────────────────────────────────────────────
//  Apply full themes
// ─────────────────────────────────────────────────────────────────────────────
// apply() is the canonical entry point used after ImGui::CreateContext()
void apply();
void apply_dark();
void apply_light();
void apply_high_contrast();

// ─────────────────────────────────────────────────────────────────────────────
//  Push/pop helpers for semantic button styles
// ─────────────────────────────────────────────────────────────────────────────
void push_accent_button();
void pop_accent_button();
void push_danger_button();
void pop_danger_button();
void push_record_button(bool active);
void pop_record_button();

// ─────────────────────────────────────────────────────────────────────────────
//  Individual colour tokens
// ─────────────────────────────────────────────────────────────────────────────

// Backgrounds
ImVec4 background();        // #0D0F12
ImVec4 surface();           // #161921
ImVec4 surface2();          // #1E2230
ImVec4 border_color();      // #2A3044

// Accents
ImVec4 accent();            // #00E5FF  laser cyan
ImVec4 accent_warm();       // #FF6B35  hot orange
ImVec4 accent_hot();        // #FF2D55  danger / red

// Text
ImVec4 text_primary();      // #E8EAF0
ImVec4 text_secondary();    // #7B8499
ImVec4 text_disabled();     // #3D4459

// Semantic
ImVec4 success();           // #00E676
ImVec4 warning();           // #FFB300

// Convenience: accent with custom alpha
ImVec4 accent_alpha(float a);
ImVec4 accent_warm_alpha(float a);

// ─────────────────────────────────────────────────────────────────────────────
//  Dimension tokens
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float kRounding        = 4.f;
static constexpr float kSmallRounding   = 2.f;
static constexpr float kLargeRounding   = 8.f;
static constexpr float kBorderSize      = 1.f;
static constexpr float kScrollbarSize   = 8.f;
static constexpr float kGrabMinSize     = 6.f;
static constexpr float kWindowPadX      = 8.f;
static constexpr float kWindowPadY      = 6.f;
static constexpr float kItemSpacingX    = 6.f;
static constexpr float kItemSpacingY    = 4.f;
static constexpr float kTransportHeight = 48.f;

} // namespace idhmfis::theme
