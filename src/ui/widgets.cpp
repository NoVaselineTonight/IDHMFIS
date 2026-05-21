// IDHMFIS — Custom widget implementations

#include "widgets.h"
#include "ui_state.h"
#include "theme.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <string>

namespace idhmfis::widgets {

// ─────────────────────────────────────────────────────────────────────────────
//  Helper: hex colour → ImU32
// ─────────────────────────────────────────────────────────────────────────────
static ImU32 col32(unsigned int hex, float a = 1.f) {
    ImVec4 v(
        ((hex >> 16) & 0xFF) / 255.f,
        ((hex >>  8) & 0xFF) / 255.f,
        ( hex        & 0xFF) / 255.f,
        a);
    return ImGui::ColorConvertFloat4ToU32(v);
}

// ─────────────────────────────────────────────────────────────────────────────
//  FaderStack
// ─────────────────────────────────────────────────────────────────────────────
bool FaderStack(const char* id,
                uint8_t*    channels,
                int         n,
                float       fader_width,
                float       fader_height,
                bool        read_only)
{
    bool changed = false;

    ImGui::PushID(id);
    ImGui::BeginGroup();

    const float channel_spacing = 2.f;
    const float label_height    = 14.f;
    const float value_height    = 14.f;
    const float total_height    = fader_height + label_height + value_height;

    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2      origin = ImGui::GetCursorScreenPos();

    for (int i = 0; i < n; ++i) {
        float x = origin.x + i * (fader_width + channel_spacing);
        float y = origin.y;

        // Channel number label
        char ch_label[8];
        std::snprintf(ch_label, sizeof(ch_label), "%d", i + 1);
        dl->AddText(ImVec2(x + fader_width * 0.5f - 6.f, y), col32(0x7B8499), ch_label);
        y += label_height;

        // Fader track background
        dl->AddRectFilled(ImVec2(x + fader_width * 0.3f, y),
                          ImVec2(x + fader_width * 0.7f, y + fader_height),
                          col32(0x131620), 2.f);

        // Fader fill
        float norm  = channels[i] / 255.f;
        float fill_y = y + fader_height * (1.f - norm);
        ImVec4 fill_col = theme::accent();
        if (channels[i] > 200)  fill_col = theme::accent_warm();
        if (channels[i] == 255) fill_col = theme::accent_hot();
        dl->AddRectFilled(ImVec2(x + fader_width * 0.3f, fill_y),
                          ImVec2(x + fader_width * 0.7f, y + fader_height),
                          ImGui::ColorConvertFloat4ToU32(fill_col), 2.f);

        // Grab handle
        float grab_y = fill_y - 4.f;
        dl->AddRectFilled(ImVec2(x + 1.f, grab_y),
                          ImVec2(x + fader_width - 1.f, grab_y + 8.f),
                          col32(0xE8EAF0), 2.f);

        // Interaction
        if (!read_only) {
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::InvisibleButton(ch_label, ImVec2(fader_width, fader_height));
            if (ImGui::IsItemActive()) {
                float drag_delta = -ImGui::GetIO().MouseDelta.y;
                float delta_norm = drag_delta / fader_height;
                float new_val    = channels[i] / 255.f + delta_norm;
                new_val          = std::clamp(new_val, 0.f, 1.f);
                channels[i]      = static_cast<uint8_t>(new_val * 255.f);
                changed          = true;
            }
        }

        // Value label
        char val_label[8];
        std::snprintf(val_label, sizeof(val_label), "%d", channels[i]);
        dl->AddText(ImVec2(x + fader_width * 0.5f - 6.f,
                           y + fader_height + 2.f),
                    col32(0x7B8499), val_label);
    }

    // Advance cursor past the drawn content
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + total_height));
    ImGui::Dummy(ImVec2(n * (fader_width + channel_spacing), 1.f));

    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  XYPad
// ─────────────────────────────────────────────────────────────────────────────
bool XYPad(const char* id, float* x, float* y, ImVec2 size) {
    bool changed = false;

    ImGui::PushID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, size);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = ImGui::ColorConvertFloat4ToU32(theme::surface2());
    ImU32 border = ImGui::ColorConvertFloat4ToU32(theme::border_color());
    ImU32 dot = ImGui::ColorConvertFloat4ToU32(theme::accent());
    ImU32 grid = col32(0x2A3044, 0.4f);

    // Background
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, theme::kRounding);
    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), border, theme::kRounding);

    // Grid lines
    float cx = pos.x + size.x * 0.5f;
    float cy = pos.y + size.y * 0.5f;
    dl->AddLine(ImVec2(cx, pos.y + 2.f), ImVec2(cx, pos.y + size.y - 2.f), grid);
    dl->AddLine(ImVec2(pos.x + 2.f, cy), ImVec2(pos.x + size.x - 2.f, cy), grid);

    // Handle
    float hx = cx + *x * (size.x * 0.5f - 8.f);
    float hy = cy - *y * (size.y * 0.5f - 8.f);
    dl->AddCircleFilled(ImVec2(hx, hy), 6.f, dot);
    dl->AddCircle(ImVec2(hx, hy), 8.f, col32(0x00E5FF, 0.5f));

    // Interaction
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        *x = std::clamp((mouse.x - cx) / (size.x * 0.5f - 8.f), -1.f, 1.f);
        *y = std::clamp(-(mouse.y - cy) / (size.y * 0.5f - 8.f), -1.f, 1.f);
        changed = true;
    }

    ImGui::PopID();
    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ColorWheel — outer hue ring, inner SV square, plus brightness bar
// ─────────────────────────────────────────────────────────────────────────────
bool ColorWheel(const char* id, float colour[4], float radius) {
    bool changed = false;
    ImGui::PushID(id);

    ImVec2 pos  = ImGui::GetCursorScreenPos();
    float  diam = radius * 2.f;
    ImVec2 size { diam + 30.f, diam };  // extra 30px for brightness bar

    ImGui::InvisibleButton(id, size);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float cx = pos.x + radius;
    float cy = pos.y + radius;

    // Convert current colour to HSV
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(colour[0], colour[1], colour[2], h, s, v);

    // --- Hue ring (outer) -----------------------------------------------------
    const int ring_segs = 64;
    float inner_r = radius * 0.72f;
    for (int i = 0; i < ring_segs; ++i) {
        float a0 = (float)i      / ring_segs * 2.f * IM_PI;
        float a1 = (float)(i+1)  / ring_segs * 2.f * IM_PI;
        float hue_here = (float)i / ring_segs;
        ImVec4 col_inner, col_outer;
        ImGui::ColorConvertHSVtoRGB(hue_here, 1.f, 1.f,
            col_inner.x, col_inner.y, col_inner.z);
        col_inner.w = 1.f; col_outer = col_inner;
        dl->AddQuadFilled(
            ImVec2(cx + inner_r * cosf(a0), cy + inner_r * sinf(a0)),
            ImVec2(cx + radius  * cosf(a0), cy + radius  * sinf(a0)),
            ImVec2(cx + radius  * cosf(a1), cy + radius  * sinf(a1)),
            ImVec2(cx + inner_r * cosf(a1), cy + inner_r * sinf(a1)),
            ImGui::ColorConvertFloat4ToU32(col_inner)
        );
    }

    // --- SV square (inner) ----------------------------------------------------
    float sq = inner_r * 0.707f;   // inscribed square half-side
    for (int yi = 0; yi < 16; ++yi) {
        for (int xi = 0; xi < 16; ++xi) {
            float sv_s  = (float)xi / 16.f;
            float sv_v  = 1.f - (float)yi / 16.f;
            float r2, g2, b2;
            ImGui::ColorConvertHSVtoRGB(h, sv_s, sv_v, r2, g2, b2);
            ImVec2 tl { cx - sq + xi     * sq * 2.f / 16.f,
                        cy - sq + yi     * sq * 2.f / 16.f };
            ImVec2 br { tl.x + sq * 2.f / 16.f,
                        tl.y + sq * 2.f / 16.f };
            dl->AddRectFilled(tl, br, ImGui::ColorConvertFloat4ToU32({r2,g2,b2,1.f}));
        }
    }

    // --- Hue handle -----------------------------------------------------------
    float ha = h * 2.f * IM_PI;
    float hr = (inner_r + radius) * 0.5f;
    dl->AddCircle(ImVec2(cx + hr*cosf(ha), cy + hr*sinf(ha)),
                  5.f, col32(0xFFFFFF), 12, 2.f);

    // --- SV crosshair ---------------------------------------------------------
    float dot_x = cx - sq + s * sq * 2.f;
    float dot_y = cy + sq - v * sq * 2.f;
    dl->AddCircle(ImVec2(dot_x, dot_y), 5.f, col32(0xFFFFFF), 12, 2.f);

    // --- Brightness bar (right side) ------------------------------------------
    float bar_x = pos.x + diam + 6.f;
    for (int bi = 0; bi < 16; ++bi) {
        float bv = 1.f - (float)bi / 16.f;
        float r2, g2, b2;
        ImGui::ColorConvertHSVtoRGB(h, s, bv, r2, g2, b2);
        ImVec2 tl { bar_x,          pos.y + bi * (diam / 16.f) };
        ImVec2 br { bar_x + 20.f,   tl.y + diam / 16.f };
        dl->AddRectFilled(tl, br, ImGui::ColorConvertFloat4ToU32({r2,g2,b2,1.f}));
    }
    // Brightness bar handle
    float bh_y = pos.y + (1.f - v) * diam;
    dl->AddRect(ImVec2(bar_x, bh_y - 3.f), ImVec2(bar_x + 20.f, bh_y + 3.f), col32(0xFFFFFF));

    // --- Interaction ----------------------------------------------------------
    if (ImGui::IsItemActive()) {
        ImVec2 mouse = ImGui::GetMousePos();
        // Brightness bar?
        if (mouse.x >= bar_x) {
            v = 1.f - std::clamp((mouse.y - pos.y) / diam, 0.f, 1.f);
            changed = true;
        } else {
            float dx = mouse.x - cx;
            float dy = mouse.y - cy;
            float dist = sqrtf(dx*dx + dy*dy);
            if (dist >= inner_r) {
                // Hue ring
                h = atan2f(dy, dx) / (2.f * IM_PI);
                if (h < 0.f) h += 1.f;
                changed = true;
            } else {
                // SV square
                s = std::clamp((mouse.x - (cx - sq)) / (sq * 2.f), 0.f, 1.f);
                v = std::clamp(1.f - (mouse.y - (cy - sq)) / (sq * 2.f), 0.f, 1.f);
                changed = true;
            }
        }
        if (changed) {
            ImGui::ColorConvertHSVtoRGB(h, s, v, colour[0], colour[1], colour[2]);
        }
    }

    ImGui::Dummy({ size.x, 4.f });
    ImGui::PopID();
    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TransportBar
// ─────────────────────────────────────────────────────────────────────────────
void TransportBar(bool*       playing,
                  bool*       paused,
                  float*      bpm,
                  float*      master_intensity,
                  bool        ndi_streaming,
                  bool        dac_connected,
                  float       engine_fps,
                  const char* project_name,
                  bool        project_dirty,
                  std::function<void()> on_play,
                  std::function<void()> on_pause,
                  std::function<void()> on_stop,
                  std::function<void()> on_save)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 bar_pos = ImGui::GetCursorScreenPos();
    float bar_w    = ImGui::GetContentRegionAvail().x;
    float bar_h    = theme::kTransportHeight;

    // Background
    dl->AddRectFilled(bar_pos,
                      ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h),
                      ImGui::ColorConvertFloat4ToU32(theme::background()));
    dl->AddLine(ImVec2(bar_pos.x, bar_pos.y + bar_h - 1),
                ImVec2(bar_pos.x + bar_w, bar_pos.y + bar_h - 1),
                ImGui::ColorConvertFloat4ToU32(theme::border_color()));

    ImGui::SetCursorScreenPos(ImVec2(bar_pos.x + 8.f, bar_pos.y + 8.f));
    ImGui::BeginGroup();

    // --- Play button ----------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Button,
        *playing && !*paused
            ? ImGui::ColorConvertFloat4ToU32(theme::success())
            : ImGui::ColorConvertFloat4ToU32(theme::surface2()));
    ImGui::PushStyleColor(ImGuiCol_Text,
        *playing && !*paused
            ? ImGui::ColorConvertFloat4ToU32(theme::background())
            : ImGui::ColorConvertFloat4ToU32(theme::text_primary()));
    if (ImGui::Button((*playing && !*paused) ? "##Playing" : "##Stopped",
                      ImVec2(32.f, 32.f))) {
        if (on_play) on_play();
    }
    // Draw play triangle
    {
        ImVec2 p = ImGui::GetItemRectMin();
        float cx = p.x + 16.f, cy = p.y + 16.f;
        dl->AddTriangleFilled(ImVec2(cx - 6, cy - 8), ImVec2(cx - 6, cy + 8),
                              ImVec2(cx + 8, cy),
                              col32(*playing && !*paused ? 0x0D0F12 : 0xE8EAF0));
    }
    ImGui::PopStyleColor(2);

    ImGui::SameLine(0, 4);

    // --- Pause button ---------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Button,
        *paused ? ImGui::ColorConvertFloat4ToU32(theme::warning())
                : ImGui::ColorConvertFloat4ToU32(theme::surface2()));
    if (ImGui::Button("##Pause", ImVec2(32.f, 32.f))) {
        if (on_pause) on_pause();
    }
    {
        ImVec2 p = ImGui::GetItemRectMin();
        float cx = p.x + 16.f, cy = p.y + 16.f;
        dl->AddRectFilled(ImVec2(cx - 7, cy - 8), ImVec2(cx - 2, cy + 8),
                          col32(*paused ? 0x0D0F12 : 0xE8EAF0));
        dl->AddRectFilled(ImVec2(cx + 2, cy - 8), ImVec2(cx + 7, cy + 8),
                          col32(*paused ? 0x0D0F12 : 0xE8EAF0));
    }
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 4);

    // --- Stop button ----------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImGui::ColorConvertFloat4ToU32(theme::surface2()));
    if (ImGui::Button("##Stop", ImVec2(32.f, 32.f))) {
        if (on_stop) on_stop();
    }
    {
        ImVec2 p = ImGui::GetItemRectMin();
        dl->AddRectFilled(ImVec2(p.x + 8, p.y + 8), ImVec2(p.x + 24, p.y + 24),
                          col32(0xE8EAF0));
    }
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 16);

    // --- BPM display ----------------------------------------------------------
    ImGui::PushItemWidth(70.f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::accent()));
    ImGui::DragFloat("##BPM", bpm, 0.5f, 20.f, 300.f, "%.1f BPM");
    ImGui::PopStyleColor();
    ImGui::PopItemWidth();

    ImGui::SameLine(0, 16);

    // --- Master intensity slider ----------------------------------------------
    ImGui::TextDisabled("INT");
    ImGui::SameLine(0, 4);
    ImGui::PushItemWidth(120.f);
    ImGui::SliderFloat("##Intensity", master_intensity, 0.f, 1.f, "%.0f%%",
                       ImGuiSliderFlags_AlwaysClamp);
    // Manually format as percentage
    char pct[8];
    std::snprintf(pct, sizeof(pct), "%.0f%%", *master_intensity * 100.f);
    ImGui::SameLine(-ImGui::CalcTextSize(pct).x - 4.f);
    ImGui::TextDisabled("%s", pct);
    ImGui::PopItemWidth();

    ImGui::SameLine(0, 16);

    // --- NDI status dot -------------------------------------------------------
    NDIStatusDot(ndi_streaming);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", ndi_streaming ? "NDI: Streaming" : "NDI: Idle");
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("NDI");
    ImGui::SameLine(0, 12);

    // --- ArtNet status --------------------------------------------------------
    {
        ImVec4 dot_col = dac_connected ? theme::success() : theme::text_disabled();
        ImDrawList* wdl = ImGui::GetWindowDrawList();
        ImVec2 dp = ImGui::GetCursorScreenPos();
        wdl->AddCircleFilled(ImVec2(dp.x + 5.f, dp.y + 8.f), 4.f,
                             ImGui::ColorConvertFloat4ToU32(dot_col));
        ImGui::Dummy({ 12.f, 0.f });
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", dac_connected ? "DAC: Connected" : "DAC: Disconnected");
    ImGui::SameLine(0, 4);
    ImGui::TextDisabled("DAC");
    ImGui::SameLine(0, 12);

    // --- FPS counter ----------------------------------------------------------
    char fps_str[16];
    std::snprintf(fps_str, sizeof(fps_str), "%.0f fps", engine_fps);
    ImGui::TextDisabled("%s", fps_str);
    ImGui::SameLine(0, 16);

    // --- Project name + dirty indicator ---------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Text,
        project_dirty
            ? ImGui::ColorConvertFloat4ToU32(theme::warning())
            : ImGui::ColorConvertFloat4ToU32(theme::text_primary()));
    char proj_disp[256];
    std::snprintf(proj_disp, sizeof(proj_disp), "%s%s",
                  project_dirty ? "* " : "",
                  project_name);
    ImGui::Text("%s", proj_disp);
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 8);

    // --- Save button ----------------------------------------------------------
    ImGui::PushStyleColor(ImGuiCol_Button,
        project_dirty
            ? ImGui::ColorConvertFloat4ToU32(theme::accent())
            : ImGui::ColorConvertFloat4ToU32(theme::surface2()));
    ImGui::PushStyleColor(ImGuiCol_Text,
        project_dirty
            ? ImGui::ColorConvertFloat4ToU32(theme::background())
            : ImGui::ColorConvertFloat4ToU32(theme::text_secondary()));
    if (ImGui::Button("Save", ImVec2(48.f, 26.f))) {
        if (on_save) on_save();
    }
    ImGui::PopStyleColor(2);

    ImGui::EndGroup();
}

// ─────────────────────────────────────────────────────────────────────────────
//  CueCard
// ─────────────────────────────────────────────────────────────────────────────
bool CueCard(const CueInfo& cue,
             bool selected,
             std::function<void(const char* action)> on_context_menu)
{
    bool clicked = false;
    const float cw = 120.f, ch = 80.f;

    ImGui::PushID(cue.index);
    ImVec2 pos = ImGui::GetCursorScreenPos();

    // Hit area
    ImGui::InvisibleButton("##card", ImVec2(cw, ch));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) clicked = true;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Card background
    ImU32 bg_col = selected
        ? ImGui::ColorConvertFloat4ToU32(theme::surface2())
        : ImGui::ColorConvertFloat4ToU32(theme::surface());
    dl->AddRectFilled(pos, ImVec2(pos.x + cw, pos.y + ch), bg_col, theme::kRounding);

    // Accent border
    ImU32 border_col = selected
        ? ImGui::ColorConvertFloat4ToU32(theme::accent())
        : (hovered ? ImGui::ColorConvertFloat4ToU32(theme::border_color())
                   : col32(0x1E2230));
    dl->AddRect(pos, ImVec2(pos.x + cw, pos.y + ch), border_col, theme::kRounding, 0, selected ? 2.f : 1.f);

    // Colour strip at top
    ImU32 strip_col = ImGui::ColorConvertFloat4ToU32(
        ImVec4(cue.card_color.r, cue.card_color.g, cue.card_color.b, 1.f));
    dl->AddRectFilled(pos, ImVec2(pos.x + cw, pos.y + 4.f), strip_col,
                      theme::kRounding,
                      ImDrawFlags_RoundCornersTop);

    // Generator icon — sine-wave dot pattern scaled to cue card dimensions
    {
        float my = pos.y + 12.f + (ch - 16.f) * 0.35f;
        ImU32 beam_col = ImGui::ColorConvertFloat4ToU32(
            ImVec4(cue.card_color.r, cue.card_color.g, cue.card_color.b, 0.6f));
        for (int j = 0; j < 5; ++j) {
            float t  = (float)j / 4.f;
            float x0 = pos.x + 8.f + t * (cw - 16.f);
            float y0 = my + sinf(t * 3.14159f * 2.f) * 10.f;
            dl->AddCircleFilled(ImVec2(x0, y0), 2.f, beam_col);
        }
    }

    // Name label
    dl->AddText(ImVec2(pos.x + 6.f, pos.y + ch - 20.f),
                col32(0xE8EAF0), cue.name.c_str());

    // Cue index badge
    char idx_str[8];
    std::snprintf(idx_str, sizeof(idx_str), "#%d", cue.index + 1);
    dl->AddText(ImVec2(pos.x + 6.f, pos.y + 6.f),
                col32(0x3D4459), idx_str);

    // Duration badge (top-right)
    if (cue.duration_s > 0.f) {
        char dur[16];
        std::snprintf(dur, sizeof(dur), "%.1fs", cue.duration_s);
        float dw = ImGui::CalcTextSize(dur).x;
        dl->AddText(ImVec2(pos.x + cw - dw - 4.f, pos.y + 6.f),
                    col32(0x3D4459), dur);
    } else {
        dl->AddText(ImVec2(pos.x + cw - 18.f, pos.y + 6.f),
                    col32(0x3D4459), "∞");
    }

    // Selected indicator
    if (selected) {
        dl->AddCircleFilled(ImVec2(pos.x + cw - 8.f, pos.y + ch - 8.f),
                            4.f, ImGui::ColorConvertFloat4ToU32(theme::accent()));
    }

    // Context menu
    if (ImGui::BeginPopupContextItem("##cue_ctx")) {
        if (ImGui::MenuItem("Duplicate"))  { if (on_context_menu) on_context_menu("duplicate"); }
        if (ImGui::MenuItem("Rename"))     { if (on_context_menu) on_context_menu("rename"); }
        if (ImGui::MenuItem("Color..."))   { if (on_context_menu) on_context_menu("color"); }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::accent_hot()));
        if (ImGui::MenuItem("Delete"))     { if (on_context_menu) on_context_menu("delete"); }
        ImGui::PopStyleColor();
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return clicked;
}

// ─────────────────────────────────────────────────────────────────────────────
//  TimelineRuler
// ─────────────────────────────────────────────────────────────────────────────
double TimelineRuler(const char* id,
                     double playhead_s,
                     double total_s,
                     float  width,
                     float  height,
                     float  pixels_per_second,
                     float  bpm)
{
    double new_playhead = -1.0;
    ImGui::PushID(id);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                      ImGui::ColorConvertFloat4ToU32(theme::background()));

    float seconds_beat = 60.f / bpm;

    // Tick marks
    // Choose subdivision: quarter note, bar (4 beats), etc.
    float beat_px = seconds_beat * pixels_per_second;

    // Draw bars and beats
    double start_s = 0.0;
    float  x_end   = pos.x + width;

    for (double t = start_s; t <= total_s; t += seconds_beat) {
        float x = pos.x + (float)(t * pixels_per_second);
        if (x > x_end) break;

        bool is_bar = std::fmod(t, (double)(seconds_beat * 4.f)) < 0.001;
        float tick_h = is_bar ? height * 0.7f : height * 0.35f;
        ImU32 tick_col = is_bar ? col32(0x4A5577) : col32(0x2A3044);
        dl->AddLine(ImVec2(x, pos.y + height - tick_h),
                    ImVec2(x, pos.y + height),
                    tick_col);

        if (is_bar && beat_px > 30.f) {
            char bar_label[8];
            int bar_num = (int)(t / (seconds_beat * 4.f)) + 1;
            std::snprintf(bar_label, sizeof(bar_label), "%d", bar_num);
            dl->AddText(ImVec2(x + 2.f, pos.y + 2.f),
                        col32(0x7B8499), bar_label);
        }
    }

    // Border
    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                ImGui::ColorConvertFloat4ToU32(theme::border_color()));

    // Playhead
    float ph_x = pos.x + (float)(playhead_s * pixels_per_second);
    if (ph_x >= pos.x && ph_x <= pos.x + width) {
        dl->AddLine(ImVec2(ph_x, pos.y), ImVec2(ph_x, pos.y + height),
                    ImGui::ColorConvertFloat4ToU32(theme::accent()), 2.f);
        // Arrow head
        dl->AddTriangleFilled(
            ImVec2(ph_x - 5.f, pos.y),
            ImVec2(ph_x + 5.f, pos.y),
            ImVec2(ph_x, pos.y + 8.f),
            ImGui::ColorConvertFloat4ToU32(theme::accent()));
    }

    // Click to seek
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        float mx = ImGui::GetMousePos().x;
        double t = (mx - pos.x) / pixels_per_second;
        new_playhead = std::clamp(t, 0.0, total_s);
    }

    ImGui::PopID();
    return new_playhead;
}

// ─────────────────────────────────────────────────────────────────────────────
//  KeyframeTrack
// ─────────────────────────────────────────────────────────────────────────────
bool KeyframeTrack(const char*           id,
                   std::vector<Keyframe>& keyframes,
                   double                playhead_s,
                   float                 pixels_per_second,
                   float                 width,
                   float                 height,
                   float                 value_min,
                   float                 value_max)
{
    bool changed = false;
    ImGui::PushID(id);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height),
                      ImGui::ColorConvertFloat4ToU32(theme::background()));

    // Draw connections between keyframes
    for (size_t i = 1; i < keyframes.size(); ++i) {
        auto& kf0 = keyframes[i-1];
        auto& kf1 = keyframes[i];
        float x0 = pos.x + kf0.time_s * pixels_per_second;
        float x1 = pos.x + kf1.time_s * pixels_per_second;
        float y0 = pos.y + height * (1.f - (kf0.value - value_min) / (value_max - value_min));
        float y1 = pos.y + height * (1.f - (kf1.value - value_min) / (value_max - value_min));
        dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col32(0x2A3044), 1.5f);
    }

    // Draw keyframe dots
    static int dragging_idx = -1;
    for (int i = 0; i < (int)keyframes.size(); ++i) {
        auto& kf  = keyframes[i];
        float kx  = pos.x + kf.time_s * pixels_per_second;
        float ky  = pos.y + height * (1.f - std::clamp((kf.value - value_min) /
                                      (value_max - value_min), 0.f, 1.f));

        bool hovered_dot = false;
        ImVec2 mp = ImGui::GetMousePos();
        float dx = mp.x - kx, dy = mp.y - ky;
        if (sqrtf(dx*dx + dy*dy) < 6.f) hovered_dot = true;

        ImU32 kf_col = hovered_dot || dragging_idx == i
            ? ImGui::ColorConvertFloat4ToU32(theme::accent_warm())
            : ImGui::ColorConvertFloat4ToU32(theme::accent());

        dl->AddCircleFilled(ImVec2(kx, ky), 5.f, kf_col);
        dl->AddCircle(ImVec2(kx, ky), 6.f, col32(0xFFFFFF, 0.3f));

        if (hovered_dot && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            dragging_idx = i;
        }
    }

    if (dragging_idx >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        ImVec2 delta = ImGui::GetIO().MouseDelta;
        auto& kf = keyframes[dragging_idx];
        kf.time_s += delta.x / pixels_per_second;
        kf.time_s  = std::max(kf.time_s, 0.f);
        float norm_delta = -delta.y / height;
        kf.value += norm_delta * (value_max - value_min);
        kf.value   = std::clamp(kf.value, value_min, value_max);
        changed = true;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        dragging_idx = -1;
    }

    // Playhead on track
    float ph_x = pos.x + (float)(playhead_s * pixels_per_second);
    if (ph_x >= pos.x && ph_x <= pos.x + width)
        dl->AddLine(ImVec2(ph_x, pos.y), ImVec2(ph_x, pos.y + height),
                    col32(0x00E5FF, 0.5f));

    dl->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                ImGui::ColorConvertFloat4ToU32(theme::border_color()));

    ImGui::PopID();
    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  NDIStatusDot
// ─────────────────────────────────────────────────────────────────────────────
void NDIStatusDot(bool streaming, float radius) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float cx = pos.x + radius + 1.f;
    float cy = pos.y + radius + 3.f;

    if (streaming) {
        // Pulse animation
        float t     = (float)ImGui::GetTime();
        float pulse = 0.5f + 0.5f * sinf(t * 4.f);

        // Outer glow ring
        dl->AddCircle(ImVec2(cx, cy), radius + 3.f,
                      col32(0x00E676, (unsigned char)(80 + (int)(100 * pulse))),
                      16, 1.5f);
        // Solid dot
        dl->AddCircleFilled(ImVec2(cx, cy), radius,
                            ImGui::ColorConvertFloat4ToU32(theme::success()));
    } else {
        dl->AddCircleFilled(ImVec2(cx, cy), radius,
                            ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
    }

    ImGui::InvisibleButton("##ndi_dot", ImVec2(radius * 2.f + 4.f, radius * 2.f + 6.f));
}

// ─────────────────────────────────────────────────────────────────────────────
//  BeamThicknessSlider
// ─────────────────────────────────────────────────────────────────────────────
bool BeamThicknessSlider(const char* label, float* value, float width) {
    // Map log-scale [0.1, 20] to [0, 1] for the slider
    static constexpr float log_min = -2.302585f; // ln(0.1)
    static constexpr float log_max =  2.995732f; // ln(20)

    float log_val = logf(std::clamp(*value, 0.1f, 20.f));
    float norm    = (log_val - log_min) / (log_max - log_min);

    ImGui::PushItemWidth(width);
    bool changed = ImGui::SliderFloat(label, &norm, 0.f, 1.f,
                                      "##beam", ImGuiSliderFlags_NoInput);
    ImGui::PopItemWidth();

    if (changed) {
        float new_log = log_min + norm * (log_max - log_min);
        *value = expf(new_log);
    }

    // Tooltip with actual value
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.2f px", *value);
    }

    // Inline display of actual value
    ImGui::SameLine();
    char disp[16];
    std::snprintf(disp, sizeof(disp), "%.2f px", *value);
    ImGui::TextDisabled("%s", disp);

    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  LatencyMeter
// ─────────────────────────────────────────────────────────────────────────────
void LatencyMeter(const char* id,
                  const float* history,
                  int          n,
                  int          idx,
                  float        p99_ms,
                  ImVec2       size)
{
    ImGui::PushID(id);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                      ImGui::ColorConvertFloat4ToU32(theme::background()),
                      theme::kSmallRounding);

    // Find max for scale
    float max_val = 0.f;
    for (int i = 0; i < n; ++i)
        max_val = std::max(max_val, history[i]);
    if (max_val < 1.f) max_val = 1.f;

    float bar_w = size.x / (float)n;
    for (int i = 0; i < n; ++i) {
        int real_i = (idx + i) % n;
        float v    = history[real_i];
        float h    = (v / max_val) * (size.y - 4.f);
        float bx   = pos.x + i * bar_w;
        float by   = pos.y + size.y - h - 2.f;

        // Colour: green < 5ms, yellow < 15ms, red >= 15ms
        ImU32 bar_col;
        if (v < 5.f)       bar_col = ImGui::ColorConvertFloat4ToU32(theme::success());
        else if (v < 15.f) bar_col = ImGui::ColorConvertFloat4ToU32(theme::warning());
        else               bar_col = ImGui::ColorConvertFloat4ToU32(theme::accent_hot());

        dl->AddRectFilled(ImVec2(bx + 1.f, by),
                          ImVec2(bx + bar_w - 1.f, pos.y + size.y - 2.f),
                          bar_col);
    }

    dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                ImGui::ColorConvertFloat4ToU32(theme::border_color()),
                theme::kSmallRounding);

    ImGui::InvisibleButton(id, size);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("ArtNet latency p99: %.1f ms", p99_ms);
    }

    ImGui::PopID();
}

} // namespace idhmfis::widgets
