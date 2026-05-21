// IDHMFIS — Full panel layout implementation

#include "layout.h"
#include "panel_quickshow.h"
#include "panel_livepro.h"
#include "panel_setup.h"
#include "panel_3d_preview.h"
#include "theme.h"
#include "widgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "../fx/expression.h"
#include "../generators/igenerator.h"
#include "../render/cue_thumbnailer.h"
#include "../timeline/timeline_types.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <string>
#include <cstring>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Timing display helper — "Xs / YY.Y BPM" dual format
// ─────────────────────────────────────────────────────────────────────────────
static bool DragTimingFloat(const char* label, float* v, float speed,
                             float min_v, float max_v, float /*bpm*/ = 0.f)
{
    bool changed = ImGui::DragFloat(label, v, speed, min_v, max_v, "%.2f s", ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemHovered() && *v > 0.001f) {
        float equiv_bpm = 60.f / *v;
        ImGui::SetTooltip("%.2f s  (%.1f BPM)", *v, equiv_bpm);
    }
    return changed;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Symmetry helper — returns {original, copy1, copy2, ...} sets of points
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<std::vector<ImVec2>> apply_symmetry(
    const std::vector<ImVec2>& pts,
    FrameEditorState::SymmetryMode mode,
    float cx, float cy)
{
    using SM = FrameEditorState::SymmetryMode;

    std::vector<std::vector<ImVec2>> result;
    result.push_back(pts);  // first element is always the original

    if (mode == SM::None || pts.empty())
        return result;

    auto reflect_x = [&](ImVec2 p) -> ImVec2 {
        return { 2.f * cx - p.x, p.y };
    };
    auto reflect_y = [&](ImVec2 p) -> ImVec2 {
        return { p.x, 2.f * cy - p.y };
    };
    auto rotate_pt = [&](ImVec2 p, float angle) -> ImVec2 {
        float dx = p.x - cx;
        float dy = p.y - cy;
        float c  = cosf(angle);
        float s  = sinf(angle);
        return { cx + dx * c - dy * s, cy + dx * s + dy * c };
    };
    auto transform_set = [&](const std::vector<ImVec2>& src,
                              std::function<ImVec2(ImVec2)> fn)
            -> std::vector<ImVec2> {
        std::vector<ImVec2> out;
        out.reserve(src.size());
        for (const auto& p : src) out.push_back(fn(p));
        return out;
    };

    switch (mode) {
    case SM::MirrorX: {
        result.push_back(transform_set(pts, reflect_x));
        break;
    }
    case SM::MirrorY: {
        result.push_back(transform_set(pts, reflect_y));
        break;
    }
    case SM::MirrorXY: {
        result.push_back(transform_set(pts, reflect_x));
        result.push_back(transform_set(pts, reflect_y));
        // fourth quadrant: reflect both
        auto both = transform_set(pts, reflect_x);
        result.push_back(transform_set(both, reflect_y));
        break;
    }
    case SM::Radial2:
    case SM::Radial3:
    case SM::Radial4:
    case SM::Radial6:
    case SM::Radial8:
    case SM::Radial12: {
        int n = 2;
        if (mode == SM::Radial3)  n = 3;
        if (mode == SM::Radial4)  n = 4;
        if (mode == SM::Radial6)  n = 6;
        if (mode == SM::Radial8)  n = 8;
        if (mode == SM::Radial12) n = 12;
        float step = 6.28318530f / (float)n;
        for (int k = 1; k < n; ++k) {
            float angle = step * (float)k;
            result.push_back(transform_set(pts, [&](ImVec2 p){ return rotate_pt(p, angle); }));
        }
        break;
    }
    default:
        break;
    }
    return result;
}


// ─────────────────────────────────────────────────────────────────────────────
//  Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
static void draw_show_view_grid(UIState&, LayoutContext&, LayoutCallbacks&,
                                ImVec2, ImVec2, float, float);

// ─────────────────────────────────────────────────────────────────────────────
//  Dockspace IDs (stable)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr const char* kWinMain        = "IDHMFIS_Main";
static constexpr const char* kWinCueLib      = "Cue Library##w";
static constexpr const char* kWinPreview     = "Laser Preview##w";
static constexpr const char* kWinStreams     = "STREAMS##streams_win";
static constexpr const char* kWinInspector   = "Inspector##w";
static constexpr const char* kWinTimeline    = "Timeline##w";
static constexpr const char* kWinFrameEditor = "Frame Editor##w";
static constexpr const char* kWinMacros      = "Macros##w";
static constexpr const char* kWinZones       = "Zones##w";
static constexpr const char* kWinBam         = "Safety/BAM##w";
static constexpr const char* kWinQuickShow   = "Quick Show##w";
static constexpr const char* kWinLivePRO     = "LivePRO##w";
static constexpr const char* kWinMidiLearn   = "MIDI Learn##w";
static constexpr const char* kWinPalette     = "Palette##w";

// ─────────────────────────────────────────────────────────────────────────────
//  layout_draw — top-level
// ─────────────────────────────────────────────────────────────────────────────
void layout_draw(UIState&          state,
                 LayoutContext&    ctx,
                 LayoutCallbacks&  cbs,
                 CommandPalette&   palette,
                 ShortcutRegistry& shortcuts)
{
    // ── Emergency shutoff ─────────────────────────────────────────────────────
    // Two-layer approach:
    //  1. Fullscreen ImGui window (no transparency) — blocks ALL mouse/keyboard
    //     input from reaching any panel behind it, and accepts focus.
    //  2. GetForegroundDrawList() — draws the red overlay on top of EVERYTHING
    //     including the input-blocking window, guaranteed last.
    if (state.emergency_shutoff_active) {
        // Check for Ctrl+Alt+Enter release BEFORE drawing so it works this frame.
        bool ctrl  = ImGui::IsKeyDown(ImGuiKey_LeftCtrl)  || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
        bool alt   = ImGui::IsKeyDown(ImGuiKey_LeftAlt)   || ImGui::IsKeyDown(ImGuiKey_RightAlt);
        bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false);
        if (ctrl && alt && enter) {
            if (cbs.on_emergency_shutoff) cbs.on_emergency_shutoff(false);
            state.emergency_shutoff_active = false;
        }

        if (state.emergency_shutoff_active) {
            ImGuiIO& eio  = ImGui::GetIO();
            ImVec2   disp = eio.DisplaySize;

            // Layer 1: fullscreen opaque ImGui window that eats all input.
            ImGui::SetNextWindowPos({0.f, 0.f});
            ImGui::SetNextWindowSize(disp);
            ImGui::SetNextWindowBgAlpha(1.f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.549f, 0.f, 0.f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
            ImGui::Begin("##eso_blocker", nullptr,
                ImGuiWindowFlags_NoTitleBar       |
                ImGuiWindowFlags_NoResize         |
                ImGuiWindowFlags_NoMove           |
                ImGuiWindowFlags_NoScrollbar      |
                ImGuiWindowFlags_NoSavedSettings  |
                ImGuiWindowFlags_NoCollapse       |
                ImGuiWindowFlags_NoNav            |
                ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();

            // Layer 2: draw overlay text on the foreground draw list (above the window).
            ImFont* font    = ImGui::GetFont();
            float   base_sz = ImGui::GetFontSize();
            ImDrawList* fdl = ImGui::GetForegroundDrawList();

            float pulse     = 0.5f + 0.5f * sinf(static_cast<float>(ImGui::GetTime()) * 4.f);
            ImU32 title_col = IM_COL32(255,
                                       static_cast<int>(pulse * 60.f),
                                       static_cast<int>(pulse * 60.f), 255);

            const char* title    = "! EMERGENCY SHUTOFF ACTIVE !";
            const float title_sz = base_sz * 2.4f;
            ImVec2 t_sz = font->CalcTextSizeA(title_sz, FLT_MAX, 0.f, title);
            float  t_y  = (disp.y - t_sz.y) * 0.5f - base_sz * 3.5f;
            fdl->AddText(font, title_sz,
                         {(disp.x - t_sz.x) * 0.5f, t_y}, title_col, title);

            const char* sub1 = "All laser output is blocked.";
            ImVec2 s1_sz = font->CalcTextSizeA(base_sz, FLT_MAX, 0.f, sub1);
            fdl->AddText(font, base_sz,
                         {(disp.x - s1_sz.x) * 0.5f, t_y + t_sz.y + base_sz * 1.8f},
                         IM_COL32(220, 220, 220, 255), sub1);

            const char* sub2 = "Press  Ctrl + Alt + Enter  to release";
            ImVec2 s2_sz = font->CalcTextSizeA(base_sz * 1.2f, FLT_MAX, 0.f, sub2);
            fdl->AddText(font, base_sz * 1.2f,
                         {(disp.x - s2_sz.x) * 0.5f, t_y + t_sz.y + base_sz * 3.8f},
                         IM_COL32(255, 225, 0, 255), sub2);

            return;  // Skip all other panels this frame
        }
    }

    // Process keyboard shortcuts before rendering panels
    shortcuts.process();

    // Global Ctrl+Z / Ctrl+Y — project undo/redo (skipped when frame editor has focus)
    {
        ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && !io.WantTextInput && !ctx.fe_window_focused) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (io.KeyShift) { if (cbs.on_redo) cbs.on_redo(); }
                else             { if (cbs.on_undo) cbs.on_undo(); }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
                if (cbs.on_redo) cbs.on_redo();
            }
        }
    }

    ImGuiViewport* main_vp = ImGui::GetMainViewport();
    (void)ImGui::GetIO();  // io not used directly; sidebar/dockspace use viewport

    // =========================================================================
    // Row 1 — Main Menu Bar (~20px, managed entirely by ImGui)
    // =========================================================================
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Pending action for unsaved-changes confirmation
            static enum class PendingFileAction { None, New, Open } s_pending_action
                = PendingFileAction::None;

            // New Show
            if (ImGui::MenuItem("New Show", "Ctrl+N")) {
                if (state.project_dirty) {
                    s_pending_action = PendingFileAction::New;
                    ImGui::OpenPopup("Unsaved Changes##fc");
                } else {
                    if (cbs.on_new_project) cbs.on_new_project();
                }
            }
            // Open
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                if (state.project_dirty) {
                    s_pending_action = PendingFileAction::Open;
                    ImGui::OpenPopup("Unsaved Changes##fc");
                } else {
                    if (cbs.on_open_project) cbs.on_open_project();
                }
            }

            // Unsaved changes confirmation popup
            if (ImGui::BeginPopupModal("Unsaved Changes##fc", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("You have unsaved changes. Continue without saving?");
                ImGui::Spacing();
                if (ImGui::Button("Continue", ImVec2(100, 0))) {
                    if (s_pending_action == PendingFileAction::New  && cbs.on_new_project)
                        cbs.on_new_project();
                    if (s_pending_action == PendingFileAction::Open && cbs.on_open_project)
                        cbs.on_open_project();
                    s_pending_action = PendingFileAction::None;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                    s_pending_action = PendingFileAction::None;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Save",         "Ctrl+S")) if (cbs.on_save_project) cbs.on_save_project();
            if (ImGui::MenuItem("Save As...",   "Ctrl+Shift+S")) if (cbs.on_save_as) cbs.on_save_as();
            ImGui::Separator();
            if (ImGui::MenuItem("Import ILDA...")) if (cbs.on_import_ilda) cbs.on_import_ilda();
            if (ImGui::MenuItem("Export ILDA...")) if (cbs.on_export_ilda) cbs.on_export_ilda();
            ImGui::Separator();
            if (!state.recent_files.empty()) {
                if (ImGui::BeginMenu("Recent Files")) {
                    for (const auto& f : state.recent_files) {
                        // Show only filename; full path on hover
                        auto sep = f.find_last_of("\\/");
                        std::string display = (sep != std::string::npos) ? f.substr(sep + 1) : f;
                        if (ImGui::MenuItem(display.c_str())) {
                            if (cbs.on_open_recent) cbs.on_open_recent(f);
                        }
                        ImGui::SetItemTooltip("%s", f.c_str());
                    }
                    ImGui::EndMenu();
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Setup...")) { ctx.setup_window_open = true; }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Alt+F4")) { ctx.quit_requested = true; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo",         "Ctrl+Z"))     if (cbs.on_undo)  cbs.on_undo();
            if (ImGui::MenuItem("Redo",         "Ctrl+Shift+Z")) if (cbs.on_redo) cbs.on_redo();
            ImGui::Separator();
            if (ImGui::MenuItem("Select All",   "Ctrl+A")) {
                ctx.frame_editor.selected_ids.clear();
                for (const auto& obj : ctx.frame_editor.objects)
                    ctx.frame_editor.selected_ids.push_back(obj.id);
                ctx.frame_editor.active_tool = FrameEditorState::Tool::Select;
            }
            if (ImGui::MenuItem("Duplicate Cue","Ctrl+D"))     {}
            if (ImGui::MenuItem("Delete",       "Del"))        {}
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Fullscreen Preview","F11")) if (cbs.on_fullscreen) cbs.on_fullscreen();
            ImGui::Separator();
            if (ImGui::MenuItem("Dark Theme"))    theme::apply_dark();
            if (ImGui::MenuItem("Light Theme"))   theme::apply_light();
            if (ImGui::MenuItem("High Contrast")) theme::apply_high_contrast();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            // Floating / dockable panels — clicking re-opens if closed
            ImGui::MenuItem("Laser Preview##wm",   nullptr, &ctx.panel_preview_open);
            ImGui::MenuItem("3D Preview##wm",      nullptr, &ctx.panel_3d_open);
            ImGui::MenuItem("Frame Editor##wm",    nullptr, &ctx.frame_editor_open);
            ImGui::MenuItem("BPM##wm",             nullptr, &ctx.bpm_open);
            ImGui::MenuItem("Timelines##wm",       nullptr, &ctx.panel_timeline_open);
            ImGui::MenuItem("Cue Library##wm",     nullptr, &ctx.panel_cue_lib_open);
            ImGui::MenuItem("STREAMS##wm",         nullptr, &ctx.streams_window_open);
            ImGui::MenuItem("Quick Show##wm",      nullptr, &ctx.quickshow_open);
            ImGui::MenuItem("LivePRO##wm",         nullptr, &ctx.livepro_open);
            ImGui::Separator();
            if (ImGui::MenuItem("Setup",           nullptr, ctx.setup_window_open))
                ctx.setup_window_open = !ctx.setup_window_open;
            ImGui::Separator();
            ImGui::MenuItem("Clock##wm",             nullptr, &ctx.clock_open);
            ImGui::MenuItem("Snake##wm",             nullptr, &ctx.snake_open);
            ImGui::MenuItem("Connection Status##wm", nullptr, &ctx.conn_status_open);
            ImGui::Separator();
            if (ImGui::MenuItem("Reset Layout")) {
                ctx.dockspace_initialised    = false;
                ctx.show_layout_initialised  = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Documentation", "F1")) if (cbs.on_help) cbs.on_help();
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // =========================================================================
    // Row 2 — Transport Side Bar (44px, sits immediately below menu bar)
    // =========================================================================
    panel_transport(state, ctx, cbs);
    panel_operator_sidebar(state, ctx, cbs);

    // ── Global keyboard: playback GO keys + BPM tap key (all views) ──────────
    {
        ImGuiIO& kio = ImGui::GetIO();
        if (!kio.WantTextInput) {
            // Playback GO key bindings
            for (int ki = 0; ki < UIState::kMaxPlaybacks; ++ki) {
                auto& kconf = state.pb_conf[ki];
                if (kconf.keyboard_key != 0 &&
                    ImGui::IsKeyPressed(static_cast<ImGuiKey>(kconf.keyboard_key))) {
                    if (cbs.on_playback_go) cbs.on_playback_go(ki + 1);
                }
            }
            // BPM tap key
            if (state.bpm_tap_key != 0 &&
                ImGui::IsKeyPressed(static_cast<ImGuiKey>(state.bpm_tap_key))) {
                static double s_tap_last_k  = 0.0;
                static double s_tap_avg_k   = 0.0;
                static int    s_tap_count_k = 0;
                double now = ImGui::GetTime();
                double gap = now - s_tap_last_k;
                s_tap_last_k = now;
                if (gap > 0.1 && gap < 3.0) {
                    double new_bpm = 60.0 / gap;
                    if (s_tap_count_k == 0) s_tap_avg_k = new_bpm;
                    else s_tap_avg_k = s_tap_avg_k * 0.6 + new_bpm * 0.4;
                    ++s_tap_count_k;
                    state.bpm = (float)std::clamp(s_tap_avg_k, 20.0, 999.0);
                    if (cbs.on_bpm_changed) cbs.on_bpm_changed(state.bpm);
                } else {
                    s_tap_count_k = 0;
                    s_tap_avg_k   = 0.0;
                }
            }
            // Space: toggle play/pause on the active timeline (global, window-focus-independent)
            if (ImGui::IsKeyPressed(ImGuiKey_Space, /*repeat=*/false) &&
                !ctx.timeline_view_id.empty()) {
                const UIState::TimelineInfo* tl_sp = nullptr;
                for (const auto& t : state.timelines)
                    if (t.id == ctx.timeline_view_id) { tl_sp = &t; break; }
                if (tl_sp) {
                    if (tl_sp->state == TimelineState::Playing) {
                        if (cbs.on_timeline_pause) cbs.on_timeline_pause(ctx.timeline_view_id);
                    } else {
                        if (cbs.on_timeline_play)  cbs.on_timeline_play(ctx.timeline_view_id);
                    }
                }
            }
        }
    }

    // =========================================================================
    // View mode dispatch — dockspace + panels
    // =========================================================================
    using VM = LayoutContext::ViewMode;

    // NOTE: BeginViewportSideBar() updates WorkPos/WorkSize immediately, so
    // WorkPos.y already accounts for the menu bar and transport bar.  Do NOT
    // add top_offset again when positioning dockspace / host windows.
    float menubar_h   = ImGui::GetFrameHeight();
    float transport_h = 44.f;
    float playback_h  = 264.f;
    (void)menubar_h; (void)transport_h;  // kept for documentation only
    float dock_h      = main_vp->WorkSize.y - playback_h;

    if (ctx.active_view == VM::Programmer) {
        // PROGRAMMER: Frame Editor + Laser Preview in dockspace
        ImGuiWindowFlags host_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove     |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;

        ImGui::SetNextWindowPos(ImVec2(main_vp->WorkPos.x, main_vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(main_vp->WorkSize.x, dock_h));
        ImGui::SetNextWindowViewport(main_vp->ID);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin(kWinMain, nullptr, host_flags);
        ImGui::PopStyleVar(3);

        ImGuiID dockspace_id = ImGui::GetID("MainDockSpace");
        ImGui::DockSpace(dockspace_id, ImVec2(0.f, 0.f), 0);

        if (!ctx.dockspace_initialised) {
            ctx.dockspace_initialised = true;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id, ImVec2(main_vp->WorkSize.x, dock_h));

            // Right zone (~42%): streams column + preview/3D column.
            // Streams sits immediately left of the two output previews.
            ImGuiID dock_right_zone, dock_center;
            ImGui::DockBuilderSplitNode(dockspace_id, ImGuiDir_Right, 0.42f,
                                        &dock_right_zone, &dock_center);

            // Within right zone: previews take ~2/3, streams take ~1/3
            ImGuiID dock_right, dock_streams;
            ImGui::DockBuilderSplitNode(dock_right_zone, ImGuiDir_Right, 0.67f,
                                        &dock_right, &dock_streams);

            ImGuiID dock_right_top, dock_right_bot;
            ImGui::DockBuilderSplitNode(dock_right, ImGuiDir_Down, 0.45f,
                                        &dock_right_bot, &dock_right_top);

            ImGui::DockBuilderDockWindow(kWinFrameEditor,   dock_center);
            ImGui::DockBuilderDockWindow(kWinStreams,        dock_streams);
            ImGui::DockBuilderDockWindow(kWinPreview,       dock_right_top);
            ImGui::DockBuilderDockWindow("3D Preview##3dp", dock_right_bot);
            ImGui::DockBuilderFinish(dockspace_id);
        }
        ImGui::End();

        panel_frame_editor(state, ctx, cbs);
        panel_preview     (state, ctx, cbs);
        panel_streams     (state, ctx, cbs);

    } else if (ctx.active_view == VM::Show) {
        // SHOW: DockSpace-based layout — left 30% (Timelines top / Cue Library bottom),
        // right 70% (Output Preview full height), BPM and Timeline Editor also docked.
        // DockBuilder init is gated on ctx.show_layout_initialised (reset on view switch).
        ImGuiWindowFlags show_host_flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove     |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;

        ImGui::SetNextWindowPos(ImVec2(main_vp->WorkPos.x, main_vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(main_vp->WorkSize.x, dock_h));
        ImGui::SetNextWindowViewport(main_vp->ID);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("ShowDockHost##show_host", nullptr, show_host_flags);
        ImGui::PopStyleVar(3);

        ImGuiID show_dockspace_id = ImGui::GetID("ShowDockSpace");
        ImGui::DockSpace(show_dockspace_id, ImVec2(0.f, 0.f), 0);

        if (!ctx.show_layout_initialised) {
            ctx.show_layout_initialised = true;
            ImGui::DockBuilderRemoveNode(show_dockspace_id);
            ImGui::DockBuilderAddNode(show_dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(show_dockspace_id, ImVec2(main_vp->WorkSize.x, dock_h));

            // ── Reference layout ──────────────────────────────────────────────
            // Left  60%: Playbacks grid (top 55%) + Timeline Editor (bottom 45%)
            // Mid   28%: Timelines list  (full height, tabbed w/ Cue Library)
            // Right 12%: BPM (top ~15%) + Laser Preview (mid ~42%) + 3D Preview (bot ~43%)
            // ─────────────────────────────────────────────────────────────────

            // 1. Carve off the right preview column (~12% of total)
            ImGuiID dock_main, dock_preview_col;
            ImGui::DockBuilderSplitNode(show_dockspace_id, ImGuiDir_Right, 0.18f,
                                        &dock_preview_col, &dock_main);

            // 2. From the remaining main, carve off the Timelines column (~28/82 = 34%)
            ImGuiID dock_left, dock_timelines;
            ImGui::DockBuilderSplitNode(dock_main, ImGuiDir_Right, 0.34f,
                                        &dock_timelines, &dock_left);

            // 3. Split left into Playbacks (top 55%) and Timeline Editor (bottom 45%)
            ImGuiID dock_playbacks, dock_tl_editor;
            ImGui::DockBuilderSplitNode(dock_left, ImGuiDir_Down, 0.45f,
                                        &dock_tl_editor, &dock_playbacks);

            // 4. Split preview column: BPM small top (~15%), previews below (~85%)
            ImGuiID dock_bpm, dock_previews;
            ImGui::DockBuilderSplitNode(dock_preview_col, ImGuiDir_Down, 0.85f,
                                        &dock_previews, &dock_bpm);

            // 5. Split previews: 2D top (55%), 3D bottom (45%)
            ImGuiID dock_2d, dock_3d;
            ImGui::DockBuilderSplitNode(dock_previews, ImGuiDir_Down, 0.45f,
                                        &dock_3d, &dock_2d);

            ImGui::DockBuilderDockWindow("Playbacks##show_view_grid", dock_playbacks);
            ImGui::DockBuilderDockWindow("Timeline Editor##tlv",      dock_tl_editor);
            ImGui::DockBuilderDockWindow("Timelines##tl_list",        dock_timelines);
            ImGui::DockBuilderDockWindow("Cue Library##w",            dock_timelines);
            ImGui::DockBuilderDockWindow("BPM##bpm_float",            dock_bpm);
            ImGui::DockBuilderDockWindow("Laser Preview##w",          dock_2d);
            ImGui::DockBuilderDockWindow("3D Preview##3dp",           dock_3d);
            ImGui::DockBuilderFinish(show_dockspace_id);
        }
        ImGui::End();

        // ── BPM panel ─────────────────
        {
            ImGui::SetNextWindowBgAlpha(0.88f);
            ImGuiWindowFlags bpm_flags = ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoNav |
                                          ImGuiWindowFlags_NoScrollWithMouse;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.f, 6.f));
            if (ImGui::Begin("BPM##bpm_float", &ctx.bpm_open, bpm_flags)) {
                ImGui::SetNextItemWidth(110.f);
                ImGui::DragFloat("##bpm_w", &state.bpm, 0.5f, 20.f, 999.f, "%.1f BPM");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("BPM — drag to adjust, Ctrl+click to type\nRange: 20–999 BPM");
                {
                    static float s_prev_bpm_w = 120.f;
                    if (std::fabs(state.bpm - s_prev_bpm_w) > 0.05f) {
                        s_prev_bpm_w = state.bpm;
                        if (cbs.on_bpm_changed) cbs.on_bpm_changed(state.bpm);
                    }
                }
                ImGui::SameLine(0, 4);
                // Beat flash indicator
                {
                    float t_b    = (float)ImGui::GetTime();
                    float beat_s = 60.f / std::max(state.bpm, 1.f);
                    float phase  = std::fmod(t_b, beat_s) / beat_s;
                    bool  on     = (phase < 0.15f);
                    ImVec4 flash_col = on
                        ? ImVec4(theme::accent().x, theme::accent().y, theme::accent().z, 1.f)
                        : ImVec4(0.15f, 0.15f, 0.20f, 1.f);
                    float item_h_w = ImGui::GetFrameHeight();
                    ImDrawList* dl  = ImGui::GetWindowDrawList();
                    ImVec2      pos = ImGui::GetCursorScreenPos();
                    pos.y += (item_h_w - 12.f) * 0.5f;
                    dl->AddRectFilled(pos, ImVec2(pos.x + 12.f, pos.y + 12.f),
                                      ImGui::ColorConvertFloat4ToU32(flash_col), 2.f);
                    ImGui::Dummy(ImVec2(12.f, item_h_w));
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Beat indicator — %.1f BPM", state.bpm);
                }
                // TAP button
                {
                    static double s_tap_last_w  = 0.0;
                    static double s_tap_avg_w   = 0.0;
                    static int    s_tap_count_w = 0;
                    if (ImGui::Button("TAP##bpm_tap_w", ImVec2(-1.f, 0.f))) {
                        double now = ImGui::GetTime();
                        double gap = now - s_tap_last_w;
                        s_tap_last_w = now;
                        if (gap > 0.1 && gap < 3.0) {
                            double new_bpm = 60.0 / gap;
                            if (s_tap_count_w == 0) s_tap_avg_w = new_bpm;
                            else s_tap_avg_w = s_tap_avg_w * 0.6 + new_bpm * 0.4;
                            ++s_tap_count_w;
                            state.bpm = (float)std::clamp(s_tap_avg_w, 20.0, 999.0);
                            if (cbs.on_bpm_changed) cbs.on_bpm_changed(state.bpm);
                        } else {
                            s_tap_count_w = 0;
                            s_tap_avg_w   = 0.0;
                        }
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Tap BPM — click on the beat to set tempo");
                }
                // Tap key binding
                ImGui::Separator();
                {
                    char tap_key_label[32] = "None";
                    if (state.bpm_tap_key != 0) {
                        const char* kn = ImGui::GetKeyName(static_cast<ImGuiKey>(state.bpm_tap_key));
                        if (kn) std::snprintf(tap_key_label, sizeof(tap_key_label), "%s", kn);
                    }
                    ImGui::TextDisabled("Tap Key: %s", tap_key_label);
                    ImGui::SameLine(0, 4);
                    if (state.bpm_key_capturing) {
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.f, 1.f), "[key...]");
                        for (int k = static_cast<int>(ImGuiKey_Tab);
                             k < static_cast<int>(ImGuiKey_ReservedForModCtrl); ++k) {
                            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                                state.bpm_tap_key    = k;
                                state.bpm_key_capturing = false;
                                break;
                            }
                        }
                    } else {
                        if (ImGui::SmallButton("Set##bk")) state.bpm_key_capturing = true;
                        ImGui::SameLine(0, 4);
                        if (ImGui::SmallButton("X##bkc")) state.bpm_tap_key = 0;
                    }
                }

                // ── Timeline keyboard bindings ─────────
                ImGui::Separator();
                ImGui::TextDisabled("Timeline Keys:");
                struct TlKeyBind { const char* label; int* key; int slot_id; };
                TlKeyBind tl_binds[] = {
                    { "Play/Pause",   &state.tl_key_play,   1 },
                    { "Stop",         &state.tl_key_stop,   2 },
                    { "Rewind",       &state.tl_key_rewind, 3 },
                    { "WaitForGo",    &state.tl_key_go,     4 },
                };
                for (auto& b : tl_binds) {
                    ImGui::PushID(b.slot_id);
                    char kl[32] = "None";
                    if (*b.key != 0) {
                        const char* kn = ImGui::GetKeyName(static_cast<ImGuiKey>(*b.key));
                        if (kn) std::snprintf(kl, sizeof(kl), "%s", kn);
                    }
                    ImGui::TextDisabled("%-12s %s", b.label, kl);
                    ImGui::SameLine(0, 4);
                    bool capturing = (state.tl_key_capture_active && state.tl_key_capturing == b.slot_id);
                    if (capturing) {
                        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.f, 1.f), "[key...]");
                        for (int k = static_cast<int>(ImGuiKey_Tab);
                             k < static_cast<int>(ImGuiKey_ReservedForModCtrl); ++k) {
                            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                                *b.key                      = k;
                                state.tl_key_capture_active = false;
                                state.tl_key_capturing      = 0;
                                break;
                            }
                        }
                    } else {
                        if (ImGui::SmallButton("Set##tlks"))  {
                            state.tl_key_capturing      = b.slot_id;
                            state.tl_key_capture_active = true;
                        }
                        ImGui::SameLine(0, 2);
                        if (ImGui::SmallButton("X##tlkc")) *b.key = 0;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }

        // ── All Show-view dockable panels ─
        draw_show_view_grid(state, ctx, cbs, main_vp->WorkPos, main_vp->WorkSize, 0.f, 0.f);
        panel_timeline_view(state, ctx, cbs);
        panel_timelines(state, ctx, cbs);
        panel_cue_library(state, ctx, cbs);
        panel_preview(state, ctx, cbs);

        // ── Optional floating panels ──────
        if (ctx.quickshow_open) panel_quickshow(state, cbs, &ctx.quickshow_open);
        if (ctx.livepro_open)   panel_livepro(state, cbs, &ctx.livepro_open);

    } else if (ctx.active_view == VM::Setup) {
        // Render setup inline in the main content area — same dimensions as the
        // Programmer host window so it fills the space between transport and playback bars.
        // This makes Setup a proper resizable full-area view instead of a floating popup.
        ImGuiWindowFlags setup_host_flags =
            ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize        | ImGuiWindowFlags_NoMove     |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;

        ImGui::SetNextWindowPos(ImVec2(main_vp->WorkPos.x, main_vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(main_vp->WorkSize.x, dock_h));
        ImGui::SetNextWindowViewport(main_vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.f, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##setup_inline_host", nullptr, setup_host_flags);
        ImGui::PopStyleVar(3);
        panel_setup_content(state, ctx, cbs);
        ImGui::End();

    } else if (ctx.active_view == VM::Patch) {
        ImGuiWindowFlags patch_host_flags =
            ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize        | ImGuiWindowFlags_NoMove     |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;

        ImGui::SetNextWindowPos(ImVec2(main_vp->WorkPos.x, main_vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(main_vp->WorkSize.x, dock_h));
        ImGui::SetNextWindowViewport(main_vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##patch_inline_host", nullptr, patch_host_flags);
        ImGui::PopStyleVar(3);
        panel_patch(state, ctx, cbs);
        ImGui::End();

    } else if (ctx.active_view == VM::Safety) {
        ImGuiWindowFlags safety_host_flags =
            ImGuiWindowFlags_NoTitleBar      | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoResize        | ImGuiWindowFlags_NoMove     |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoDocking;

        ImGui::SetNextWindowPos(ImVec2(main_vp->WorkPos.x, main_vp->WorkPos.y));
        ImGui::SetNextWindowSize(ImVec2(main_vp->WorkSize.x, dock_h));
        ImGui::SetNextWindowViewport(main_vp->ID);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.f, 8.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
        ImGui::Begin("##safety_inline_host", nullptr, safety_host_flags);
        ImGui::PopStyleVar(3);
        panel_safety_content(state, ctx, cbs);
        ImGui::End();
    }

    // 3D Preview — only render in Programmer and Show views.
    // In Safety and Setup views the window is suppressed so it cannot
    // float over panels where it does not belong.  The open preference
    // (ctx.panel_3d_open) is preserved so the window reappears when the
    // user returns to a view where it is permitted.
    {
        // Track whether the 3D window has been placed for the current Show-view session.
        // Reset whenever show_layout_initialised is cleared so re-entry forces a new placement.
        // Also reset when leaving Show view so that each Show-view entry gets a fresh placement.
        static bool s_3d_placed = false;
        if (!ctx.show_layout_initialised) s_3d_placed = false;
        if (ctx.active_view != VM::Show)  s_3d_placed = false;

        bool show_3d_in_this_view = (ctx.active_view == VM::Programmer ||
                                     ctx.active_view == VM::Show);

        if (ctx.panel_3d_open && show_3d_in_this_view) {
            if (ctx.active_view == VM::Show && !s_3d_placed) {
                ImGuiViewport* vp3d = ImGui::GetMainViewport();
                static constexpr float k3dRColW = 380.f;
                static constexpr float kBpmH3d  = 120.f;
                static constexpr float kMiniH3d = 200.f;
                float top3d = main_vp->WorkPos.y;
                float right3d = vp3d->WorkPos.x + vp3d->WorkSize.x - k3dRColW;
                float preview_bot = top3d + 8.f + kBpmH3d + 4.f + kMiniH3d + 4.f;
                float avail_h = vp3d->WorkPos.y + vp3d->WorkSize.y - 200.f - preview_bot - 4.f;
                ImGui::SetNextWindowPos(ImVec2(right3d, preview_bot), ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(k3dRColW - 8.f, std::max(80.f, avail_h)),
                                         ImGuiCond_Always);
                s_3d_placed = true;
            }
            panel_3d_preview(state, &ctx.panel_3d_open);
        }
    }

    // Playback bar — always visible
    panel_playback_bar(state, ctx, cbs);

    // Cuestack editor (floating, opened by double-click on any playback from any view)
    if (ctx.cuestack_open)
        panel_timeline(state, ctx, cbs);

    // New floating panels
    if (ctx.clock_open)        panel_clock(state, ctx, cbs);
    if (ctx.snake_open)        panel_snake(state, ctx);
    if (ctx.conn_status_open)  panel_connection_status(state, ctx);

    // Setup window (floating, only when open)
    panel_setup_window(state, ctx, cbs);

    // Command palette (top layer)
    palette.render(state);

}

// ─────────────────────────────────────────────────────────────────────────────
//  Operator sidebar — permanent left strip with REC/BLND/CLR/INCL/UPDT
// ─────────────────────────────────────────────────────────────────────────────
void panel_operator_sidebar(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    ImGuiWindowFlags sidebar_flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;

    ImGuiViewport* vp = ImGui::GetMainViewport();

    if (!ImGui::BeginViewportSideBar("##op_sidebar", vp, ImGuiDir_Left,
                                     56.f, sidebar_flags)) {
        ImGui::End();
        return;
    }

    float item_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(0.f, 3.f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);

    // ── REC ──────────────────────────────────────────────────────────────────
    {
        bool armed = ctx.rec_armed;
        if (armed) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.80f, 0.08f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.12f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.05f, 0.05f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.08f, 0.42f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f, 0.58f, 0.14f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.06f, 0.30f, 0.08f, 1.f));
        }
        if (ImGui::Button("REC##sb", ImVec2(item_w, 34.f))) {
            ctx.rec_armed = !armed;
            if (cbs.on_set_record_mode) cbs.on_set_record_mode(ctx.rec_armed);
        }
        ImGui::PopStyleColor(3);
        if (armed) {
            float t = (float)ImGui::GetTime();
            bool blink = (std::fmod(t, 0.4f) < 0.2f);
            if (blink) {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 rmax = ImGui::GetItemRectMax();
                dl->AddCircleFilled(ImVec2(rmax.x - 7.f, rmax.y - 7.f),
                                    4.f, IM_COL32(255, 60, 40, 255));
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Arm recording — next playback GO records the current programmer");
    }

    // ── BLND ─────────────────────────────────────────────────────────────────
    {
        bool blinded = ctx.frame_editor_blind;
        if (blinded) {
            float pulse = 0.55f + 0.45f * std::sin((float)ImGui::GetTime() * 7.0f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.80f * pulse, 0.04f, 0.04f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f,         0.10f, 0.10f, 1.f));
        }
        if (ImGui::Button("BLND##sb", ImVec2(item_w, 34.f)))
            ctx.frame_editor_blind = !blinded;
        if (blinded) ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Blind: programmer content hidden from live laser output");
    }

    // ── CLR ──────────────────────────────────────────────────────────────────
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.52f, 0.06f, 0.06f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.10f, 0.10f, 1.f));
        static double s_clr_last_t = 0.0;
        static int    s_clr_count  = 0;
        if (ImGui::Button("CLR##sb", ImVec2(item_w, 34.f))) {
            double now = ImGui::GetTime();
            if (now - s_clr_last_t > 1.0) s_clr_count = 0;
            s_clr_last_t = now;
            ++s_clr_count;

            {
                ProgrammerUndoEntry entry;
                entry.frame_editor        = ctx.frame_editor;
                entry.programmer_global   = ctx.programmer_global;
                entry.programmer_fx_layer = ctx.programmer_fx_layer;
                entry.programmer_feeds    = ctx.programmer_feeds;
                entry.active_stream_ids   = state.active_stream_ids;
                entry.active_group_id     = state.active_group_id;
                ctx.programmer_undo_stack.push_back(std::move(entry));
                if (ctx.programmer_undo_stack.size() > 32)
                    ctx.programmer_undo_stack.erase(ctx.programmer_undo_stack.begin());
            }

            ctx.frame_editor.objects.clear();
            ctx.frame_editor.selected_ids.clear();
            ctx.frame_editor.wip_points.clear();
            ctx.frame_editor.drawing      = false;
            ctx.frame_editor.placed.clear();
            ctx.frame_editor.anim_frames.clear();
            ctx.frame_editor.lasso_active  = false;
            ctx.frame_editor.rot_dragging  = false;
            ctx.frame_editor.scale_dragging = false;
            ctx.frame_editor.obj_dragging  = false;
            ctx.programmer_global    = {};
            ctx.programmer_fx_layer  = {};

            // Notify engine to wipe all per-stream programmer content.
            // ChamSys rule: only an explicit CLR press may clear programmer state;
            // deselecting streams must never trigger this path.
            if (cbs.on_clear_programmer) cbs.on_clear_programmer();

            // CLR erases the saved programmer-feed state for the currently
            // selected outputs so that re-selecting them starts clean.
            // If nothing is selected, erase all saved feed states.
            if (!state.active_stream_ids.empty()) {
                // Build the key for the current selection and erase it
                {
                    std::vector<int> sorted_ids = state.active_stream_ids;
                    std::sort(sorted_ids.begin(), sorted_ids.end());
                    std::string key;
                    for (int id : sorted_ids) {
                        if (!key.empty()) key += ',';
                        key += std::to_string(id);
                    }
                    ctx.programmer_feeds.erase(key);
                }
                // Also erase individual stream keys for all selected streams
                for (int id : state.active_stream_ids)
                    ctx.programmer_feeds.erase(std::to_string(id));
            } else {
                ctx.programmer_feeds.clear();
            }

            // CLR always deselects outputs, clears group, and cancels REM.
            // Pass empty old_ids to suppress latching — CLR already wiped the programmer.
            if (!state.active_stream_ids.empty()) {
                state.active_stream_ids.clear();
                state.active_group_id = -1;
                if (cbs.on_active_streams_changed)
                    cbs.on_active_streams_changed({}, state.active_stream_ids);
            }
            state.active_group_id = -1;
            state.rem_mode        = false;

            if (s_clr_count >= 2) {
                ctx.rec_armed           = false;
                ctx.stream_move_mode    = false;
                ctx.frame_editor_blind  = false;
                ctx.incl_armed          = false;
                ctx.included_cue_idx    = -1;
                ctx.included_pb_id      = -1;
                ctx.included_pb_cue_idx = -1;
                if (cbs.on_set_record_mode) cbs.on_set_record_mode(false);
                s_clr_count = 0;
            }
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(s_clr_count == 1
                ? "Press again to also clear REC, BLND, and INCL modes"
                : "Clear all objects in the programmer.\nPress twice quickly to also clear all record/include modes.");

        // ── REM (remove mode) ─────────────────────────────────────────────────
        {
            bool rem = state.rem_mode;
            if (rem) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.75f, 0.15f, 0.05f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.25f, 0.10f, 1.f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.06f, 0.06f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.55f, 0.10f, 0.10f, 1.f));
            }
            if (ImGui::Button("REM##sb", ImVec2(item_w, 34.f)))
                state.rem_mode = !rem;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(rem
                    ? "REM mode ON — click any item to delete it (deactivates after one use)"
                    : "REM: arm remove mode — next click on any item deletes it");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── INCL ─────────────────────────────────────────────────────────────────
    {
        bool incl = ctx.incl_armed;
        if (incl) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.75f, 0.65f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.85f, 0.10f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.38f, 0.32f, 0.03f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.50f, 0.06f, 1.f));
        }
        if (ImGui::Button("INCL##sb", ImVec2(item_w, 34.f)))
            ctx.incl_armed = !incl;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Include: arm then click a cue to load it into the programmer");

    }

    // ── UPDT ─────────────────────────────────────────────────────────────────
    {
        bool can_updt = (ctx.included_cue_idx >= 0) || (ctx.included_pb_id >= 0 && ctx.included_pb_cue_idx >= 0);
        if (!can_updt) ImGui::BeginDisabled(true);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.38f, 0.32f, 0.03f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f, 0.50f, 0.06f, 1.f));
        if (ImGui::Button("UPDT##sb", ImVec2(item_w, 34.f))) {
            if (ctx.included_pb_id >= 0 && ctx.included_pb_cue_idx >= 0) {
                // Update a playback cue
                FullCueEntry fce;
                if (state.pb_cuelist_pb_id == ctx.included_pb_id &&
                    ctx.included_pb_cue_idx < (int)state.pb_cuelist.size())
                    fce = state.pb_cuelist[ctx.included_pb_cue_idx];
                fce.keyframe_layer.objects       = ctx.frame_editor.objects;
                fce.keyframe_layer.symmetry_mode = static_cast<int>(ctx.frame_editor.symmetry);
                fce.global_layer = ctx.programmer_global;
                fce.fx_layer     = ctx.programmer_fx_layer;
                fce.per_stream_fx.clear();
                fce.per_stream_kf.clear();
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    try {
                        int sid = std::stoi(key);
                        fce.per_stream_fx[sid] = feed.fx;
                    } catch (...) {}
                }
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    try {
                        int sid = std::stoi(key);
                        if (!feed.objects.empty()) {
                            KeyframeLayer kf;
                            kf.objects = feed.objects;
                            fce.per_stream_kf[sid] = kf;
                        }
                    } catch (...) {}
                }
                if (cbs.on_playback_cue_update)
                    cbs.on_playback_cue_update(ctx.included_pb_id, ctx.included_pb_cue_idx, fce);
                ctx.included_pb_id      = -1;
                ctx.included_pb_cue_idx = -1;
            } else if (ctx.included_cue_idx >= 0) {
                // Update a main cuelist cue
                FullCueEntry fce;
                if (ctx.included_cue_idx < (int)state.full_cue_list.size())
                    fce = state.full_cue_list[ctx.included_cue_idx];
                fce.keyframe_layer.objects       = ctx.frame_editor.objects;
                fce.keyframe_layer.symmetry_mode = static_cast<int>(ctx.frame_editor.symmetry);
                fce.global_layer = ctx.programmer_global;
                fce.fx_layer     = ctx.programmer_fx_layer;
                fce.per_stream_fx.clear();
                fce.per_stream_kf.clear();
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    try {
                        int sid = std::stoi(key);
                        fce.per_stream_fx[sid] = feed.fx;
                    } catch (...) {}
                }
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    try {
                        int sid = std::stoi(key);
                        if (!feed.objects.empty()) {
                            KeyframeLayer kf;
                            kf.objects = feed.objects;
                            fce.per_stream_kf[sid] = kf;
                        }
                    } catch (...) {}
                }
                if (cbs.on_cuelist_update_entry)
                    cbs.on_cuelist_update_entry(ctx.included_cue_idx, fce);
                ctx.included_cue_idx = -1;
            }
        }
        ImGui::PopStyleColor(2);
        if (!can_updt) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(can_updt
                ? "Update: save programmer content back to the included cue"
                : "Update: no cue included — use INCL first");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── MOVE ─────────────────────────────────────────────────────────────────
    {
        bool mov = ctx.stream_move_mode;
        if (mov) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f, 0.40f, 0.75f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.52f, 0.90f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.20f, 0.35f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.30f, 0.52f, 1.f));
        }
        if (ImGui::Button("MOVE##sb", ImVec2(item_w, 34.f)))
            ctx.stream_move_mode = !mov;
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(mov
                ? "MOVE mode ON — drag output cells in STREAMS to reorder. Click to exit."
                : "MOVE: drag-and-drop outputs in STREAMS grid to match physical rig layout");
    }

    ImGui::PopStyleVar(2);
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Transport side bar (Row 2 — sits directly below the main menu bar)
// ─────────────────────────────────────────────────────────────────────────────
void panel_transport(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    (void)cbs;
    ImGuiWindowFlags sidebar_flags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings;

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // BeginViewportSideBar places the window flush against one edge of the
    // given viewport, below any existing side bars (i.e. below the menu bar).
    if (!ImGui::BeginViewportSideBar("##transport", vp, ImGuiDir_Up,
                                     44.f, sidebar_flags)) {
        ImGui::End();
        return;
    }

    // ── Vertical centering helper ────────────────────────────────────────────
    // We want controls vertically centred in the 44px bar.
    float bar_h    = ImGui::GetContentRegionAvail().y;
    float item_h   = ImGui::GetFrameHeight();
    float v_pad    = std::max(0.f, (bar_h - item_h) * 0.5f);

    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + v_pad);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 0.f));

    // ── VIEW MODE buttons: PROGRAMMER / SHOW / SETUP ────────────────────────
    {
        using VM = LayoutContext::ViewMode;
        auto view_btn = [&](const char* label, VM mode, ImVec4 active_col, const char* tip) {
            bool active = (ctx.active_view == mode);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        active_col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active_col);
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.05f, 0.05f, 0.05f, 1.f));
            }
            if (ImGui::Button(label, ImVec2(100.f, item_h))) {
                if (mode == VM::Programmer && ctx.active_view != VM::Programmer)
                    ctx.dockspace_initialised = false;
                if (mode == VM::Show && ctx.active_view != VM::Show)
                    ctx.show_layout_initialised = false;
                ctx.active_view = mode;
            }
            if (active) ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        view_btn("PROGRAMMER", VM::Programmer, theme::accent(),
                 "Programmer view: draw laser frames, edit cues, FX and automation");
        ImGui::SameLine(0, 2);
        view_btn("SHOW",       VM::Show,       ImVec4(0.05f, 0.55f, 0.10f, 1.f),
                 "Show view: full-screen playback grid for live operation");
        ImGui::SameLine(0, 2);
        view_btn("OUTPUTS",    VM::Patch,      ImVec4(0.65f, 0.40f, 0.05f, 1.f),
                 "Outputs view: configure multiple laser/NDI/HDMI outputs, per-output safety zones");
        ImGui::SameLine(0, 2);
        view_btn("SETUP",      VM::Setup,      ImVec4(0.25f, 0.40f, 0.65f, 1.f),
                 "Setup view: DAC, NDI, MIDI, and output configuration");
        ImGui::SameLine(0, 2);
        view_btn("SAFETY",     VM::Safety,     ImVec4(0.80f, 0.10f, 0.10f, 1.f),
                 "Safety view: emergency shutoff, scan-fail, blackout zones, BAM");
    }

    ImGui::SameLine(0, 12);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0, 12);

    // ── PROG indicator ───────────────────────────────────────────────────────
    {
        bool prog_active = !ctx.frame_editor.objects.empty() && !ctx.frame_editor_blind;
        ImGui::PushStyleColor(ImGuiCol_Text,
            prog_active ? ImVec4(0.f, 0.85f, 0.35f, 1.f) : ImVec4(0.30f, 0.30f, 0.35f, 1.f));
        ImGui::TextUnformatted("PROG");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(prog_active ? "Programmer active" : "Programmer empty or blind");
    }

    ImGui::SameLine(0, 12);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0, 12);

    // ── NDI status text ──────────────────────────────────────────────────────
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
            state.ndi_streaming
            ? ImVec4(0.f, 0.85f, 0.35f, 1.f)
            : ImVec4(0.30f, 0.30f, 0.35f, 1.f));
        ImGui::TextUnformatted("NDI");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(state.ndi_streaming ? "NDI streaming active" : "NDI idle");
    }

    ImGui::SameLine(0, 8);

    // ── OUTPUT status text ───────────────────────────────────────────────────
    {
        ImGui::PushStyleColor(ImGuiCol_Text,
            state.output_enabled
            ? ImVec4(0.f, 0.85f, 0.35f, 1.f)
            : ImVec4(0.85f, 0.15f, 0.10f, 1.f));
        ImGui::TextUnformatted("OUT");
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(state.output_enabled ? "Laser output enabled" : "Laser output KILLED");
    }

    ImGui::SameLine(0, 12);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0, 12);

    // ── PANIC button — always visible in transport bar ───────────────────────
    ImGui::SameLine(0, 8);
    {
        if (state.emergency_shutoff_active) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.90f, 0.05f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(1.00f, 0.10f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.70f, 0.03f, 0.03f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
            float blink = 0.5f + 0.5f * sinf(static_cast<float>(ImGui::GetTime()) * 8.f);
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.65f + blink * 0.35f, 0.f, 0.f, 1.f));
            ImGui::Button("!! SHUTOFF ACTIVE !!", ImVec2(160.f, item_h));
            ImGui::PopStyleColor(5);
            ImGui::SetItemTooltip("Emergency shutoff is active. Press Ctrl+Alt+Enter to release.");
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.35f, 0.05f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.08f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1.00f, 0.10f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.f, 0.7f, 0.7f, 1.f));
            if (ImGui::Button("PANIC", ImVec2(70.f, item_h))) {
                state.emergency_shutoff_active = true;
                if (cbs.on_emergency_shutoff) cbs.on_emergency_shutoff(true);
            }
            ImGui::PopStyleColor(4);
            ImGui::SetItemTooltip(
                "Emergency Shutoff — instantly kills all laser output.\n"
                "Press Ctrl+Alt+Enter to release.");
        }
    }

    ImGui::SameLine(0, 8);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);

    // ── Safety dot ───────────────────────────────────────────────────────────
    ImGui::SameLine(0, 8);
    {
        ImVec4 safety_col = state.bam.safety_ok
            ? ImVec4(0.f, 0.85f, 0.35f, 1.f)
            : ImVec4(1.f, 0.15f, 0.10f, 1.f);
        ImDrawList* dl  = ImGui::GetWindowDrawList();
        ImVec2      pos = ImGui::GetCursorScreenPos();
        float       r   = 5.f;
        pos.y += item_h * 0.5f;
        pos.x += r;
        dl->AddCircleFilled(pos, r, ImGui::ColorConvertFloat4ToU32(safety_col));
        ImGui::Dummy(ImVec2(r * 2.f + 2.f, item_h));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Safety: %s", state.bam.safety_status.c_str());
    }

    // Flashing safety text when unsafe
    if (!state.bam.safety_ok) {
        ImGui::SameLine(0, 4);
        float t     = (float)ImGui::GetTime();
        bool  blink = (std::fmod(t, 0.5f) < 0.25f);
        ImGui::PushStyleColor(ImGuiCol_Text,
            blink ? ImVec4(1.f, 0.18f, 0.10f, 1.f) : ImVec4(0.5f, 0.10f, 0.08f, 1.f));
        char safety_buf[32];
        std::snprintf(safety_buf, sizeof(safety_buf), "SAFETY: %s", state.bam.safety_status.c_str());
        ImGui::TextUnformatted(safety_buf);
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar(); // ItemSpacing
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cue Library panel
// ─────────────────────────────────────────────────────────────────────────────
void panel_cue_library(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    ImGui::SetNextWindowSize(ImVec2(700.f, 500.f), ImGuiCond_FirstUseEver);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertFloat4ToU32(theme::background()));

    if (!ImGui::Begin(kWinCueLib, &ctx.panel_cue_lib_open)) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    // Panel is visible — render content
        // --- Toolbar: search (full-width) + Add Cue button -------------------
        float add_btn_w = 28.f;
        float spacing   = ImGui::GetStyle().ItemSpacing.x;

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - add_btn_w - spacing);
        char search_buf[128];
        std::strncpy(search_buf, ctx.cue_search.c_str(), sizeof(search_buf) - 1);
        search_buf[sizeof(search_buf) - 1] = '\0';
        if (ImGui::InputTextWithHint("##cue_search", "Search cues...",
                                     search_buf, sizeof(search_buf))) {
            ctx.cue_search = search_buf;
        }

        ImGui::SameLine();

        // Add Cue (+) button
        ImGui::PushStyleColor(ImGuiCol_Button,
            ImGui::ColorConvertFloat4ToU32(theme::accent()));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
            ImVec4(theme::accent().x * 1.2f, theme::accent().y * 1.2f,
                   theme::accent().z * 1.2f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.f));
        if (ImGui::Button("+##add_cue", ImVec2(add_btn_w, 0.f))) {
            // Fire add callback if present, or append a placeholder
            if (cbs.on_cue_duplicate && !state.cues.empty()) {
                cbs.on_cue_duplicate(-1);   // -1 signals "new empty cue"
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add new cue");
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0, 4);
        if (ImGui::Button("Chaser##add_chaser")) {
            if (cbs.on_new_chaser_cue) cbs.on_new_chaser_cue();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add new chaser/step-sequence cue");

        // Cue count (right side of toolbar)
        ImGui::SameLine(0, 8);
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%d", (int)state.cues.size());
        ImGui::TextDisabled("%s cues", cnt);

        ImGui::Separator();

        // --- Cue list (single-column card rows) -------------------------------
        ImGui::BeginChild("##cue_grid", ImVec2(-1.f, -1.f), ImGuiChildFlags_None);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.f, 3.f));

        bool any_visible = false;
        for (const auto& cue : state.cues) {
            // Filter
            if (!ctx.cue_search.empty()) {
                std::string name_lower  = cue.name;
                std::string query_lower = ctx.cue_search;
                for (char& c : name_lower)  c = (char)std::tolower((unsigned char)c);
                for (char& c : query_lower) c = (char)std::tolower((unsigned char)c);
                if (name_lower.find(query_lower) == std::string::npos) continue;
            }

            any_visible = true;
            bool selected = (state.active_cue_idx == cue.index);
            int  idx      = cue.index;

            // ── Rich cue card row ────────────────────────────────────────────
            // Card occupies full available width; use Selectable as background
            float avail_w  = ImGui::GetContentRegionAvail().x;
            float card_h   = ImGui::GetTextLineHeightWithSpacing() * 2.f + 6.f;

            ImVec2 card_min = ImGui::GetCursorScreenPos();

            ImGui::PushID(idx);
            bool clicked = ImGui::Selectable("##cue_sel", selected,
                               ImGuiSelectableFlags_None,
                               ImVec2(avail_w, card_h));
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();

            if (clicked) {
                if (ctx.incl_armed) {
                    ctx.included_cue_idx = idx;
                    ctx.incl_armed = false;
                    if (idx >= 0 && idx < (int)state.full_cue_list.size()) {
                        const auto& fce = state.full_cue_list[idx];
                        if (!fce.keyframe_layer.objects.empty())
                            ctx.frame_editor.objects = fce.keyframe_layer.objects;
                        ctx.programmer_global    = fce.global_layer;
                        ctx.programmer_fx_layer  = fce.fx_layer;
                        // Restore per-stream FX into programmer_feeds so streams
                        // that had per-head FX overrides get them back correctly.
                        for (const auto& [sid, sfx] : fce.per_stream_fx) {
                            auto& feed = ctx.programmer_feeds[std::to_string(sid)];
                            feed.fx = sfx;
                        }
                    }
                    if (cbs.on_include_cue) cbs.on_include_cue(idx);
                } else {
                    state.active_cue_idx = idx;
                    if (cbs.on_cue_selected) cbs.on_cue_selected(idx);
                }
            }

            // Coloured left stripe — uses cue card_color
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImU32 stripe_col = ImGui::ColorConvertFloat4ToU32(
                ImVec4(cue.card_color.r, cue.card_color.g, cue.card_color.b, 0.9f));
            dl->AddRectFilled(card_min,
                              ImVec2(card_min.x + 4.f, card_min.y + card_h),
                              stripe_col, 1.f);

            // Active (playing) cue gets a bright kGreen 2px left-edge line
            bool is_active_playing = (state.playing && cue.index == state.active_cue_idx);
            if (is_active_playing) {
                ImU32 green_u32 = ImGui::ColorConvertFloat4ToU32(
                    ImVec4(theme::kGreen.x, theme::kGreen.y, theme::kGreen.z, 1.f));
                dl->AddLine(ImVec2(card_min.x + 1.f, card_min.y + 1.f),
                            ImVec2(card_min.x + 1.f, card_min.y + card_h - 1.f),
                            green_u32, 2.f);
            }

            // Text content overlaid on the selectable
            ImVec2 text_pos = ImVec2(card_min.x + 10.f, card_min.y + 3.f);
            ImGui::SetCursorScreenPos(text_pos);

            // Row 1: cue number + name
            char num_name[128];
            std::snprintf(num_name, sizeof(num_name), "%d.  %s",
                          cue.index + 1, cue.name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text,
                selected
                    ? ImGui::ColorConvertFloat4ToU32(theme::accent())
                    : ImGui::ColorConvertFloat4ToU32(theme::text_primary()));
            ImGui::TextUnformatted(num_name);
            ImGui::PopStyleColor();

            // Row 2: generator type + duration
            ImGui::SetCursorScreenPos(ImVec2(text_pos.x, text_pos.y
                + ImGui::GetTextLineHeightWithSpacing()));
            char detail[128];
            if (cue.duration_s > 0.f)
                std::snprintf(detail, sizeof(detail), "%s  |  %.1f s",
                              cue.generator.c_str(), cue.duration_s);
            else
                std::snprintf(detail, sizeof(detail), "%s  |  loop",
                              cue.generator.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
            ImGui::TextUnformatted(detail);
            ImGui::PopStyleColor();

            // Delete button — visible only when this row is hovered or selected
            if (hovered || selected) {
                float del_btn_w = 20.f;
                float del_btn_h = card_h - 4.f;
                ImVec2 del_pos  = ImVec2(card_min.x + avail_w - del_btn_w - 4.f,
                                         card_min.y + 2.f);
                ImGui::SetCursorScreenPos(del_pos);
                ImGui::PushID(idx + 10000);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f, 0.f, 0.f, 0.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.1f, 0.1f, 0.9f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(theme::accent_hot()));
                if (ImGui::Button("\xE2\x9C\x95##del", ImVec2(del_btn_w, del_btn_h))) {
                    if (cbs.on_cue_delete) cbs.on_cue_delete(idx);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete cue");
                ImGui::PopStyleColor(3);
                ImGui::PopID();
            }

            // Context menu (right-click) — kept for duplicate
            ImGui::PushID(idx + 20000);
            if (ImGui::BeginPopupContextItem("##cue_ctx")) {
                if (ImGui::MenuItem("Duplicate")) {
                    if (cbs.on_cue_duplicate) cbs.on_cue_duplicate(idx);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    if (cbs.on_cue_delete) cbs.on_cue_delete(idx);
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();

            // Restore cursor past this card row
            ImGui::SetCursorScreenPos(ImVec2(card_min.x,
                                             card_min.y + card_h + 3.f));
        }

        // Empty state
        if (!any_visible) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 20.f);
            float w      = ImGui::GetContentRegionAvail().x;
            const char* msg = state.cues.empty()
                ? "No cues — press + to add one"
                : "No cues match the search";
            float text_w = ImGui::CalcTextSize(msg).x;
            ImGui::SetCursorPosX((w - text_w) * 0.5f);
            ImGui::TextDisabled("%s", msg);
        }

        ImGui::PopStyleVar();
        ImGui::EndChild();
    ImGui::End();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Laser Preview panel
// ─────────────────────────────────────────────────────────────────────────────
void panel_preview(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    (void)cbs;
    ImGui::SetNextWindowSizeConstraints(ImVec2(240.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.02f, 0.02f, 0.04f, 1.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));

    if (!ImGui::Begin(kWinPreview, &ctx.panel_preview_open)) {
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return;
    }

    // Otaniemi Mode: scale up the entire preview panel for projector operators
    if (state.otaniemi.enabled) {
        ImGui::SetWindowFontScale(1.4f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 10.f));
    }

    {
        // --- Toolbar ----------------------------------------------------------
        ImGui::BeginGroup();

        // Resolution selector
        static int res_idx = 1;
        const char* resolutions[] = { "960x540", "1920x1080", "3840x2160" };
        ImGui::PushItemWidth(100.f);
        ImGui::Combo("##res", &res_idx, resolutions, 3);
        ImGui::PopItemWidth();
        ImGui::SetItemTooltip("NDI output resolution for the rendered laser preview");

        ImGui::SameLine(0, 8);

        if (ImGui::Button("Fit")) {
            state.preview_zoom = 0.f;  // 0 = fit mode
        }
        ImGui::SetItemTooltip("Scale the preview canvas to fill the available panel area");
        ImGui::SameLine(0, 4);
        if (ImGui::Button("100%")) {
            state.preview_zoom = 1.f;  // 1 = native pixels
        }
        ImGui::SetItemTooltip("Show the preview at native 1:1 pixel resolution");
        ImGui::SameLine(0, 16);

        // FPS overlay — warn in red below 60 fps, yellow below 30 fps
        char fps_buf[24];
        std::snprintf(fps_buf, sizeof(fps_buf), "%.0f fps", state.engine_fps);
        ImVec4 fps_col;
        if (state.engine_fps >= 59.f)
            fps_col = theme::accent();
        else if (state.engine_fps >= 29.f)
            fps_col = ImVec4(1.f, 0.80f, 0.05f, 1.f);   // yellow warning
        else
            fps_col = ImVec4(1.f, 0.18f, 0.10f, 1.f);   // red critical
        ImGui::PushStyleColor(ImGuiCol_Text, fps_col);
        ImGui::TextUnformatted(fps_buf);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            if (state.engine_fps < 59.f)
                ImGui::SetTooltip("Engine FPS: %.1f  (target 60 fps) -- performance warning", state.engine_fps);
            else
                ImGui::SetTooltip("Engine FPS: %.1f  (target 60 fps)", state.engine_fps);
        }

        ImGui::SameLine(0, 16);

        // DAC connection indicator — colour-coded green = connected, red = disconnected
        {
            ImU32 dac_col = state.dac_connected
                ? IM_COL32(30, 220, 60, 255)
                : IM_COL32(200, 40, 40, 255);
            ImVec2 dot_pos = ImGui::GetCursorScreenPos();
            float  dot_r   = 5.f;
            dot_pos.x += dot_r;
            dot_pos.y += ImGui::GetTextLineHeight() * 0.5f;
            ImGui::GetWindowDrawList()->AddCircleFilled(dot_pos, dot_r, dac_col);
            ImGui::Dummy(ImVec2(dot_r * 2.f + 2.f, ImGui::GetTextLineHeight()));
            if (state.dac_connected) {
                char dac_tip[64];
                std::snprintf(dac_tip, sizeof(dac_tip),
                    "DAC connected — %d pps output", state.dac_pps);
                ImGui::SetItemTooltip("%s", dac_tip);
            } else {
                ImGui::SetItemTooltip("DAC not connected — no laser output active");
            }
        }

        ImGui::SameLine(0, 6);

        // NDI indicator
        widgets::NDIStatusDot(state.ndi_streaming);
        ImGui::SetItemTooltip(state.ndi_streaming
            ? "NDI stream active — output is being broadcast over the network"
            : "NDI stream idle — start playback to begin broadcasting");

        ImGui::SameLine(0, 12);

        // Safety zone overlay toggle
        {
            bool zones_avail = state.safety_blackout.enabled;
            if (!zones_avail) ImGui::BeginDisabled();
            if (ctx.show_safety_overlay && zones_avail) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.08f, 0.08f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.12f, 0.12f, 1.f));
            }
            if (ImGui::Button("Zones")) {
                ctx.show_safety_overlay = !ctx.show_safety_overlay;
            }
            if (ctx.show_safety_overlay && zones_avail) {
                ImGui::PopStyleColor(2);
            }
            ImGui::SetItemTooltip(zones_avail
                ? (ctx.show_safety_overlay
                    ? "Hide safety blackout zone overlay"
                    : "Show safety blackout zone overlay")
                : "Safety blackout is disabled -- enable it in the BAM panel");
            if (!zones_avail) ImGui::EndDisabled();
        }

        ImGui::EndGroup();

        ImGui::Separator();

        // --- Preview area -----------------------------------------------------
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float  pw = std::min(avail.x, avail.y);
        float  ph = pw;

        float off_x = (avail.x - pw) * 0.5f;
        float off_y = (avail.y - ph) * 0.5f;

        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        img_pos.x += off_x;
        img_pos.y += off_y;

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Pure black canvas (laser output is always on black)
        dl->AddRectFilled(img_pos,
                          ImVec2(img_pos.x + pw, img_pos.y + ph),
                          IM_COL32(0, 0, 0, 255));

        // If we have a real texture from NDI, display it
        if (ctx.preview_texture) {
            dl->AddImage((ImTextureID)ctx.preview_texture,
                         img_pos, ImVec2(img_pos.x + pw, img_pos.y + ph));
        } else {
            // Draw laser beams as glowing lines
            if (!state.preview_points.empty()) {
                for (size_t i = 1; i < state.preview_points.size(); ++i) {
                    const auto& p0 = state.preview_points[i-1];
                    const auto& p1 = state.preview_points[i];
                    if (p0.blanked || p1.blanked) continue;

                    float x0 = img_pos.x + (p0.nx() * 0.5f + 0.5f) * pw;
                    float y0 = img_pos.y + (-p0.ny() * 0.5f + 0.5f) * ph;
                    float x1 = img_pos.x + (p1.nx() * 0.5f + 0.5f) * pw;
                    float y1 = img_pos.y + (-p1.ny() * 0.5f + 0.5f) * ph;

                    ImU32 col0  = IM_COL32(p0.r, p0.g, p0.b, 200);
                    ImU32 glow0 = IM_COL32(p0.r, p0.g, p0.b, 40);

                    // Glow line (thick, transparent)
                    dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), glow0, ctx.beam_thickness * 4.f);
                    // Core line (thin, opaque)
                    dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col0, ctx.beam_thickness);
                }
            } else {
                // No engine output — fall back to rendering programmer objects
                // directly so the preview reflects what the programmer contains
                // even when no cue is running.
                const bool has_prog_content =
                    !ctx.frame_editor.objects.empty() && !ctx.frame_editor_blind;

                if (has_prog_content) {
                    for (const auto& obj : ctx.frame_editor.objects) {
                        if (obj.pts.size() < 2) {
                            // Single-point object — draw as dot
                            if (!obj.pts.empty()) {
                                float sx = img_pos.x + (obj.pts[0].x * 0.5f + 0.5f) * pw;
                                float sy = img_pos.y + (-obj.pts[0].y * 0.5f + 0.5f) * ph;
                                ImU32 dc = IM_COL32(
                                    static_cast<int>(obj.r * 255.f),
                                    static_cast<int>(obj.g * 255.f),
                                    static_cast<int>(obj.b * 255.f), 200);
                                dl->AddCircleFilled(ImVec2(sx, sy), 3.f, dc);
                            }
                            continue;
                        }
                        ImU32 col  = IM_COL32(
                            static_cast<int>(obj.r * 255.f),
                            static_cast<int>(obj.g * 255.f),
                            static_cast<int>(obj.b * 255.f), 200);
                        ImU32 glow = IM_COL32(
                            static_cast<int>(obj.r * 255.f),
                            static_cast<int>(obj.g * 255.f),
                            static_cast<int>(obj.b * 255.f), 40);
                        for (size_t k = 0; k + 1 < obj.pts.size(); ++k) {
                            float x0 = img_pos.x + (obj.pts[k].x   * 0.5f + 0.5f) * pw;
                            float y0 = img_pos.y + (-obj.pts[k].y  * 0.5f + 0.5f) * ph;
                            float x1 = img_pos.x + (obj.pts[k+1].x * 0.5f + 0.5f) * pw;
                            float y1 = img_pos.y + (-obj.pts[k+1].y * 0.5f + 0.5f) * ph;
                            // Glow pass (thick, transparent)
                            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), glow, ctx.beam_thickness * 4.f);
                            // Core pass (thin, opaque)
                            dl->AddLine(ImVec2(x0, y0), ImVec2(x1, y1), col,  ctx.beam_thickness);
                        }
                    }
                } else {
                    // Show "NO ACTIVE CUE" only when both the engine and the programmer are idle
                    ImVec2 c = ImVec2(img_pos.x + pw * 0.5f - 55.f, img_pos.y + ph * 0.5f - 8.f);
                    dl->AddText(c, IM_COL32(60, 60, 60, 255), "NO ACTIVE CUE");
                }
            }
        }

        // --- Safety blackout zone overlay ------------------------------------
        if (state.safety_blackout.enabled && ctx.show_safety_overlay) {
            // Coordinate transform matches the laser-point transform above:
            //   screen_x = img_pos.x + (nx * 0.5 + 0.5) * pw
            //   screen_y = img_pos.y + (-ny * 0.5 + 0.5) * ph  (Y flipped)
            auto n2s_prev = [&](float nx, float ny) -> ImVec2 {
                return ImVec2(img_pos.x + (nx * 0.5f + 0.5f) * pw,
                              img_pos.y + (-ny * 0.5f + 0.5f) * ph);
            };

            constexpr ImU32 kFill    = IM_COL32(220,  0,  0,  60);
            constexpr ImU32 kOutline = IM_COL32(255,  0,  0, 180);
            constexpr ImU32 kZoneFill= IM_COL32(220,  0,  0,  60);
            constexpr ImU32 kZoneOut = IM_COL32(255,  0,  0, 200);
            constexpr ImU32 kLabel   = IM_COL32(255, 120, 120, 230);

            // --- Border crops (four axis-aligned filled bands) ---
            const auto& bdr = state.safety_blackout.borders;

            if (bdr.left > 0.f) {
                float cw = bdr.left * pw;
                dl->AddRectFilled(img_pos,
                                  ImVec2(img_pos.x + cw, img_pos.y + ph),
                                  kFill);
                dl->AddRect(img_pos,
                            ImVec2(img_pos.x + cw, img_pos.y + ph),
                            kOutline);
            }
            if (bdr.right > 0.f) {
                float cw = bdr.right * pw;
                ImVec2 p0(img_pos.x + pw - cw, img_pos.y);
                ImVec2 p1(img_pos.x + pw,      img_pos.y + ph);
                dl->AddRectFilled(p0, p1, kFill);
                dl->AddRect(p0, p1, kOutline);
            }
            if (bdr.top > 0.f) {
                float ch = bdr.top * ph;
                dl->AddRectFilled(img_pos,
                                  ImVec2(img_pos.x + pw, img_pos.y + ch),
                                  kFill);
                dl->AddRect(img_pos,
                            ImVec2(img_pos.x + pw, img_pos.y + ch),
                            kOutline);
            }
            if (bdr.bottom > 0.f) {
                float ch = bdr.bottom * ph;
                ImVec2 p0(img_pos.x,      img_pos.y + ph - ch);
                ImVec2 p1(img_pos.x + pw, img_pos.y + ph);
                dl->AddRectFilled(p0, p1, kFill);
                dl->AddRect(p0, p1, kOutline);
            }

            // Tilt hint: dashed diagonal line when tilt is non-zero
            if (bdr.tilt != 0.f) {
                float rad  = bdr.tilt * (3.14159265f / 180.f);
                float cxs  = img_pos.x + pw * 0.5f;
                float cys  = img_pos.y + ph * 0.5f;
                float hlen = std::min(pw, ph) * 0.5f;
                float dx   = std::cos(rad) * hlen;
                float dy   = std::sin(rad) * hlen;
                int segs   = 12;
                for (int k = 0; k < segs; k += 2) {
                    float t0 = static_cast<float>(k)     / static_cast<float>(segs);
                    float t1 = static_cast<float>(k + 1) / static_cast<float>(segs);
                    ImVec2 a(cxs - dx + 2.f * dx * t0, cys - dy + 2.f * dy * t0);
                    ImVec2 b(cxs - dx + 2.f * dx * t1, cys - dy + 2.f * dy * t1);
                    dl->AddLine(a, b, IM_COL32(255, 160, 0, 140), 1.5f);
                }
            }

            // --- Block zones (rotated filled rectangles) ---
            for (const auto& zone : state.safety_blackout.zones) {
                if (!zone.enabled) continue;

                float ang   = zone.angle_deg * (3.14159265f / 180.f);
                float cos_a = std::cos(ang);
                float sin_a = std::sin(ang);

                // Local corners relative to zone centre: TL, TR, BR, BL
                float lx[4] = { -zone.hw,  zone.hw,  zone.hw, -zone.hw };
                float ly[4] = {  zone.hh,  zone.hh, -zone.hh, -zone.hh };

                ImVec2 corners[4];
                for (int k = 0; k < 4; ++k) {
                    float rx = lx[k] * cos_a - ly[k] * sin_a;
                    float ry = lx[k] * sin_a + ly[k] * cos_a;
                    corners[k] = n2s_prev(zone.cx + rx, zone.cy + ry);
                }

                dl->AddQuadFilled(corners[0], corners[1], corners[2], corners[3], kZoneFill);
                dl->AddQuad(corners[0], corners[1], corners[2], corners[3], kZoneOut, 2.0f);

                // X marker at zone centre
                ImVec2 ctr = n2s_prev(zone.cx, zone.cy);
                constexpr float arm = 5.f;
                dl->AddLine(ImVec2(ctr.x - arm, ctr.y - arm),
                            ImVec2(ctr.x + arm, ctr.y + arm),
                            kZoneOut, 1.5f);
                dl->AddLine(ImVec2(ctr.x + arm, ctr.y - arm),
                            ImVec2(ctr.x - arm, ctr.y + arm),
                            kZoneOut, 1.5f);

                // Zone name label centred below the X marker
                if (!zone.name.empty()) {
                    ImVec2 ts = ImGui::CalcTextSize(zone.name.c_str());
                    ImVec2 tp(ctr.x - ts.x * 0.5f, ctr.y + arm + 2.f);
                    // Clamp inside canvas bounds
                    if (tp.x < img_pos.x + 2.f)               tp.x = img_pos.x + 2.f;
                    if (tp.y < img_pos.y + 2.f)               tp.y = img_pos.y + 2.f;
                    if (tp.x > img_pos.x + pw - ts.x - 2.f)  tp.x = img_pos.x + pw - ts.x - 2.f;
                    if (tp.y > img_pos.y + ph - ts.y - 2.f)  tp.y = img_pos.y + ph - ts.y - 2.f;
                    dl->AddText(tp, kLabel, zone.name.c_str());
                }
            }
        }
        // --- End safety blackout zone overlay --------------------------------

        // Border around canvas
        dl->AddRect(img_pos, ImVec2(img_pos.x + pw, img_pos.y + ph),
                    ImGui::ColorConvertFloat4ToU32(theme::border_color()));

        // LIVE / NO OUTPUT badge (top-right corner of canvas)
        {
            const char* badge_text = nullptr;
            ImU32 badge_bg = 0;
            if (state.playing && state.dac_connected && state.output_enabled) {
                badge_text = "  LIVE  ";
                badge_bg = IM_COL32(200, 30, 30, 220);
            } else if (state.playing && !state.dac_connected) {
                badge_text = " NO OUTPUT ";
                badge_bg = IM_COL32(140, 100, 0, 200);
            } else if (state.playing && !state.output_enabled) {
                badge_text = " OUT OFF ";
                badge_bg = IM_COL32(140, 40, 0, 200);
            }
            if (badge_text) {
                ImVec2 ts = ImGui::CalcTextSize(badge_text);
                float bpad = 4.f;
                float badge_w = ts.x + bpad * 2.f;
                float badge_h = ts.y + bpad * 2.f;
                ImVec2 badge_min = ImVec2(img_pos.x + pw - badge_w - 6.f, img_pos.y + 6.f);
                ImVec2 badge_max = ImVec2(badge_min.x + badge_w, badge_min.y + badge_h);
                dl->AddRectFilled(badge_min, badge_max, badge_bg, 4.f);
                dl->AddText(ImVec2(badge_min.x + bpad, badge_min.y + bpad),
                            IM_COL32(255, 255, 255, 255), badge_text);
            }
        }

        // Advance cursor past canvas
        ImGui::SetCursorScreenPos(ImVec2(img_pos.x, img_pos.y + ph + 4.f));
        ImGui::Dummy(ImVec2(pw, 0.f));

        // Right-click context menu on the preview
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && ImGui::IsWindowHovered()) {
            ImGui::OpenPopup("##preview_ctx");
        }
        if (ImGui::BeginPopup("##preview_ctx")) {
            if (ImGui::MenuItem("Fit to Window")) state.preview_zoom = 0.f;
            if (ImGui::MenuItem("100%"))          state.preview_zoom = 1.f;
            if (ImGui::MenuItem("200%"))          state.preview_zoom = 2.f;
            ImGui::EndPopup();
        }
        // Preview panel has no outbound callbacks — state writes are sufficient.
        (void)cbs;
    } // end BeginPopup block / toolbar group

    // Restore Otaniemi mode overrides
    if (state.otaniemi.enabled) {
        ImGui::PopStyleVar(); // ItemSpacing
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inspector panel
// ─────────────────────────────────────────────────────────────────────────────
void panel_inspector(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    ImGui::SetNextWindowSize(ImVec2(700.f, 500.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 260.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertFloat4ToU32(theme::surface()));

    if (!ImGui::Begin(kWinInspector, &ctx.panel_inspector_open)) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }
    // Panel is visible — render content
    {
        // Tab bar — scrollable to handle many tabs gracefully
        if (ImGui::BeginTabBar("##inspector_tabs",
                ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) {
            // ── Tab 0: Parameters ────────────────────────────────────────────
            if (ImGui::BeginTabItem("Parameters")) {
                ctx.inspector_tab = 0;
                GeneratorParams& p = state.active_params;

                ImGui::PushItemWidth(-120.f);

                if (state.active_cue_idx >= 0 && state.active_cue_idx < (int)state.cues.size()) {
                    ImGui::TextColored(theme::accent(), "%s",
                        state.cues[state.active_cue_idx].name.c_str());
                    ImGui::TextDisabled("%s",
                        state.cues[state.active_cue_idx].generator.c_str());
                } else {
                    ImGui::TextDisabled("No cue selected");
                }
                ImGui::Separator();

                ImGui::SeparatorText("Motion");

                // Helper macro: fire on_param_changed when slider is released
#define PARAM_SLIDER(label, field, lo, hi, fmt)                         \
                if (ImGui::SliderFloat(label, &p.field, lo, hi, fmt))   \
                    if (cbs.on_param_changed) cbs.on_param_changed(#field, p.field);

                PARAM_SLIDER("Speed",     speed,    0.01f, 10.f, "%.2f x")
                ImGui::SetItemTooltip("Animation playback speed multiplier (1.0 = normal)");
                PARAM_SLIDER("Scale",     scale,    0.01f, 5.f,  "%.2f x")
                ImGui::SetItemTooltip("Geometry scale factor applied to the entire cue (1.0 = native)");
                PARAM_SLIDER("Density",   density,  0.f,   1.f,  "%.2f")
                ImGui::SetItemTooltip("Point density along path segments — higher = smoother but slower");
                ImGui::SeparatorText("Generator");
                {
                    const IGenerator* gen = nullptr;
                    if (state.active_cue_idx >= 0 && state.active_cue_idx < (int)state.cues.size())
                        gen = find_generator(state.cues[state.active_cue_idx].generator);

                    if (gen) {
                        auto pdefs = gen->param_defs();
                        for (const auto& pd : pdefs) {
                            const char* lbl = pd.label ? pd.label : pd.name;
                            // Read current value from the appropriate slot
                            float val = pd.default_val;
                            std::string pname(pd.name);
                            if      (pname == "param_a") val = p.param_a;
                            else if (pname == "param_b") val = p.param_b;
                            else if (pname == "param_c") val = p.param_c;
                            else {
                                auto it = p.extra_params.find(pname);
                                val = (it != p.extra_params.end()) ? it->second : pd.default_val;
                            }
                            // Boolean params (fmt == "bool") render as a checkbox
                            if (pd.fmt && std::strcmp(pd.fmt, "bool") == 0) {
                                bool bval = (val > 0.5f);
                                if (ImGui::Checkbox(lbl, &bval)) {
                                    val = bval ? 1.f : 0.f;
                                    if (cbs.on_param_changed) cbs.on_param_changed(pd.name, val);
                                }
                            } else {
                                if (ImGui::SliderFloat(lbl, &val, pd.min_val, pd.max_val, pd.fmt))
                                    if (cbs.on_param_changed) cbs.on_param_changed(pd.name, val);
                            }
                        }
                    } else {
                        // Fallback: 3 generic sliders
                        PARAM_SLIDER("Param A", param_a, 0.f, 1.f, "%.2f")
                        PARAM_SLIDER("Param B", param_b, 0.f, 1.f, "%.2f")
                        PARAM_SLIDER("Param C", param_c, 0.f, 1.f, "%.2f")
                    }
                }
                ImGui::Separator();
#undef PARAM_SLIDER

                // Colour A
                float col_a[4] = { p.color_a.r, p.color_a.g, p.color_a.b, p.color_a.a };
                if (ImGui::ColorEdit4("Color A", col_a,
                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
                    p.color_a = { col_a[0], col_a[1], col_a[2], col_a[3] };
                    if (cbs.on_param_changed) cbs.on_param_changed("color_a_r", col_a[0]);
                }
                float col_b[4] = { p.color_b.r, p.color_b.g, p.color_b.b, p.color_b.a };
                if (ImGui::ColorEdit4("Color B", col_b,
                        ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
                    p.color_b = { col_b[0], col_b[1], col_b[2], col_b[3] };
                }
                ImGui::Separator();

#define PARAM_SLIDER(label, field, lo, hi, fmt)                         \
                if (ImGui::SliderFloat(label, &p.field, lo, hi, fmt))   \
                    if (cbs.on_param_changed) cbs.on_param_changed(#field, p.field);

                ImGui::SeparatorText("Transform");
                PARAM_SLIDER("Rotation", rotation, -3.14159f, 3.14159f, "%.2f rad")
                ImGui::SetItemTooltip("Global rotation applied to the cue geometry (radians)");
                PARAM_SLIDER("Pan",      pan,       -1.f, 1.f, "%.2f")
                ImGui::SetItemTooltip("Horizontal position offset — -1.0 = far left, +1.0 = far right");
                PARAM_SLIDER("Tilt",     tilt,      -1.f, 1.f, "%.2f")
                ImGui::SetItemTooltip("Vertical position offset — -1.0 = bottom, +1.0 = top");
                PARAM_SLIDER("Zoom",     zoom,       0.1f, 4.f, "%.2f x")
                ImGui::SetItemTooltip("Output zoom level (1.0 = no zoom)");
                ImGui::SeparatorText("Output");
                PARAM_SLIDER("Intensity", intensity, 0.f, 1.f, "%.0f%%")
                ImGui::SetItemTooltip("Per-cue output intensity (0%% = off, 100%% = full)");
#undef PARAM_SLIDER

                if (ImGui::Checkbox("Blanked", &p.blanked))
                    if (cbs.on_param_changed) cbs.on_param_changed("blanked", p.blanked ? 1.f : 0.f);
                ImGui::SetItemTooltip("When checked the laser output for this cue is suppressed (blanked)");
                ImGui::SliderInt("Points", &p.point_count, 64, kMaxPointRate, "%d pts");
                ImGui::SetItemTooltip("Number of output points per frame — higher = more detail, lower = faster DAC rate");
                ImGui::Separator();

                // XY pad for pan/tilt
                ImGui::SeparatorText("Pan / Tilt");
                bool xy_changed = widgets::XYPad("##pt_pad", &p.pan, &p.tilt, ImVec2(100.f, 100.f));
                ImGui::SetItemTooltip("Drag to set Pan (X) and Tilt (Y) simultaneously");
                if (xy_changed) {
                    if (cbs.on_param_changed) {
                        cbs.on_param_changed("pan",  p.pan);
                        cbs.on_param_changed("tilt", p.tilt);
                    }
                }

                ImGui::SeparatorText("Beam");
                widgets::BeamThicknessSlider("##beam_w", &ctx.beam_thickness, 150.f);
                ImGui::SetItemTooltip("Preview beam render thickness in pixels (does not affect DAC output)");

                ImGui::PopItemWidth();
                ImGui::EndTabItem();
            }

            // ── Tab 1: DMX ──────────────────────────────────────────────────
            if (ImGui::BeginTabItem("DMX##insp")) {
                ctx.inspector_tab = 1;

                ImGui::SeparatorText("Universe 0  (ch 1-40)");

                // Show first 40 channels as fader stack
                static uint8_t dmx_copy[40]{};
                for (int i = 0; i < 40; ++i)
                    dmx_copy[i] = state.dmx_uni_0[i + 1];

                ImGui::BeginChild("##dmx_faders",
                    ImVec2(-1.f, 160.f), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
                bool u0_changed = widgets::FaderStack("##faders0", dmx_copy, 40,
                                    22.f, 120.f, false);
                ImGui::EndChild();

                if (u0_changed) {
                    for (int i = 0; i < 40; ++i) {
                        if (dmx_copy[i] != state.dmx_uni_0[i + 1]) {
                            state.dmx_uni_0[i + 1] = dmx_copy[i];
                            if (cbs.on_dmx_patch) cbs.on_dmx_patch(0, i + 1, dmx_copy[i]);
                        }
                    }
                }

                ImGui::SeparatorText("Universe 1  (ch 1-40)");
                ImGui::BeginChild("##dmx_faders1",
                    ImVec2(-1.f, 160.f), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
                static uint8_t dmx_copy1[40]{};
                for (int i = 0; i < 40; ++i)
                    dmx_copy1[i] = state.dmx_uni_1[i + 1];
                bool u1_changed = widgets::FaderStack("##faders1", dmx_copy1, 40,
                                    22.f, 120.f, false);
                ImGui::EndChild();

                if (u1_changed) {
                    for (int i = 0; i < 40; ++i) {
                        if (dmx_copy1[i] != state.dmx_uni_1[i + 1]) {
                            state.dmx_uni_1[i + 1] = dmx_copy1[i];
                            if (cbs.on_dmx_patch) cbs.on_dmx_patch(1, i + 1, dmx_copy1[i]);
                        }
                    }
                }

                ImGui::SeparatorText("ArtNet Latency");
                {
                    char lat_buf[32];
                    std::snprintf(lat_buf, sizeof(lat_buf), "%.1f ms", state.artnet_latency_ms);
                    ImGui::TextDisabled("Current: %s", lat_buf);
                    ImGui::SetItemTooltip(
                        "Round-trip ArtNet packet latency in milliseconds\n"
                        "Target: <5 ms  |  Warning: >20 ms  |  Critical: >50 ms");
                }
                float p99 = 0.f;
                for (int i = 0; i < UIState::kLatencyHistoryLen; ++i)
                    p99 = std::max(p99, state.latency_history[i]);
                widgets::LatencyMeter("##latency",
                    state.latency_history, UIState::kLatencyHistoryLen,
                    state.latency_history_idx, p99, ImVec2(200.f, 40.f));
                ImGui::SetItemTooltip("Rolling 64-sample ArtNet latency histogram (ms)");

                ImGui::EndTabItem();
            }

            // ── Tab 2: Automation ────────────────────────────────────────────
            if (ImGui::BeginTabItem("Automation##insp")) {
                ctx.inspector_tab = 2;

                if (state.automation_tracks.empty()) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16.f);
                    ImGui::TextDisabled("No automation tracks for this cue.");
                    ImGui::TextDisabled("Right-click a parameter to add automation.");
                } else {
                    static const char* kCurveNames[] = { "Lin", "EaseIn", "EaseOut", "Step" };
                    static constexpr int kCurveCount = 4;
                    static float s_kf_drag_start_t   = -1.f;
                    static float s_kf_drag_start_v   = -1.f;
                    static int   s_kf_drag_track      = -1;
                    static int   s_kf_drag_idx        = -1;
                    static int   s_kf_hover_track     = -1;
                    static int   s_kf_hover_idx       = -1;

                    float label_w    = 120.f;
                    float avail_w    = ImGui::GetContentRegionAvail().x;
                    float mini_tl_w  = avail_w - label_w - ImGui::GetStyle().ItemSpacing.x;
                    float mini_tl_h  = 48.f;
                    float loop_end   = (float)state.loop_end_s;
                    if (loop_end < 0.1f) loop_end = 4.f;

                    for (int ti = 0; ti < (int)state.automation_tracks.size(); ++ti) {
                        AutomationTrack& track = state.automation_tracks[ti];
                        ImGui::PushID(ti);

                        // ── Label column ─────────────────────────────────────
                        ImGui::BeginGroup();
                        ImGui::TextColored(theme::accent(), "%s", track.param_name.c_str());
                        ImGui::SameLine(0, 4);
                        ImGui::Checkbox("##vis", &track.visible);
                        ImGui::EndGroup();
                        ImGui::SameLine(label_w);

                        // ── Mini timeline canvas ──────────────────────────────
                        ImVec2 mt_pos  = ImGui::GetCursorScreenPos();
                        ImGui::InvisibleButton("##mt_hit",
                            ImVec2(mini_tl_w, mini_tl_h),
                            ImGuiButtonFlags_MouseButtonLeft |
                            ImGuiButtonFlags_MouseButtonRight);

                        bool mt_hovered = ImGui::IsItemHovered();
                        bool mt_lmb     = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        bool mt_rmb     = ImGui::IsItemClicked(ImGuiMouseButton_Right);
                        ImVec2 mt_mouse = ImGui::GetIO().MousePos;

                        auto mt_time_to_x = [&](float t) {
                            return mt_pos.x + (t / loop_end) * mini_tl_w;
                        };
                        auto mt_val_to_y = [&](float v) {
                            return mt_pos.y + mini_tl_h - v * mini_tl_h;
                        };
                        auto mt_x_to_time = [&](float x) {
                            return std::clamp((x - mt_pos.x) / mini_tl_w * loop_end,
                                              0.f, loop_end);
                        };
                        auto mt_y_to_val = [&](float y) {
                            return std::clamp(1.f - (y - mt_pos.y) / mini_tl_h, 0.f, 1.f);
                        };

                        ImDrawList* mdl = ImGui::GetWindowDrawList();
                        // Background
                        mdl->AddRectFilled(mt_pos,
                            ImVec2(mt_pos.x + mini_tl_w, mt_pos.y + mini_tl_h),
                            IM_COL32(12, 14, 20, 255));
                        mdl->AddRect(mt_pos,
                            ImVec2(mt_pos.x + mini_tl_w, mt_pos.y + mini_tl_h),
                            IM_COL32(50, 55, 70, 200));

                        // Playhead
                        {
                            float ph_x = mt_time_to_x((float)state.playhead_s);
                            mdl->AddLine({ph_x, mt_pos.y},
                                         {ph_x, mt_pos.y + mini_tl_h},
                                         IM_COL32(0, 230, 255, 100), 1.f);
                        }

                        // Draw curve connecting keyframes
                        if (track.keyframes.size() >= 2) {
                            for (int ki = 0; ki + 1 < (int)track.keyframes.size(); ++ki) {
                                float x0 = mt_time_to_x(track.keyframes[ki].time_s);
                                float y0 = mt_val_to_y(track.keyframes[ki].value);
                                float x1 = mt_time_to_x(track.keyframes[ki + 1].time_s);
                                float y1 = mt_val_to_y(track.keyframes[ki + 1].value);
                                mdl->AddLine({x0, y0}, {x1, y1},
                                             IM_COL32(0, 200, 230, 120), 1.f);
                            }
                        }

                        // Hit test + draw diamonds
                        float kf_r = 5.f;
                        for (int ki = 0; ki < (int)track.keyframes.size(); ++ki) {
                            Keyframe& kf = track.keyframes[ki];
                            float kx = mt_time_to_x(kf.time_s);
                            float ky = mt_val_to_y(kf.value);
                            bool kf_sel = (s_kf_drag_track == ti && s_kf_drag_idx == ki);
                            ImU32 kf_col = kf_sel
                                ? ImGui::ColorConvertFloat4ToU32(theme::accent())
                                : IM_COL32(200, 180, 50, 255);
                            mdl->AddQuadFilled(
                                {kx,        ky - kf_r},
                                {kx + kf_r, ky},
                                {kx,        ky + kf_r},
                                {kx - kf_r, ky},
                                kf_col);

                            float hdx = mt_mouse.x - kx;
                            float hdy = mt_mouse.y - ky;
                            bool hit  = (std::abs(hdx) + std::abs(hdy)) < kf_r + 2.f;

                            if (hit && mt_lmb) {
                                s_kf_drag_track   = ti;
                                s_kf_drag_idx     = ki;
                                s_kf_drag_start_t = kf.time_s;
                                s_kf_drag_start_v = kf.value;
                            }
                            if (hit && mt_rmb) {
                                s_kf_hover_track = ti;
                                s_kf_hover_idx   = ki;
                                ImGui::OpenPopup("##kf_auto_ctx");
                            }

                            // Curve combo on hover (show tooltip)
                            if (hit && mt_hovered) {
                                int cv = kf.curve;
                                if (cv < 0 || cv >= kCurveCount) cv = 0;
                                ImGui::SetTooltip("t=%.2fs  v=%.3f  curve=%s",
                                    kf.time_s, kf.value, kCurveNames[cv]);
                            }
                        }

                        // Drag keyframe
                        if (s_kf_drag_track == ti && s_kf_drag_idx >= 0
                                && s_kf_drag_idx < (int)track.keyframes.size()) {
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                Keyframe& dkf = track.keyframes[s_kf_drag_idx];
                                float new_t = mt_x_to_time(mt_mouse.x);
                                float new_v = mt_y_to_val(mt_mouse.y);
                                if (std::fabs(new_t - dkf.time_s) > 0.001f
                                        || std::fabs(new_v - dkf.value) > 0.001f) {
                                    dkf.time_s = new_t;
                                    dkf.value  = new_v;
                                    if (cbs.on_automation_move_kf)
                                        cbs.on_automation_move_kf(ti, s_kf_drag_idx,
                                                                    new_t, new_v);
                                }
                            } else {
                                s_kf_drag_track = -1;
                                s_kf_drag_idx   = -1;
                            }
                        }

                        // Click on empty area — add keyframe
                        if (mt_lmb && mt_hovered) {
                            // Only add if not near existing keyframe
                            bool near_existing = false;
                            float click_t = mt_x_to_time(mt_mouse.x);
                            for (const auto& ekf : track.keyframes) {
                                if (std::fabs(ekf.time_s - click_t) < 0.1f) {
                                    near_existing = true;
                                    break;
                                }
                            }
                            if (!near_existing) {
                                float click_v = mt_y_to_val(mt_mouse.y);
                                Keyframe new_kf;
                                new_kf.time_s = click_t;
                                new_kf.value  = click_v;
                                new_kf.curve  = 0;
                                auto ins_it = track.keyframes.begin();
                                while (ins_it != track.keyframes.end()
                                        && ins_it->time_s < new_kf.time_s)
                                    ++ins_it;
                                int ins_idx = (int)(ins_it - track.keyframes.begin());
                                track.keyframes.insert(ins_it, new_kf);
                                if (cbs.on_automation_add_kf)
                                    cbs.on_automation_add_kf(ti, click_t, click_v, 0);
                                s_kf_drag_track = ti;
                                s_kf_drag_idx   = ins_idx;
                            }
                        }

                        // Context menu for right-clicked keyframe
                        if (ImGui::BeginPopup("##kf_auto_ctx")) {
                            if (s_kf_hover_track == ti
                                    && s_kf_hover_idx >= 0
                                    && s_kf_hover_idx < (int)track.keyframes.size()) {
                                Keyframe& ckf = track.keyframes[s_kf_hover_idx];
                                ImGui::TextDisabled("KF @ %.2fs  v=%.3f",
                                    ckf.time_s, ckf.value);
                                ImGui::Separator();

                                // Curve selector
                                ImGui::TextDisabled("Curve:");
                                ImGui::SameLine();
                                int cv = ckf.curve;
                                ImGui::PushItemWidth(90.f);
                                if (ImGui::Combo("##kf_curve", &cv,
                                        kCurveNames, kCurveCount)) {
                                    ckf.curve = cv;
                                    if (cbs.on_automation_move_kf)
                                        cbs.on_automation_move_kf(ti, s_kf_hover_idx,
                                                                    ckf.time_s, ckf.value);
                                }
                                ImGui::PopItemWidth();

                                ImGui::Separator();
                                if (ImGui::MenuItem("Delete")) {
                                    track.keyframes.erase(
                                        track.keyframes.begin() + s_kf_hover_idx);
                                    if (cbs.on_automation_del_kf)
                                        cbs.on_automation_del_kf(ti, s_kf_hover_idx);
                                    s_kf_hover_track = -1;
                                    s_kf_hover_idx   = -1;
                                }
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::Separator();
                        ImGui::PopID();
                    }
                }
                ImGui::EndTabItem();
            }

            // ── Tab 3: FX Stack ──────────────────────────────────────────────
            if (ImGui::BeginTabItem("FX Stack##insp")) {
                ctx.inspector_tab = 3;
                int active_cue = state.active_cue_idx;

                if (state.active_fx_stack.empty()) {
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16.f);
                    ImGui::TextDisabled("No FX blocks for this cue.");
                    ImGui::TextDisabled("(FX engine integration in progress)");
                } else {
                    char blk_count_buf[32];
                    std::snprintf(blk_count_buf, sizeof(blk_count_buf),
                        "%d block(s)", (int)state.active_fx_stack.size());
                    ImGui::TextDisabled("%s", blk_count_buf);
                    ImGui::Separator();

                    for (int bi = 0; bi < (int)state.active_fx_stack.size(); ++bi) {
                        auto& blk = state.active_fx_stack[bi];
                        ImGui::PushID(bi);

                        // Block header: enable toggle
                        bool enabled = blk.enabled;
                        if (ImGui::Checkbox("##en", &enabled)) {
                            blk.enabled = enabled;
                            if (cbs.on_fx_enabled) cbs.on_fx_enabled(active_cue, bi, enabled);
                        }
                        ImGui::SetItemTooltip(
                            enabled ? "FX block enabled — click to disable"
                                    : "FX block disabled — click to enable");
                        ImGui::SameLine(0, 4);

                        // Category colour
                        ImVec4 cat_col = { 0.4f, 0.8f, 1.f, 1.f };   // Geometry: blue
                        if (blk.category == "Color")     cat_col = { 1.f, 0.6f, 0.2f, 1.f };
                        if (blk.category == "Time")      cat_col = { 0.8f, 0.4f, 1.f, 1.f };
                        if (blk.category == "Modulator") cat_col = { 0.4f, 1.f, 0.5f, 1.f };

                        ImGui::PushStyleColor(ImGuiCol_Text, cat_col);
                        bool open = ImGui::TreeNodeEx(blk.name.c_str(),
                            ImGuiTreeNodeFlags_DefaultOpen |
                            ImGuiTreeNodeFlags_SpanAvailWidth);
                        ImGui::PopStyleColor();

                        // Bypass toggle — clearly shows current state
                        {
                            float avail_x = ImGui::GetContentRegionAvail().x;
                            ImGui::SameLine(avail_x - 64.f + ImGui::GetScrollX());
                            bool bypassed = blk.bypassed;
                            // Colour: red=bypassed, dim=active
                            if (bypassed) {
                                ImGui::PushStyleColor(ImGuiCol_Button,
                                    ImVec4(0.55f, 0.10f, 0.10f, 1.f));
                                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                    ImVec4(0.70f, 0.15f, 0.15f, 1.f));
                            }
                            if (ImGui::SmallButton(bypassed ? "BYPASSED" : "ACTIVE  ")) {
                                bypassed = !bypassed;
                                blk.bypassed = bypassed;
                                if (cbs.on_fx_bypassed)
                                    cbs.on_fx_bypassed(active_cue, bi, bypassed);
                            }
                            if (blk.bypassed) ImGui::PopStyleColor(2);
                            ImGui::SetItemTooltip(
                                bypassed
                                ? "FX bypassed — signal passes through unprocessed; click to re-engage"
                                : "FX active — click to bypass this block without disabling it");
                        }

                        if (open) {
                            // Wet/dry slider — display as 0..100 %, store/send as 0..1
                            float wet_pct = blk.wet * 100.f;
                            ImGui::PushItemWidth(120.f);
                            if (ImGui::SliderFloat("Wet##wet", &wet_pct, 0.f, 100.f, "%.0f%%")) {
                                blk.wet = wet_pct / 100.f;
                                if (cbs.on_fx_wet) cbs.on_fx_wet(active_cue, bi, blk.wet);
                            }
                            ImGui::SetItemTooltip(
                                "Wet/dry mix — 0%% = dry (no FX), 100%% = fully processed");

                            // Parameters — Bug1 fix: use param.name, min_val/max_val, unit
                            for (auto& p : blk.params) {
                                float v = p.val;
                                std::string slider_id = "##fx_" + p.name;

                                // FxBlockInfo::Param has name, val, base_val
                                // Range defaults to 0..1 (params are normalised)
                                float lo = 0.f, hi = 1.f;
                                // Format string using param name as-is
                                // (unit suffix not available in FxBlockInfo::Param yet — use %.3f)
                                if (ImGui::SliderFloat(p.name.c_str(), &v, lo, hi, "%.3f")) {
                                    p.val = v;
                                    if (cbs.on_fx_param_changed)
                                        cbs.on_fx_param_changed(active_cue, bi, p.name.c_str(), v);
                                }
                                // Draw base_val marker as a thin vertical line inside the slider rect
                                if (std::fabs(p.base_val - p.val) > 0.001f || true) {
                                    ImVec2 smin = ImGui::GetItemRectMin();
                                    ImVec2 smax = ImGui::GetItemRectMax();
                                    float range = hi - lo;
                                    if (range > 0.f) {
                                        float t_base = (p.base_val - lo) / range;
                                        float bx = smin.x + t_base * (smax.x - smin.x);
                                        // 1px dotted marker in accent dim colour
                                        ImDrawList* fdl = ImGui::GetWindowDrawList();
                                        fdl->AddLine(ImVec2(bx, smin.y + 2.f),
                                                     ImVec2(bx, smax.y - 2.f),
                                                     ImGui::ColorConvertFloat4ToU32(
                                                         ImVec4(0.f, 0.898f, 1.f, 0.5f)), 1.f);
                                    }
                                }
                                // Show modulation delta when base and effective differ
                                if (std::fabs(p.val - p.base_val) > 0.001f) {
                                    ImGui::SameLine();
                                    char diff_buf[32];
                                    std::snprintf(diff_buf, sizeof(diff_buf),
                                        "(+%.3f)", p.val - p.base_val);
                                    ImGui::TextDisabled("%s", diff_buf);
                                }
                            }
                            ImGui::PopItemWidth();
                            ImGui::TreePop();
                        }
                        ImGui::Separator();
                        ImGui::PopID();
                    }
                }
                ImGui::EndTabItem();
            }

            // ── Tab 4: Expressions ──────────────────────────────────────────
            if (!state.automation_tracks.empty()) {
                if (ImGui::BeginTabItem("Expressions##insp")) {
                    ctx.inspector_tab = 4;

                    // Variable palette labels
                    static const char* const kVarNames[] = {
                        "t", "x", "i", "beat", "bar", "bpm",
                        "rms", "sub", "mid", "hi", "rand",
                        "pi", "tau", "e", "phi"
                    };
                    static constexpr int kVarCount = 15;

                    for (auto& track : state.automation_tracks) {
                        const std::string& param = track.param_name;
                        ExprEditorState& ed = ctx.expr_editors[param];

                        ImGui::PushID(param.c_str());
                        ImGui::TextColored(theme::accent(), "%s", param.c_str());
                        ImGui::Separator();

                        // Full-width monospace text field
                        ImGui::PushItemWidth(-1.f);
                        bool text_changed = ImGui::InputText(
                            "##expr_text", ed.text, sizeof(ed.text),
                            ImGuiInputTextFlags_EnterReturnsTrue |
                            ImGuiInputTextFlags_AutoSelectAll);
                        // Also detect any change via IsItemDeactivatedAfterEdit
                        text_changed = text_changed || ImGui::IsItemDeactivatedAfterEdit();
                        ImGui::PopItemWidth();

                        if (text_changed || ed.plot_dirty) {
                            // Compile the expression
                            ExpressionEngine eng;
                            std::string err;
                            bool ok = eng.compile(std::string(ed.text), err);
                            if (ok) {
                                ed.has_error = false;
                                ed.compile_error.clear();
                                // Rebuild preview plot
                                ExprContext preview_ctx;
                                preview_ctx.t   = 0.0;
                                preview_ctx.bpm = 120.f;
                                std::vector<float> samples;
                                eng.sample_preview(preview_ctx, ExprEditorState::kPlotSamples, samples);
                                for (int pi = 0; pi < ExprEditorState::kPlotSamples; ++pi)
                                    ed.plot_y[pi] = samples[pi];
                                ed.plot_dirty = false;
                                if (text_changed && cbs.on_expr_compiled)
                                    cbs.on_expr_compiled(param, std::string(ed.text));
                            } else {
                                ed.has_error = true;
                                ed.compile_error = err;
                                ed.plot_dirty = false;
                            }
                        }

                        // Error display
                        if (ed.has_error) {
                            ImGui::TextColored(ImVec4(1.f, 0.2f, 0.2f, 1.f),
                                "Error: %s", ed.compile_error.c_str());
                        }

                        // Live preview plot
                        ImGui::PlotLines("##plot", ed.plot_y,
                            ExprEditorState::kPlotSamples, 0,
                            "Preview (x: 0->1)",
                            FLT_MAX, FLT_MAX,
                            ImVec2(-1.f, 60.f));

                        // Variable palette
                        ImGui::TextDisabled("Variables:");
                        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2.f, 2.f));
                        for (int vi = 0; vi < kVarCount; ++vi) {
                            if (vi > 0) ImGui::SameLine(0.f, 2.f);
                            ImGui::PushID(vi);
                            if (ImGui::SmallButton(kVarNames[vi])) {
                                // Append variable name to text buffer
                                std::size_t cur_len = std::strlen(ed.text);
                                std::size_t var_len = std::strlen(kVarNames[vi]);
                                if (cur_len + var_len + 1 < sizeof(ed.text)) {
                                    std::strncat(ed.text, kVarNames[vi],
                                        sizeof(ed.text) - cur_len - 1);
                                    ed.plot_dirty = true;
                                }
                            }
                            ImGui::PopID();
                        }
                        ImGui::PopStyleVar();

                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::PopID();
                    }

                    ImGui::EndTabItem();
                }
            }

            // ── Tab: ArtNet Output ───────────────────────────────────────────
            if (ImGui::BeginTabItem("ArtNet Out##insp")) {
                ImGui::Separator();
                ImGui::TextDisabled("ArtNet DMX Output");
                ImGui::Spacing();

                // Persistent state for this tab — initialized from project on first use.
                static char  s_artnet_ip[64]   = "2.255.255.255";
                static int   s_artnet_univ_off  = 0;
                static bool  s_artnet_enabled   = false;
                {
                    static bool s_artnet_init = false;
                    if (!s_artnet_init) {
                        s_artnet_init = true;
                        std::strncpy(s_artnet_ip,
                                     state.output_config.artnet_out_ip.c_str(),
                                     sizeof(s_artnet_ip) - 1);
                        s_artnet_ip[sizeof(s_artnet_ip) - 1] = '\0';
                        s_artnet_univ_off = state.output_config.artnet_out_universe_offset;
                        s_artnet_enabled  = state.output_config.artnet_out_enabled;
                    }
                }

                bool changed = false;

                ImGui::PushItemWidth(-120.f);

                // Target IP
                ImGui::TextDisabled("Target IP:");
                changed |= ImGui::InputText("##ao_ip", s_artnet_ip,
                                            sizeof(s_artnet_ip));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Unicast IP or 2.255.255.255 for broadcast");

                // Universe offset
                ImGui::TextDisabled("Universe Offset:");
                changed |= ImGui::InputInt("##ao_univ", &s_artnet_univ_off);
                if (s_artnet_univ_off < 0) s_artnet_univ_off = 0;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Added to internal universe index 0");

                ImGui::PopItemWidth();

                // Enable checkbox
                ImGui::Spacing();
                bool en = s_artnet_enabled;
                if (ImGui::Checkbox("Enable ArtNet Output", &en)) {
                    s_artnet_enabled = en;
                    changed = true;
                }

                // Fire callback whenever any setting changes
                if (changed && cbs.on_artnet_out_config) {
                    cbs.on_artnet_out_config(
                        std::string(s_artnet_ip),
                        s_artnet_univ_off,
                        s_artnet_enabled);
                }

                ImGui::Spacing();
                ImGui::Separator();

                // Status display
                ImGui::TextDisabled("Status:");
                ImGui::SameLine();
                if (s_artnet_enabled) {
                    ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.3f, 1.f), "Sending");
                } else {
                    ImGui::TextDisabled("Disabled");
                }

                ImGui::EndTabItem();
            }

            // ── Tab: Chaser ──────────────────────────────────────────────────
            {
                int sel = ctx.cuelist_selected_idx;
                bool show_chaser_tab = (sel >= 0 && sel < (int)state.full_cue_list.size()
                                        && state.full_cue_list[sel].is_chaser);
                if (show_chaser_tab) {
                    if (ImGui::BeginTabItem("Chaser##insp")) {
                        FullCueEntry edited = state.full_cue_list[sel]; // local copy to edit

                        ImGui::TextColored(theme::accent(), "Chaser: %s", edited.name.c_str());
                        ImGui::Separator();

                        // -- Global settings --
                        ImGui::TextDisabled("Global Settings");
                        ImGui::PushItemWidth(160.f);

                        bool dirty = false;

                        dirty |= DragTimingFloat("Hold##ch_hold",
                            &edited.chaser_global_hold, 0.01f, 0.01f, 10.f, state.bpm);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Default hold time per step in seconds");

                        dirty |= DragTimingFloat("Crossfade##ch_xfade",
                            &edited.chaser_global_xfade, 0.01f, 0.f, 2.f, state.bpm);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Default crossfade duration between steps");

                        dirty |= ImGui::Checkbox("Beat Sync##ch_bs", &edited.chaser_beat_sync);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Sync step advance to engine BPM");

                        {
                            static const char* kBeatDivNames[] = { "Bar (1)", "Half (2)", "Quarter (4)", "8th (8)", "16th (16)" };
                            static const int   kBeatDivVals[]  = { 1, 2, 4, 8, 16 };
                            static constexpr int kBeatDivCount = 5;
                            int cur_div_idx = 2; // default Quarter
                            for (int d = 0; d < kBeatDivCount; ++d) {
                                if (kBeatDivVals[d] == edited.chaser_beat_div) { cur_div_idx = d; break; }
                            }
                            if (ImGui::Combo("Beat Div##ch_bdiv", &cur_div_idx,
                                             kBeatDivNames, kBeatDivCount)) {
                                edited.chaser_beat_div = kBeatDivVals[cur_div_idx];
                                dirty = true;
                            }
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip("Beat subdivision for beat-synced advance");
                        }

                        ImGui::PopItemWidth();
                        ImGui::Separator();

                        // -- Step list --
                        ImGui::TextDisabled("Steps (%d)", (int)edited.chaser_steps.size());

                        if (ImGui::BeginTable("##chaser_steps", 5,
                                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                                ImVec2(-1.f, 160.f)))
                        {
                            ImGui::TableSetupColumn("#",     ImGuiTableColumnFlags_WidthFixed, 24.f);
                            ImGui::TableSetupColumn("Cue",   ImGuiTableColumnFlags_WidthStretch);
                            ImGui::TableSetupColumn("Hold",  ImGuiTableColumnFlags_WidthFixed, 70.f);
                            ImGui::TableSetupColumn("Beat",  ImGuiTableColumnFlags_WidthFixed, 36.f);
                            ImGui::TableSetupColumn("Del",   ImGuiTableColumnFlags_WidthFixed, 24.f);
                            ImGui::TableHeadersRow();

                            int remove_idx = -1;
                            for (int si = 0; si < (int)edited.chaser_steps.size(); ++si) {
                                ChaserStep& step = edited.chaser_steps[si];
                                ImGui::TableNextRow();
                                ImGui::PushID(si);

                                // # column
                                ImGui::TableSetColumnIndex(0);
                                ImGui::TextDisabled("%d", si + 1);

                                // Cue column — combo of all project cues
                                ImGui::TableSetColumnIndex(1);
                                ImGui::PushItemWidth(-1.f);
                                {
                                    int cur_ref = step.cue_ref_idx;
                                    const char* cur_name = (cur_ref >= 0 && cur_ref < (int)state.cues.size())
                                        ? state.cues[cur_ref].name.c_str() : "(none)";
                                    if (ImGui::BeginCombo("##sc_cue", cur_name)) {
                                        for (int ci = 0; ci < (int)state.cues.size(); ++ci) {
                                            bool sel2 = (ci == cur_ref);
                                            if (ImGui::Selectable(state.cues[ci].name.c_str(), sel2)) {
                                                step.cue_ref_idx = ci;
                                                dirty = true;
                                            }
                                            if (sel2) ImGui::SetItemDefaultFocus();
                                        }
                                        ImGui::EndCombo();
                                    }
                                }
                                ImGui::PopItemWidth();

                                // Hold override column
                                ImGui::TableSetColumnIndex(2);
                                ImGui::PushItemWidth(-1.f);
                                if (DragTimingFloat("##sc_hold", &step.hold_s, 0.01f, -1.f, 10.f, state.bpm)) {
                                    dirty = true;
                                }
                                if (ImGui::IsItemHovered())
                                    ImGui::SetTooltip("-1 = use global hold");
                                ImGui::PopItemWidth();

                                // Beat sync override column
                                ImGui::TableSetColumnIndex(3);
                                if (ImGui::Checkbox("##sc_beat", &step.beat_sync)) dirty = true;

                                // Delete column
                                ImGui::TableSetColumnIndex(4);
                                if (ImGui::SmallButton("X##sc_del")) {
                                    remove_idx = si;
                                }

                                ImGui::PopID();
                            }

                            ImGui::EndTable();

                            if (remove_idx >= 0) {
                                edited.chaser_steps.erase(edited.chaser_steps.begin() + remove_idx);
                                dirty = true;
                            }
                        }

                        // Add Step button
                        if (ImGui::Button("Add Step##ch_add")) {
                            ChaserStep ns;
                            ns.cue_ref_idx = 0;
                            edited.chaser_steps.push_back(ns);
                            dirty = true;
                        }

                        // Fire update callback on any change
                        if (dirty && cbs.on_cuelist_update_entry) {
                            cbs.on_cuelist_update_entry(sel, edited);
                        }

                        ImGui::EndTabItem();
                    }
                }
            }

            ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Timeline + DMX Monitor panel
// ─────────────────────────────────────────────────────────────────────────────
void panel_timeline(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    ImGui::SetNextWindowSize(ImVec2(800.f, 400.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.f, 160.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        ImGui::ColorConvertFloat4ToU32(theme::background()));

    bool tl_visible = ImGui::Begin(kWinTimeline, &ctx.cuestack_open);
    if (!ctx.cuestack_open) {
        // X button was just clicked — reset cuestack selection too
        ctx.cuestack_selected_pb = -1;
    }
    if (!tl_visible) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    // Cuestack view — shown when a playback is selected via double-click
    if (ctx.cuestack_selected_pb >= 0) {
        const UIState::PlaybackSnap* pb_ptr = nullptr;
        for (int pi = 0; pi < state.snap.playback_count; ++pi) {
            if (state.snap.playbacks[pi].id == ctx.cuestack_selected_pb) {
                pb_ptr = &state.snap.playbacks[pi];
                break;
            }
        }
        const int pb_id = ctx.cuestack_selected_pb;

        // ── Header ──────────────────────────────────────────────────────────
        {
            char title_buf[64];
            if (pb_ptr && !pb_ptr->name.empty())
                std::snprintf(title_buf, sizeof(title_buf), "Cuestack  %s", pb_ptr->name.c_str());
            else
                std::snprintf(title_buf, sizeof(title_buf), "Cuestack  PB %d", pb_id);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::accent()));
            ImGui::TextUnformatted(title_buf);
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 60.f);
        if (ImGui::Button("Close##cs_close")) {
            ctx.cuestack_selected_pb = -1;
            ctx.cuestack_open = false;
        }

        // ── Transport ────────────────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.55f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.70f, 0.15f, 1.f));
        if (ImGui::Button("  GO  ##cs_go")) {
            if (cbs.on_playback_go) cbs.on_playback_go(pb_id);
        }
        ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("Advance to the next cue in this playback");
        ImGui::SameLine(0, 4);
        if (ImGui::Button("  STP  ##cs_stp")) {
            if (cbs.on_playback_stop) cbs.on_playback_stop(pb_id);
        }
        ImGui::SetItemTooltip("Stop this playback and release its output");
        ImGui::SameLine(0, 4);
        // << button: step back to previous cue in this playback
        ImGui::BeginDisabled(!cbs.on_playback_cue_back);
        if (ImGui::Button("  <<  ##cs_back")) {
            if (cbs.on_playback_cue_back) cbs.on_playback_cue_back(pb_id);
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Step back to the previous cue in this playback");
        ImGui::SameLine(0, 12);
        if (pb_ptr && !pb_ptr->current_cue_name.empty()) {
            char cinfo[64];
            std::snprintf(cinfo, sizeof(cinfo), "Playing: %s", pb_ptr->current_cue_name.c_str());
            ImGui::TextColored(theme::accent(), "%s", cinfo);
        } else {
            ImGui::TextDisabled("Stopped");
        }

        ImGui::Separator();

        // ── Cue table ────────────────────────────────────────────────────────
        const auto& cuelist = state.pb_cuelist;

        if (cuelist.empty()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No cues recorded yet. Draw shapes and press REC.");
            ImGui::End();
            ImGui::PopStyleColor();
            return;
        }

        ImGuiTableFlags tflags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                 ImGuiTableFlags_SizingStretchProp;

        if (ImGui::BeginTable("##cstable", 8, tflags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#",        ImGuiTableColumnFlags_WidthFixed,   28.f);
            ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn("Fade In",  ImGuiTableColumnFlags_WidthStretch, 2.f);
            ImGui::TableSetupColumn("Hold",     ImGuiTableColumnFlags_WidthStretch, 2.f);
            ImGui::TableSetupColumn("Fade Out", ImGuiTableColumnFlags_WidthStretch, 2.f);
            ImGui::TableSetupColumn("Wait",     ImGuiTableColumnFlags_WidthStretch, 2.f);
            ImGui::TableSetupColumn("Trig",     ImGuiTableColumnFlags_WidthFixed,   50.f);
            ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   36.f);
            ImGui::TableHeadersRow();

            for (int ci = 0; ci < static_cast<int>(cuelist.size()); ++ci) {
                ImGui::PushID(ci);
                const FullCueEntry& fce = cuelist[static_cast<size_t>(ci)];
                bool is_current = pb_ptr && pb_ptr->current_cue == ci;

                ImGui::TableNextRow(0, 24.f);

                // Highlight the currently playing row (must be after TableNextRow)
                if (is_current) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::ColorConvertFloat4ToU32(
                            ImVec4(theme::accent().x * 0.22f,
                                   theme::accent().y * 0.22f,
                                   theme::accent().z * 0.22f, 1.f)));
                }

                // Col 0: cue number
                ImGui::TableSetColumnIndex(0);
                char num_buf[8];
                std::snprintf(num_buf, sizeof(num_buf), "%d", fce.number.major);
                if (is_current)
                    ImGui::TextColored(theme::accent(), "%s", num_buf);
                else
                    ImGui::TextUnformatted(num_buf);

                // Click row to jump
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (cbs.on_playback_cue_jump) cbs.on_playback_cue_jump(pb_id, ci);
                }

                // Col 1: name — single-click to INCL, double-click to jump
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(fce.name.c_str());
                if (ImGui::IsItemHovered()) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        if (cbs.on_playback_cue_jump) cbs.on_playback_cue_jump(pb_id, ci);
                    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ctx.incl_armed) {
                        // INCL + click: load this playback cue into programmer
                        ctx.included_pb_id      = pb_id;
                        ctx.included_pb_cue_idx = ci;
                        ctx.included_cue_idx    = -1;   // not a main cuelist include
                        ctx.incl_armed          = false;
                        if (!fce.keyframe_layer.objects.empty())
                            ctx.frame_editor.objects = fce.keyframe_layer.objects;
                        ctx.programmer_global    = fce.global_layer;
                        ctx.programmer_fx_layer  = fce.fx_layer;
                        for (const auto& [sid, sfx] : fce.per_stream_fx) {
                            auto& feed = ctx.programmer_feeds[std::to_string(sid)];
                            feed.fx = sfx;
                        }
                        if (cbs.on_include_playback_cue)
                            cbs.on_include_playback_cue(pb_id, ci);
                    }
                }

                // Col 2: Fade In
                ImGui::TableSetColumnIndex(2);
                FullCueEntry edited = fce;
                bool dirty = false;
                ImGui::SetNextItemWidth(-1.f);
                if (DragTimingFloat("##fi",   &edited.timing.fade_in,  0.01f, 0.f,   60.f, state.bpm)) dirty = true;

                // Col 3: Hold
                ImGui::TableSetColumnIndex(3);
                ImGui::SetNextItemWidth(-1.f);
                if (DragTimingFloat("##hld",  &edited.timing.hold,     0.01f, 0.f,  600.f, state.bpm)) dirty = true;

                // Col 4: Fade Out
                ImGui::TableSetColumnIndex(4);
                ImGui::SetNextItemWidth(-1.f);
                if (DragTimingFloat("##fo",   &edited.timing.fade_out,  0.01f, 0.f,  60.f, state.bpm)) dirty = true;

                // Col 5: Wait (auto-advance)
                ImGui::TableSetColumnIndex(5);
                ImGui::SetNextItemWidth(-1.f);
                if (DragTimingFloat("##wait", &edited.timing.wait,      0.01f, 0.f, 600.f, state.bpm)) dirty = true;

                if (dirty && cbs.on_playback_cue_update)
                    cbs.on_playback_cue_update(pb_id, ci, edited);

                // Col 6: Trigger mode toggle
                ImGui::TableSetColumnIndex(6);
                {
                    bool is_follow = (fce.trigger.type == TriggerType::Follow);
                    const char* trig_label = is_follow ? "PLAY" : "HALT";
                    ImVec4 trig_col = is_follow ? ImVec4(0.05f, 0.50f, 0.10f, 1.f)
                                                : ImVec4(0.30f, 0.30f, 0.35f, 1.f);
                    ImGui::PushStyleColor(ImGuiCol_Button, trig_col);
                    if (ImGui::Button(trig_label, ImVec2(-1.f, 0.f))) {
                        FullCueEntry toggled = fce;
                        toggled.trigger.type  = is_follow ? TriggerType::Halt : TriggerType::Follow;
                        toggled.trigger.time_s = 0.f;
                        if (cbs.on_playback_cue_update) cbs.on_playback_cue_update(pb_id, ci, toggled);
                    }
                    ImGui::PopStyleColor();
                    ImGui::SetItemTooltip(
                        is_follow
                        ? "PLAY — cue auto-advances to next; click to set HALT"
                        : "HALT — cue waits for GO command; click to set auto-PLAY");
                }

                // Col 7: Delete (two-step confirm to prevent accidental deletion)
                ImGui::TableSetColumnIndex(7);
                {
                    static int s_del_confirm_ci = -1;
                    bool confirming = (s_del_confirm_ci == ci);
                    if (!confirming) {
                        ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0.30f, 0.08f, 0.08f, 1.f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.55f, 0.10f, 0.10f, 1.f));
                        if (ImGui::Button("DEL##cs_del", ImVec2(-1.f, 0.f)))
                            s_del_confirm_ci = ci;
                        ImGui::PopStyleColor(2);
                        ImGui::SetItemTooltip("Delete this cue from the stack (click once to confirm)");
                    } else {
                        ImGui::PushStyleColor(ImGuiCol_Button,
                            ImVec4(0.75f, 0.12f, 0.08f, 1.f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                            ImVec4(0.90f, 0.18f, 0.10f, 1.f));
                        if (ImGui::Button("OK?##cs_delok", ImVec2(-1.f, 0.f))) {
                            if (cbs.on_playback_cue_delete) cbs.on_playback_cue_delete(pb_id, ci);
                            s_del_confirm_ci = -1;
                        }
                        ImGui::PopStyleColor(2);
                        ImGui::SetItemTooltip("Click again to confirm deletion — this cannot be undone");
                        // Cancel on any other click outside
                        if (!ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            s_del_confirm_ci = -1;
                    }
                }

                ImGui::PopID();
            }

            ImGui::EndTable();
        }

        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    // Normal timeline view (no cuestack selected)
    if (ImGui::BeginTabBar("##tl_tabs")) {

            // ── Tab: Timeline ────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Timeline")) {
                float avail_w = ImGui::GetContentRegionAvail().x;

                // --- Zoom controls --------------------------------------------
                ImGui::TextDisabled("Zoom:");
                ImGui::SameLine(0, 4);
                ImGui::PushItemWidth(100.f);
                ImGui::SliderFloat("##zoom", &ctx.timeline_zoom, 10.f, 300.f, "%.0f px/s");
                ImGui::PopItemWidth();
                ImGui::SetItemTooltip(
                    "Horizontal zoom — pixels per second\n"
                    "10 px/s = very zoomed out  |  300 px/s = very detailed");
                ImGui::SameLine(0, 8);
                char total_label[32];
                std::snprintf(total_label, sizeof(total_label), "%.1f s total", state.loop_end_s);
                ImGui::TextDisabled("%s", total_label);
                ImGui::SetItemTooltip("Total loop region length in seconds");
                ImGui::SameLine(0, 16);
                // Playhead time display (MM:SS.FF)
                {
                    int mins    = (int)(state.playhead_s / 60.0);
                    double secs = std::fmod(state.playhead_s, 60.0);
                    char phtime[32];
                    std::snprintf(phtime, sizeof(phtime), "%02d:%05.2f", mins, secs);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::ColorConvertFloat4ToU32(theme::accent()));
                    ImGui::TextUnformatted(phtime);
                    ImGui::PopStyleColor();
                    ImGui::SetItemTooltip("Current playhead position (MM:SS.ff)");
                }

                ImGui::Separator();

                // --- Timeline ruler -------------------------------------------
                float ruler_h = 28.f;
                double new_ph = widgets::TimelineRuler("##ruler",
                                                       state.playhead_s,
                                                       state.loop_end_s,
                                                       avail_w, ruler_h,
                                                       ctx.timeline_zoom,
                                                       state.bpm);
                if (new_ph >= 0.0) {
                    state.playhead_s = new_ph;
                    if (cbs.on_seek) cbs.on_seek(new_ph);
                }

                // --- Cue blocks -----------------------------------------------
                float lane_h  = 30.f;
                float block_y = ImGui::GetCursorScreenPos().y;
                ImDrawList* dl = ImGui::GetWindowDrawList();

                // Empty-state placeholder when no cues are placed
                if (state.timeline_cues.empty()) {
                    float ph = ImGui::GetContentRegionAvail().y * 0.5f - ImGui::GetTextLineHeight();
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ph);
                    float tw = ImGui::CalcTextSize("No cues on timeline — drag from Cue Library").x;
                    ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - tw) * 0.5f);
                    ImGui::TextDisabled("No cues on timeline — drag from Cue Library");
                }

                // Determine number of lanes
                int max_track = 0;
                for (const auto& tc : state.timeline_cues)
                    max_track = std::max(max_track, tc.track);
                int num_tracks = std::max(max_track + 1, 2);

                // Draw lane backgrounds
                ImVec2 lane_origin = ImGui::GetCursorScreenPos();
                for (int tr = 0; tr < num_tracks; ++tr) {
                    ImU32 lane_bg = (tr % 2 == 0)
                        ? IM_COL32(22, 25, 33, 255)
                        : IM_COL32(18, 20, 28, 255);
                    dl->AddRectFilled(
                        ImVec2(lane_origin.x, block_y + tr * (lane_h + 2.f)),
                        ImVec2(lane_origin.x + avail_w,
                               block_y + tr * (lane_h + 2.f) + lane_h),
                        lane_bg);
                }

                // Draw cue blocks
                for (const auto& tc : state.timeline_cues) {
                    float bx = lane_origin.x + (float)(tc.start_s * ctx.timeline_zoom);
                    float bw = (float)(tc.duration_s * ctx.timeline_zoom);
                    float by = block_y + tc.track * (lane_h + 2.f);

                    ImVec4 block_col = { 0.f, 0.898f, 1.f, 0.7f };
                    for (const auto& ci : state.cues) {
                        if (ci.index == tc.cue_index) {
                            block_col = { ci.card_color.r, ci.card_color.g,
                                          ci.card_color.b, 0.7f };
                            break;
                        }
                    }

                    bool is_active = (tc.cue_index == state.active_cue_idx);
                    dl->AddRectFilled(ImVec2(bx, by + 2.f),
                                      ImVec2(bx + bw - 1.f, by + lane_h - 2.f),
                                      ImGui::ColorConvertFloat4ToU32(block_col),
                                      theme::kSmallRounding);
                    if (is_active) {
                        dl->AddRect(ImVec2(bx, by + 2.f),
                                    ImVec2(bx + bw - 1.f, by + lane_h - 2.f),
                                    ImGui::ColorConvertFloat4ToU32(theme::accent()),
                                    theme::kSmallRounding, 0, 1.5f);
                    }

                    if (bw > 20.f) {
                        std::string label;
                        for (const auto& ci : state.cues)
                            if (ci.index == tc.cue_index) { label = ci.name; break; }
                        dl->AddText(ImVec2(bx + 4.f, by + 8.f),
                                    IM_COL32(20, 20, 28, 220), label.c_str());
                    }
                }

                // Beat grid lines — subtle vertical lines at each beat position
                {
                    if (state.bpm > 1.f) {
                        float beat_s   = 60.f / state.bpm;
                        float total_lane_h = num_tracks * (lane_h + 2.f);
                        float visible_w = avail_w;
                        float t_max    = visible_w / ctx.timeline_zoom;
                        ImU32 beat_col = IM_COL32(0, 230, 255, 25);  // kAccentDim at 0.10
                        ImU32 bar_col  = IM_COL32(0, 230, 255, 50);  // kAccentDim stronger for bars
                        int   beat_num = 0;
                        for (float t = 0.f; t < t_max; t += beat_s, ++beat_num) {
                            float bx = lane_origin.x + t * ctx.timeline_zoom;
                            ImU32 col = (beat_num % 4 == 0) ? bar_col : beat_col;
                            dl->AddLine(ImVec2(bx, block_y),
                                        ImVec2(bx, block_y + total_lane_h),
                                        col, 1.f);
                        }
                    }
                }

                // Playhead line over blocks — 2px kAccent
                {
                    float ph_x = lane_origin.x +
                                 (float)(state.playhead_s * ctx.timeline_zoom);
                    float total_lane_h = num_tracks * (lane_h + 2.f);
                    dl->AddLine(ImVec2(ph_x, block_y),
                                ImVec2(ph_x, block_y + total_lane_h),
                                ImGui::ColorConvertFloat4ToU32(theme::accent()), 2.f);
                }

                // Advance cursor past lanes
                ImGui::Dummy(ImVec2(avail_w, num_tracks * (lane_h + 2.f)));

                // --- DMX Activity bar -----------------------------------------
                ImGui::Separator();
                ImGui::TextDisabled("DMX Activity");

                float dmx_bar_h = 16.f;
                float dmx_ch_w  = avail_w / 512.f;

                // Universe 0
                {
                    ImVec2 dmx_pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(dmx_pos,
                                      ImVec2(dmx_pos.x + avail_w, dmx_pos.y + dmx_bar_h),
                                      IM_COL32(13, 15, 18, 255));
                    for (int ch = 0; ch < 512; ++ch) {
                        uint8_t v = state.dmx_uni_0[ch + 1];
                        if (v > 0) {
                            float  bx2 = dmx_pos.x + ch * dmx_ch_w;
                            float  bh  = v / 255.f * dmx_bar_h;
                            ImVec4 bc  = theme::accent();
                            dl->AddRectFilled(
                                ImVec2(bx2, dmx_pos.y + dmx_bar_h - bh),
                                ImVec2(bx2 + dmx_ch_w, dmx_pos.y + dmx_bar_h),
                                ImGui::ColorConvertFloat4ToU32(bc));
                        }
                    }
                    ImGui::Dummy(ImVec2(avail_w, dmx_bar_h));
                }

                // Universe 1
                {
                    ImVec2 dmx_pos = ImGui::GetCursorScreenPos();
                    dl->AddRectFilled(dmx_pos,
                                      ImVec2(dmx_pos.x + avail_w, dmx_pos.y + dmx_bar_h),
                                      IM_COL32(13, 15, 18, 255));
                    for (int ch = 0; ch < 512; ++ch) {
                        uint8_t v = state.dmx_uni_1[ch + 1];
                        if (v > 0) {
                            float bx2 = dmx_pos.x + ch * dmx_ch_w;
                            float bh  = v / 255.f * dmx_bar_h;
                            dl->AddRectFilled(
                                ImVec2(bx2, dmx_pos.y + dmx_bar_h - bh),
                                ImVec2(bx2 + dmx_ch_w, dmx_pos.y + dmx_bar_h),
                                ImGui::ColorConvertFloat4ToU32(theme::accent_warm()));
                        }
                    }
                    ImGui::Dummy(ImVec2(avail_w, dmx_bar_h));
                }

                ImGui::EndTabItem();
            } // Timeline tab

            // ── Tab: Cue Sheet ────────────────────────────────────────────────
            if (ImGui::BeginTabItem("Cue Sheet")) {

                // ── Top toolbar ──────────────────────────────────────────────
                {
                    bool rec = state.record_mode;
                    if (rec) {
                        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.78f,0.08f,0.08f,1.f));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f,0.15f,0.15f,1.f));
                    }
                    if (ImGui::Button(rec ? "REC ON" : "Record", ImVec2(70.f, 0.f)))
                        if (cbs.on_set_record_mode) cbs.on_set_record_mode(!rec);
                    if (rec) ImGui::PopStyleColor(2);
                    ImGui::SetItemTooltip(
                        rec ? "Record mode ON — output is being captured; click to stop"
                            : "Enable record mode to capture programmer output into cues");

                    ImGui::SameLine(0, 6);
                    if (ImGui::Button("+ Add Cue", ImVec2(80.f, 0.f)))
                        if (cbs.on_record_cue) cbs.on_record_cue("New Cue", 2.f, 2.f);
                    ImGui::SetItemTooltip("Create a new empty cue at the end of the list (2 s fade in/out)");

                    ImGui::SameLine(0, 8);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine(0, 8);

                    bool has_sel = (ctx.cuelist_selected_idx >= 0
                                    && ctx.cuelist_selected_idx < (int)state.cues.size());
                    if (!has_sel) ImGui::BeginDisabled();

                    // Delete — two-step confirm
                    {
                        static bool s_del_confirm = false;
                        if (!s_del_confirm) {
                            if (ImGui::Button("Delete", ImVec2(58.f, 0.f)))
                                s_del_confirm = true;
                            ImGui::SetItemTooltip("Delete the selected cue (click to confirm)");
                        } else {
                            ImGui::PushStyleColor(ImGuiCol_Button,
                                ImVec4(0.78f, 0.08f, 0.08f, 1.f));
                            if (ImGui::Button("Confirm?", ImVec2(58.f, 0.f))) {
                                if (cbs.on_delete_cue) cbs.on_delete_cue(ctx.cuelist_selected_idx);
                                ctx.cuelist_selected_idx = -1;
                                s_del_confirm = false;
                            }
                            ImGui::PopStyleColor();
                            ImGui::SetItemTooltip("Click again to permanently delete this cue");
                            if (!has_sel) s_del_confirm = false;
                        }
                    }

                    ImGui::SameLine(0, 4);
                    if (ImGui::Button("Duplicate", ImVec2(72.f, 0.f)))
                        if (has_sel && cbs.on_cue_duplicate) cbs.on_cue_duplicate(ctx.cuelist_selected_idx);
                    ImGui::SetItemTooltip("Duplicate the selected cue and insert it after");
                    if (!has_sel) ImGui::EndDisabled();

                    ImGui::SameLine(0, 8);
                    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
                    ImGui::SameLine(0, 8);

                    const char* props_lbl = ctx.cuelist_show_properties ? "Props [ON]" : "Properties";
                    if (ImGui::Button(props_lbl, ImVec2(84.f, 0.f)))
                        ctx.cuelist_show_properties = !ctx.cuelist_show_properties;
                    ImGui::SetItemTooltip(
                        ctx.cuelist_show_properties
                        ? "Properties panel visible — click to hide"
                        : "Show timing and trigger properties for the selected cue");
                }

                ImGui::Separator();

                float cs_props_w  = ctx.cuelist_show_properties ? 210.f : 0.f;
                float cs_avail_w  = ImGui::GetContentRegionAvail().x;
                float cs_table_w  = cs_avail_w - cs_props_w - (cs_props_w > 0.f ? 6.f : 0.f);
                float cs_avail_h  = ImGui::GetContentRegionAvail().y;

                // ── Main cue table ───────────────────────────────────────────
                ImGui::BeginChild("##cs_table_area", ImVec2(cs_table_w, cs_avail_h), ImGuiChildFlags_None);

                static constexpr ImGuiTableFlags kTblFlags =
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_ScrollY       | ImGuiTableFlags_RowBg;

                if (ImGui::BeginTable("##cue_sheet_tbl", 9, kTblFlags)) {
                    ImGui::TableSetupScrollFreeze(0, 2); // freeze header + Set All row
                    ImGui::TableSetupColumn("Cue#",     ImGuiTableColumnFlags_WidthFixed,   46.f);
                    ImGui::TableSetupColumn("Name",     ImGuiTableColumnFlags_WidthStretch,  1.f);
                    ImGui::TableSetupColumn("Fade In",  ImGuiTableColumnFlags_WidthFixed,   70.f);
                    ImGui::TableSetupColumn("Fade Out", ImGuiTableColumnFlags_WidthFixed,   70.f);
                    ImGui::TableSetupColumn("Delay In", ImGuiTableColumnFlags_WidthFixed,   66.f);
                    ImGui::TableSetupColumn("Hold",     ImGuiTableColumnFlags_WidthFixed,   58.f);
                    ImGui::TableSetupColumn("Trigger",  ImGuiTableColumnFlags_WidthFixed,   68.f);
                    ImGui::TableSetupColumn("State",    ImGuiTableColumnFlags_WidthFixed,   58.f);
                    ImGui::TableSetupColumn("Notes",    ImGuiTableColumnFlags_WidthStretch, 0.8f);
                    ImGui::TableHeadersRow();

                    // Delay/Hold per-cue arrays (hoisted so Set All row can write them)
                    static float s_di[1024]{};
                    static float s_hld[1024]{};

                    // ── Set All row: drag a value to apply it to every cue ──────
                    {
                        static float s_sa_fi  = 0.f;
                        static float s_sa_fo  = 0.f;
                        static float s_sa_di  = 0.f;
                        static float s_sa_hld = 0.f;

                        ImGui::TableNextRow();
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(80, 60, 8, 110));
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(80, 60, 8, 110));
                        ImGui::PushID("##sa");

                        ImGui::TableSetColumnIndex(0);
                        ImGui::TextDisabled("Set All");

                        // Fade In (col 2)
                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::DragFloat("##sa_fi", &s_sa_fi, 0.05f, 0.f, 60.f, "%.2f s")) {
                            for (int k = 0; k < (int)state.cues.size(); ++k)
                                if (cbs.on_set_cue_timing)
                                    cbs.on_set_cue_timing(k, s_sa_fi,
                                        state.cues[k].fade_out,
                                        (k < 1024) ? s_di[k]  : 0.f,
                                        (k < 1024) ? s_hld[k] : 0.f);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set Fade In for ALL cues");

                        // Fade Out (col 3)
                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::DragFloat("##sa_fo", &s_sa_fo, 0.05f, 0.f, 60.f, "%.2f s")) {
                            for (int k = 0; k < (int)state.cues.size(); ++k)
                                if (cbs.on_set_cue_timing)
                                    cbs.on_set_cue_timing(k, state.cues[k].fade_in,
                                        s_sa_fo,
                                        (k < 1024) ? s_di[k]  : 0.f,
                                        (k < 1024) ? s_hld[k] : 0.f);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set Fade Out for ALL cues");

                        // Delay In (col 4)
                        ImGui::TableSetColumnIndex(4);
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::DragFloat("##sa_di", &s_sa_di, 0.05f, 0.f, 60.f, "%.2f s")) {
                            for (int k = 0; k < (int)state.cues.size() && k < 1024; ++k)
                                s_di[k] = s_sa_di;
                            for (int k = 0; k < (int)state.cues.size(); ++k)
                                if (cbs.on_set_cue_timing)
                                    cbs.on_set_cue_timing(k, state.cues[k].fade_in,
                                        state.cues[k].fade_out,
                                        (k < 1024) ? s_di[k] : s_sa_di,
                                        (k < 1024) ? s_hld[k] : 0.f);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set Delay In for ALL cues");

                        // Hold (col 5)
                        ImGui::TableSetColumnIndex(5);
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::DragFloat("##sa_hld", &s_sa_hld, 0.05f, 0.f, 600.f, "%.2f s")) {
                            for (int k = 0; k < (int)state.cues.size() && k < 1024; ++k)
                                s_hld[k] = s_sa_hld;
                            for (int k = 0; k < (int)state.cues.size(); ++k)
                                if (cbs.on_set_cue_timing)
                                    cbs.on_set_cue_timing(k, state.cues[k].fade_in,
                                        state.cues[k].fade_out,
                                        (k < 1024) ? s_di[k]  : 0.f,
                                        (k < 1024) ? s_hld[k] : s_sa_hld);
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Drag to set Hold for ALL cues");

                        ImGui::PopID();
                    }

                    static int  s_editing_name_idx  = -1;
                    static int  s_editing_notes_idx = -1;
                    static char s_name_buf[128]     = {};
                    static char s_notes_buf[256]    = {};

                    // Empty-state row when list is empty
                    if (state.cues.empty()) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(1);
                        ImGui::TextDisabled("No cues — click \"+ Add Cue\" or enable Record mode");
                    }

                    for (int i = 0; i < (int)state.cues.size(); ++i) {
                        const CueInfo& ci = state.cues[i];
                        bool is_current   = (state.cuelist_current_idx == i);
                        bool is_next      = (!is_current && state.cuelist_current_idx >= 0
                                             && state.cuelist_current_idx + 1 == i);
                        bool is_selected  = (ctx.cuelist_selected_idx == i);

                        ImGui::TableNextRow();

                        if (is_current) {
                            ImVec4 acc = theme::accent();
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                IM_COL32((int)(acc.x*255),(int)(acc.y*255),(int)(acc.z*255),55));
                        } else if (is_next) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(90,85,10,60));
                        }

                        ImGui::PushID(i);

                        // Col 0 — Cue#
                        ImGui::TableSetColumnIndex(0);
                        char num_buf[16];
                        std::snprintf(num_buf, sizeof(num_buf), "%d.0", i + 1);
                        bool row_clicked = ImGui::Selectable(num_buf, is_selected,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                            ImVec2(0.f, 0.f));
                        bool row_dbl    = ImGui::IsItemHovered()
                                          && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
                        bool row_rclick = ImGui::IsItemHovered()
                                          && ImGui::IsMouseClicked(ImGuiMouseButton_Right);

                        if (row_clicked) ctx.cuelist_selected_idx = i;
                        if (row_dbl && cbs.on_cuelist_jump) cbs.on_cuelist_jump(i + 1, 0);
                        if (row_rclick) ImGui::OpenPopup("##cs_ctx");

                        // Col 1 — Name (double-click to edit inline)
                        ImGui::TableSetColumnIndex(1);
                        {
                            // Color swatch — derived from thumbnail first lit pixel, or card_color.
                            ImVec4 swatch_col(ci.card_color.r, ci.card_color.g,
                                              ci.card_color.b, 1.f);
                            if (i < (int)state.cue_thumbnails.size()
                                    && state.cue_thumbnails[i].ready) {
                                const auto& pix = state.cue_thumbnails[i].pixels;
                                bool found = false;
                                for (int pi = 0; pi < kThumbPixels && !found; ++pi) {
                                    uint8_t pr = pix[static_cast<size_t>(pi * 4)];
                                    uint8_t pg = pix[static_cast<size_t>(pi * 4 + 1)];
                                    uint8_t pb = pix[static_cast<size_t>(pi * 4 + 2)];
                                    if (pr > 20 || pg > 20 || pb > 20) {
                                        swatch_col = ImVec4(pr / 255.f, pg / 255.f, pb / 255.f, 1.f);
                                        found = true;
                                    }
                                }
                            }
                            char sw_id[24];
                            std::snprintf(sw_id, sizeof(sw_id), "##sw%d", i);
                            ImGui::ColorButton(sw_id, swatch_col,
                                ImGuiColorEditFlags_NoPicker |
                                ImGuiColorEditFlags_NoTooltip |
                                ImGuiColorEditFlags_NoAlpha,
                                ImVec2(12.f, 12.f));
                            ImGui::SameLine(0.f, 4.f);
                        }
                        if (s_editing_name_idx == i) {
                            ImGui::SetNextItemWidth(-1.f);
                            if (ImGui::InputText("##name_edit", s_name_buf, sizeof(s_name_buf),
                                    ImGuiInputTextFlags_EnterReturnsTrue |
                                    ImGuiInputTextFlags_AutoSelectAll)) {
                                if (cbs.on_rename_cue) cbs.on_rename_cue(i, std::string(s_name_buf));
                                s_editing_name_idx = -1;
                            }
                            if (!ImGui::IsItemActive() && !ImGui::IsItemHovered()
                                    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                                s_editing_name_idx = -1;
                        } else {
                            ImGui::TextUnformatted(ci.name.c_str());
                            if (ImGui::IsItemHovered()
                                    && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                s_editing_name_idx = i;
                                std::strncpy(s_name_buf, ci.name.c_str(), sizeof(s_name_buf) - 1);
                                s_name_buf[sizeof(s_name_buf)-1] = '\0';
                            }
                        }

                        // Col 2 — Fade In
                        ImGui::TableSetColumnIndex(2);
                        {
                            float fi = ci.fade_in;
                            ImGui::SetNextItemWidth(-1.f);
                            char fi_id[16]; std::snprintf(fi_id, sizeof(fi_id), "##fi%d", i);
                            if (ImGui::DragFloat(fi_id, &fi, 0.05f, 0.f, 60.f, "%.2f s"))
                                if (cbs.on_set_cue_timing) cbs.on_set_cue_timing(i, fi, ci.fade_out, 0.f, 0.f);
                            ImGui::SetItemTooltip("Fade-in duration in seconds (0 = immediate)");
                        }

                        // Col 3 — Fade Out
                        ImGui::TableSetColumnIndex(3);
                        {
                            float fo = ci.fade_out;
                            ImGui::SetNextItemWidth(-1.f);
                            char fo_id[16]; std::snprintf(fo_id, sizeof(fo_id), "##fo%d", i);
                            if (ImGui::DragFloat(fo_id, &fo, 0.05f, 0.f, 60.f, "%.2f s"))
                                if (cbs.on_set_cue_timing) cbs.on_set_cue_timing(i, ci.fade_in, fo, 0.f, 0.f);
                            ImGui::SetItemTooltip("Fade-out duration in seconds (0 = immediate)");
                        }

                        // Col 4 — Delay In (time before the fade starts)
                        ImGui::TableSetColumnIndex(4);
                        {
                            float& di = (i < 1024) ? s_di[i] : s_di[0];
                            ImGui::SetNextItemWidth(-1.f);
                            char di_id[16]; std::snprintf(di_id, sizeof(di_id), "##di%d", i);
                            if (ImGui::DragFloat(di_id, &di, 0.05f, 0.f, 60.f, "%.2f s"))
                                if (cbs.on_set_cue_timing) cbs.on_set_cue_timing(i, ci.fade_in, ci.fade_out, di, 0.f);
                            ImGui::SetItemTooltip("Delay before fade-in starts (s) — 0 = immediate");
                        }

                        // Col 5 — Hold (time between fade-in end and fade-out start)
                        ImGui::TableSetColumnIndex(5);
                        {
                            float& hld = (i < 1024) ? s_hld[i] : s_hld[0];
                            ImGui::SetNextItemWidth(-1.f);
                            char hld_id[16]; std::snprintf(hld_id, sizeof(hld_id), "##hld%d", i);
                            if (ImGui::DragFloat(hld_id, &hld, 0.05f, 0.f, 600.f, "%.2f s"))
                                if (cbs.on_set_cue_timing) cbs.on_set_cue_timing(i, ci.fade_in, ci.fade_out, 0.f, hld);
                            ImGui::SetItemTooltip("Hold time after fade-in before fade-out begins (s)\n0 = trigger-controlled, >0 = auto-advance after this many seconds");
                        }

                        // Col 6 — Trigger combo
                        ImGui::TableSetColumnIndex(6);
                        {
                            static const char* kTrigNames[] = {
                                "Halt", "Follow", "Wait", "TC", "MIDI", "OSC", "DMX", "Audio"
                            };
                            static constexpr int kTrigCount = 8;
                            int trig_idx = ci.trigger_type;
                            if (trig_idx < 0 || trig_idx >= kTrigCount) trig_idx = 0;
                            ImGui::SetNextItemWidth(-1.f);
                            char trig_id[16]; std::snprintf(trig_id, sizeof(trig_id), "##trig%d", i);
                            if (ImGui::Combo(trig_id, &trig_idx, kTrigNames, kTrigCount)) {
                                // Trigger type editing — callback to be wired when engine supports it
                            }
                            ImGui::SetItemTooltip(
                                "Trigger mode:\n"
                                "Halt = wait for GO  |  Follow = auto-advance\n"
                                "Wait = time-delayed  |  TC/MIDI/OSC/DMX/Audio = external trigger");
                        }

                        // Col 7 — State badge
                        ImGui::TableSetColumnIndex(7);
                        if (is_current) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.1f,0.9f,0.3f,1.f));
                            ImGui::TextUnformatted("ACTIVE");
                            ImGui::PopStyleColor();
                        } else if (is_next) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.9f,0.1f,1.f));
                            ImGui::TextUnformatted("NEXT");
                            ImGui::PopStyleColor();
                        } else {
                            ImGui::TextDisabled("-");
                        }

                        // Col 8 — Notes (double-click to edit)
                        ImGui::TableSetColumnIndex(8);
                        if (s_editing_notes_idx == i) {
                            ImGui::SetNextItemWidth(-1.f);
                            if (ImGui::InputText("##notes_edit", s_notes_buf, sizeof(s_notes_buf),
                                    ImGuiInputTextFlags_EnterReturnsTrue))
                                s_editing_notes_idx = -1;
                            if (!ImGui::IsItemActive() && !ImGui::IsItemHovered()
                                    && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                                s_editing_notes_idx = -1;
                        } else {
                            ImGui::TextDisabled("...");
                            if (ImGui::IsItemHovered()
                                    && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                s_editing_notes_idx = i;
                                s_notes_buf[0] = '\0';
                            }
                        }

                        // Right-click context menu
                        if (ImGui::BeginPopup("##cs_ctx")) {
                            if (ImGui::MenuItem("GO (Jump to)"))
                                if (cbs.on_cuelist_jump) cbs.on_cuelist_jump(i + 1, 0);
                            ImGui::Separator();
                            if (ImGui::MenuItem("Rename")) {
                                s_editing_name_idx = i;
                                std::strncpy(s_name_buf, ci.name.c_str(), sizeof(s_name_buf) - 1);
                                s_name_buf[sizeof(s_name_buf)-1] = '\0';
                            }
                            if (ImGui::MenuItem("Duplicate"))
                                if (cbs.on_cue_duplicate) cbs.on_cue_duplicate(i);
                            if (ImGui::MenuItem("Delete")) {
                                if (cbs.on_delete_cue) cbs.on_delete_cue(i);
                                if (ctx.cuelist_selected_idx == i) ctx.cuelist_selected_idx = -1;
                            }
                            ImGui::Separator();
                            if (ImGui::MenuItem("Properties")) {
                                ctx.cuelist_selected_idx    = i;
                                ctx.cuelist_show_properties = true;
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                    } // for each cue

                    ImGui::EndTable();
                }

                ImGui::EndChild(); // ##cs_table_area

                // ── Properties panel (right side, 200px) ─────────────────────
                if (ctx.cuelist_show_properties) {
                    ImGui::SameLine(0, 6);
                    ImGui::BeginChild("##cs_props", ImVec2(cs_props_w, cs_avail_h), ImGuiChildFlags_Borders);

                    int sel = ctx.cuelist_selected_idx;
                    if (sel >= 0 && sel < (int)state.cues.size()) {
                        const CueInfo& ci = state.cues[sel];

                        ImGui::TextColored(theme::accent(), "Cue %d.0", sel + 1);
                        ImGui::Separator();

                        // Name
                        static char s_prop_name[128] = {};
                        static int  s_prop_name_for  = -1;
                        if (s_prop_name_for != sel) {
                            std::strncpy(s_prop_name, ci.name.c_str(), sizeof(s_prop_name) - 1);
                            s_prop_name[sizeof(s_prop_name)-1] = '\0';
                            s_prop_name_for = sel;
                        }
                        ImGui::PushItemWidth(-1.f);
                        if (ImGui::InputText("##prop_name", s_prop_name, sizeof(s_prop_name),
                                ImGuiInputTextFlags_EnterReturnsTrue |
                                ImGuiInputTextFlags_AutoSelectAll))
                            if (cbs.on_rename_cue) cbs.on_rename_cue(sel, std::string(s_prop_name));
                        ImGui::PopItemWidth();

                        ImGui::TextDisabled("Gen: %s", ci.generator.c_str());
                        ImGui::Separator();
                        ImGui::TextDisabled("Timing");

                        float fi  = ci.fade_in;
                        float fo  = ci.fade_out;
                        float di  = 0.f;
                        float hld = 0.f;
                        float do_ = 0.f;
                        float wt  = 0.f;

                        ImGui::PushItemWidth(-1.f);
                        bool t_ch = false;
                        t_ch |= DragTimingFloat("Fade In##p",  &fi,  0.05f, 0.f,  60.f, state.bpm);
                        t_ch |= DragTimingFloat("Fade Out##p", &fo,  0.05f, 0.f,  60.f, state.bpm);
                        t_ch |= DragTimingFloat("Delay In##p", &di,  0.05f, 0.f,  60.f, state.bpm);
                        t_ch |= DragTimingFloat("Hold##p",     &hld, 0.05f, 0.f, 600.f, state.bpm);
                        DragTimingFloat("Delay Out##p", &do_, 0.05f, 0.f,  60.f, state.bpm);
                        DragTimingFloat("Wait##p",      &wt,  0.05f, 0.f, 600.f, state.bpm);
                        if (t_ch && cbs.on_set_cue_timing)
                            cbs.on_set_cue_timing(sel, fi, fo, di, hld);
                        ImGui::PopItemWidth();

                        ImGui::Separator();
                        ImGui::TextDisabled("Trigger");
                        static const char* kTrigNamesP[] = { "Halt", "Follow", "Wait" };
                        static int s_prop_trig = 0;
                        ImGui::PushItemWidth(-1.f);
                        ImGui::Combo("##prop_trig", &s_prop_trig, kTrigNamesP, 3);
                        ImGui::PopItemWidth();

                        ImGui::Separator();
                        ImGui::TextDisabled("Notes");
                        static char s_prop_notes[512] = {};
                        ImGui::PushItemWidth(-1.f);
                        ImGui::InputTextMultiline("##prop_notes", s_prop_notes, sizeof(s_prop_notes),
                            ImVec2(-1.f, 80.f));
                        ImGui::PopItemWidth();
                    } else {
                        ImGui::TextDisabled("No cue selected");
                    }

                    ImGui::EndChild(); // ##cs_props
                }

                ImGui::EndTabItem();
            } // Cue Sheet tab

            ImGui::EndTabBar();
        } // end if (BeginTabBar)
    ImGui::End();
    ImGui::PopStyleColor();
}


// ─────────────────────────────────────────────────────────────────────────────
//  §B3 Frame Editor panel  (redesigned: object-based editing, 3-layer system)
// ─────────────────────────────────────────────────────────────────────────────

// Helper: icon character for a laser object type
static const char* fe_type_icon(LaserObjectType t) {
    switch (t) {
    case LaserObjectType::Line:    return "-";
    case LaserObjectType::Dot:     return "o";
    case LaserObjectType::Bezier:  return "~";
    case LaserObjectType::Arc:     return "(";
    case LaserObjectType::Circle:  return "O";
    case LaserObjectType::Text:    return "T";
    default:                       return "?";
    }
}

// Helper: display name for a laser object type
static const char* fe_type_name(LaserObjectType t) {
    switch (t) {
    case LaserObjectType::Line:    return "Line";
    case LaserObjectType::Dot:     return "Dot";
    case LaserObjectType::Bezier:  return "Bezier";
    case LaserObjectType::Arc:     return "Arc";
    case LaserObjectType::Circle:  return "Circle";
    case LaserObjectType::Text:    return "Text";
    default:                       return "Object";
    }
}


// Interpolate between two AnimFrames by matching object IDs
static std::vector<LaserObject> fe_interp_anim_frames(
    const FrameEditorState::AnimFrame& fa,
    const FrameEditorState::AnimFrame& fb,
    float alpha)
{
    using IT = FrameEditorState::InterpType;
    std::vector<LaserObject> out;
    alpha = std::clamp(alpha, 0.f, 1.f);

    // Hold mode: just show frame A contents (no interpolation)
    if (fa.interp == IT::Hold) {
        return fa.objects;
    }

    // Apply easing to alpha
    float t = alpha;
    if (fa.interp == IT::EaseInOut) {
        // Smoothstep: t*t*(3-2t)
        t = t * t * (3.f - 2.f * t);
    }
    // IT::Linear and IT::Bezier both use linear alpha here (Bezier would need control pts)

    for (const auto& oa : fa.objects) {
        const LaserObject* ob_ptr = nullptr;
        for (const auto& ob : fb.objects)
            if (ob.id == oa.id) { ob_ptr = &ob; break; }

        LaserObject interp = oa;
        if (ob_ptr) {
            const auto& ob = *ob_ptr;
            interp.r         = oa.r         + (ob.r         - oa.r)         * t;
            interp.g         = oa.g         + (ob.g         - oa.g)         * t;
            interp.b         = oa.b         + (ob.b         - oa.b)         * t;
            interp.thickness = oa.thickness + (ob.thickness - oa.thickness) * t;
            int npts = (int)std::min(oa.pts.size(), ob.pts.size());
            interp.pts.resize(npts);
            for (int k = 0; k < npts; ++k) {
                interp.pts[k].x = oa.pts[k].x + (ob.pts[k].x - oa.pts[k].x) * t;
                interp.pts[k].y = oa.pts[k].y + (ob.pts[k].y - oa.pts[k].y) * t;
            }
            out.push_back(interp);
        } else {
            // Only in A — fade out
            if (alpha < 0.5f) out.push_back(interp);
        }
    }
    // Only in B — fade in
    for (const auto& ob : fb.objects) {
        bool in_a = false;
        for (const auto& oa : fa.objects)
            if (oa.id == ob.id) { in_a = true; break; }
        if (!in_a && alpha >= 0.5f) out.push_back(ob);
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Symmetry helper for editing: regenerate symmetry copies of selected objects
// ─────────────────────────────────────────────────────────────────────────────
static void fe_apply_symmetry_to_objects(FrameEditorState& fe, float /*canvas_w*/, float /*canvas_h*/) {
    using SM = FrameEditorState::SymmetryMode;
    if (fe.symmetry == SM::None) return;

    // Remove all objects whose text starts with "__sym" (symmetry copies)
    fe.objects.erase(std::remove_if(fe.objects.begin(), fe.objects.end(),
        [](const LaserObject& o) { return o.text.rfind("__sym", 0) == 0; }), fe.objects.end());

    // For each selected (source) object, generate symmetry copies
    std::vector<LaserObject> copies;
    for (auto& src : fe.objects) {
        bool is_selected = std::find(fe.selected_ids.begin(), fe.selected_ids.end(), src.id) != fe.selected_ids.end();
        if (!is_selected) continue;

        // Convert pts to ImVec2 for apply_symmetry
        std::vector<ImVec2> src_pts;
        for (auto& p : src.pts) src_pts.push_back({p.x, p.y});

        auto sym_sets = apply_symmetry(src_pts, fe.symmetry, fe.symmetry_center_x, fe.symmetry_center_y);
        // sym_sets[0] is the original; [1..N] are the copies
        for (size_t i = 1; i < sym_sets.size(); ++i) {
            LaserObject copy = src;
            copy.id = (src.id << 8) | static_cast<uint64_t>(i);
            copy.text = "__sym" + std::to_string(src.id); // mark as symmetry copy
            copy.pts.clear();
            for (auto& sp : sym_sets[i]) copy.pts.push_back({sp.x, sp.y});
            copies.push_back(copy);
        }
    }
    for (auto& c : copies) fe.objects.push_back(c);
}

// ACT symmetry: regenerate copies for ALL non-sym objects (not just selected)
static void fe_apply_symmetry_to_all_objects(FrameEditorState& fe) {
    using SM = FrameEditorState::SymmetryMode;
    if (fe.symmetry == SM::None) return;
    fe.objects.erase(std::remove_if(fe.objects.begin(), fe.objects.end(),
        [](const LaserObject& o) { return o.text.rfind("__sym", 0) == 0; }), fe.objects.end());
    std::vector<LaserObject> copies;
    for (auto& src : fe.objects) {
        std::vector<ImVec2> src_pts;
        for (auto& p : src.pts) src_pts.push_back({p.x, p.y});
        auto sym_sets = apply_symmetry(src_pts, fe.symmetry, fe.symmetry_center_x, fe.symmetry_center_y);
        for (size_t i = 1; i < sym_sets.size(); ++i) {
            LaserObject copy = src;
            copy.id   = (src.id << 8) | static_cast<uint64_t>(i);
            copy.text = "__sym" + std::to_string(src.id);
            copy.pts.clear();
            for (auto& sp : sym_sets[i]) copy.pts.push_back({sp.x, sp.y});
            copies.push_back(copy);
        }
    }
    for (auto& c : copies) fe.objects.push_back(c);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Premade Shapes — generation helper
//  Evaluates the selected function curve and appends LaserObjects to out[].
//  id_start: first monotonic ID to use; caller must advance fe.next_object_id.
// ─────────────────────────────────────────────────────────────────────────────
static void generate_premade_shapes(
        FrameEditorState::PremadeShapesState& ps,
        std::vector<LaserObject>& out,
        uint64_t& id_counter)
{
    using FT = FrameEditorState::PremadeShapesState::FuncType;
    using OT = FrameEditorState::PremadeShapesState::ObjType;

    const float kPi = 3.14159265358979323846f;

    int N = std::max(2, std::min(64, ps.sample_count));

    // Pre-compute all (raw_x, raw_y) in function space so we can normalise them
    std::vector<float> raw_x(N), raw_y(N);

    for (int i = 0; i < N; ++i) {
        float t = (N > 1) ? static_cast<float>(i) / static_cast<float>(N - 1) : 0.5f;

        float x = 0.f, y = 0.f;
        switch (ps.func) {
        case FT::Linear: {
            float xn = (N > 1) ? (t * 2.f - 1.f) : 0.f;
            x = xn;
            y = ps.param_a * xn + ps.param_b;
            break;
        }
        case FT::Quadratic: {
            x = ps.x_min + (ps.x_max - ps.x_min) * t;
            y = ps.param_a * x * x + ps.param_b * x + ps.param_c;
            break;
        }
        case FT::Cubic: {
            x = ps.x_min + (ps.x_max - ps.x_min) * t;
            y = ps.param_a * x*x*x + ps.param_b * x*x + ps.param_c * x + ps.param_d;
            break;
        }
        case FT::Sine: {
            x = ps.x_min + (ps.x_max - ps.x_min) * t;
            y = ps.param_a * std::sin(ps.param_b * x * kPi + ps.param_c * kPi / 180.f) + ps.param_d;
            break;
        }
        case FT::Cosine: {
            x = ps.x_min + (ps.x_max - ps.x_min) * t;
            y = ps.param_a * std::cos(ps.param_b * x * kPi + ps.param_c * kPi / 180.f) + ps.param_d;
            break;
        }
        case FT::Circle: {
            x = ps.param_a * std::cos(t * 2.f * kPi);
            y = ps.param_a * std::sin(t * 2.f * kPi);
            break;
        }
        case FT::Spiral: {
            float angle = t * 2.f * kPi * ps.param_b;
            x = ps.param_a * t * std::cos(angle);
            y = ps.param_a * t * std::sin(angle);
            break;
        }
        case FT::Lissajous: {
            x = std::cos(ps.lissajous_a * t * 2.f * kPi + ps.lissajous_delta * kPi / 180.f);
            y = std::sin(ps.lissajous_b * t * 2.f * kPi);
            break;
        }
        }
        raw_x[i] = x;
        raw_y[i] = y;
    }

    // For parametric shapes the coordinates are already in a well-defined range
    // (Circle: [-r,r], Spiral: [-r_max,r_max], Lissajous: [-1,1]) so normalising
    // them destroys the radius/turn parameters the user set.  Apply scale directly.
    // For algebraic shapes (Linear, Quadratic, etc.) the output range is undefined,
    // so we still normalise into [-1,1] first.
    bool is_parametric = (ps.func == FT::Circle || ps.func == FT::Spiral || ps.func == FT::Lissajous || ps.func == FT::Linear);

    std::vector<float> nx(N), ny(N);
    if (is_parametric) {
        for (int i = 0; i < N; ++i) {
            nx[i] = raw_x[i] * ps.scale_x + ps.offset_x;
            ny[i] = raw_y[i] * ps.scale_y + ps.offset_y;
        }
    } else {
        // Normalise raw_x/y into [-1,1] output space, then apply scale and offset
        float xmin = raw_x[0], xmax = raw_x[0];
        float ymin = raw_y[0], ymax = raw_y[0];
        for (int i = 1; i < N; ++i) {
            xmin = std::min(xmin, raw_x[i]);
            xmax = std::max(xmax, raw_x[i]);
            ymin = std::min(ymin, raw_y[i]);
            ymax = std::max(ymax, raw_y[i]);
        }
        float xspan = xmax - xmin;
        float yspan = ymax - ymin;
        for (int i = 0; i < N; ++i) {
            float fx = (xspan > 1e-6f) ? (2.f * (raw_x[i] - xmin) / xspan - 1.f) : 0.f;
            float fy = (yspan > 1e-6f) ? (2.f * (raw_y[i] - ymin) / yspan - 1.f) : 0.f;
            nx[i] = fx * ps.scale_x + ps.offset_x;
            ny[i] = fy * ps.scale_y + ps.offset_y;
        }
    }

    // Helper: compute tangent angle at sample i (numerical derivative)
    auto tangent_angle = [&](int i) -> float {
        int i0 = std::max(0, i - 1);
        int i1 = std::min(N - 1, i + 1);
        float dx = nx[i1] - nx[i0];
        float dy = ny[i1] - ny[i0];
        return std::atan2(dy, dx);
    };

    // Generate one LaserObject per sample
    for (int i = 0; i < N; ++i) {
        float t = (N > 1) ? static_cast<float>(i) / static_cast<float>(N - 1) : 0.5f;

        // Color (flat or gradient)
        float r, g, b;
        if (ps.use_gradient) {
            r = ps.col_r + (ps.col2_r - ps.col_r) * t;
            g = ps.col_g + (ps.col2_g - ps.col_g) * t;
            b = ps.col_b + (ps.col2_b - ps.col_b) * t;
        } else {
            r = ps.col_r; g = ps.col_g; b = ps.col_b;
        }
        r = std::clamp(r, 0.f, 1.f);
        g = std::clamp(g, 0.f, 1.f);
        b = std::clamp(b, 0.f, 1.f);

        float cx = nx[i];
        float cy = ny[i];
        float rot_rad = ps.obj_rot_tangent
            ? tangent_angle(i)
            : (ps.obj_rot * 3.14159265358979323846f / 180.f);

        LaserObject obj;
        obj.id   = id_counter++;
        obj.r    = r;
        obj.g    = g;
        obj.b    = b;
        obj.size = ps.obj_size;

        switch (ps.obj_type) {
        case OT::Dot: {
            obj.type = LaserObjectType::Dot;
            obj.pts.push_back({cx, cy});
            break;
        }
        case OT::Line: {
            // A short line of length obj_size, centred at (cx,cy), rotated by rot_rad
            obj.type = LaserObjectType::Line;
            float half = ps.obj_size * 0.5f;
            float cr = std::cos(rot_rad);
            float sr = std::sin(rot_rad);
            obj.pts.push_back({cx - cr * half, cy - sr * half});
            obj.pts.push_back({cx + cr * half, cy + sr * half});
            break;
        }
        case OT::Circle: {
            // Circle: pts[0]=center, pts[1]=edge (radius encoded as distance)
            obj.type = LaserObjectType::Circle;
            obj.pts.push_back({cx, cy});
            obj.pts.push_back({cx + ps.obj_size, cy});
            break;
        }
        case OT::Square: {
            // Square as closed Line (4 corners + closing point)
            obj.type = LaserObjectType::Line;
            float h = ps.obj_size * 0.5f;
            float cr = std::cos(rot_rad);
            float sr = std::sin(rot_rad);
            // Corners in local space: (-h,-h), (+h,-h), (+h,+h), (-h,+h)
            float lx[4] = {-h, +h, +h, -h};
            float ly[4] = {-h, -h, +h, +h};
            for (int k = 0; k < 4; ++k) {
                float wx = cx + lx[k] * cr - ly[k] * sr;
                float wy = cy + lx[k] * sr + ly[k] * cr;
                obj.pts.push_back({wx, wy});
            }
            // Close the loop
            obj.pts.push_back({obj.pts[0].x, obj.pts[0].y});
            break;
        }
        case OT::Triangle: {
            // Equilateral triangle as closed Line
            obj.type = LaserObjectType::Line;
            float kPi2 = 3.14159265358979323846f;
            for (int k = 0; k < 3; ++k) {
                float angle = rot_rad + kPi2 * 2.f / 3.f * static_cast<float>(k) - kPi2 * 0.5f;
                obj.pts.push_back({cx + ps.obj_size * std::cos(angle),
                                   cy + ps.obj_size * std::sin(angle)});
            }
            obj.pts.push_back({obj.pts[0].x, obj.pts[0].y}); // close
            break;
        }
        }

        out.push_back(obj);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Filled Shapes: scanline fill generator
// ─────────────────────────────────────────────────────────────────────────────

// Rotate a point (lx,ly) by rot_rad around origin, then translate by (cx,cy)
static void fs_rotate_pt(float lx, float ly, float rot_rad,
                          float cx, float cy, float& ox, float& oy) {
    float c = std::cos(rot_rad);
    float s = std::sin(rot_rad);
    ox = cx + lx * c - ly * s;
    oy = cy + lx * s + ly * c;
}

// Build a polygon boundary from a list of local (x,y) pairs.
// Returns closed polygon in world space.
static std::vector<std::pair<float,float>> fs_make_polygon(
    const std::vector<std::pair<float,float>>& local_verts,
    float rot_rad, float cx, float cy)
{
    std::vector<std::pair<float,float>> out;
    out.reserve(local_verts.size());
    for (auto& v : local_verts) {
        float wx, wy;
        fs_rotate_pt(v.first, v.second, rot_rad, cx, cy, wx, wy);
        out.push_back({wx, wy});
    }
    return out;
}

// Compute scanline x-intersections with a polygon (2D ray cast)
static std::vector<float> fs_scanline_intersect(
    const std::vector<std::pair<float,float>>& poly, float y)
{
    std::vector<float> xs;
    int n = (int)poly.size();
    for (int i = 0; i < n; ++i) {
        float x0 = poly[i].first,   y0 = poly[i].second;
        float x1 = poly[(i+1)%n].first, y1 = poly[(i+1)%n].second;
        if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
            float t = (y - y0) / (y1 - y0);
            xs.push_back(x0 + t * (x1 - x0));
        }
    }
    std::sort(xs.begin(), xs.end());
    return xs;
}

// Generate all outline points for the given polygon (laser-safe polyline)
static void fs_emit_outline(
    const std::vector<std::pair<float,float>>& poly,
    uint8_t r, uint8_t g, uint8_t b,
    std::vector<LaserObjectPoint>& pts_out)
{
    if (poly.empty()) return;
    // Blanked travel to first point
    (void)r; (void)g; (void)b;
    for (auto& p : poly)
        pts_out.push_back({p.first, p.second});
    // Close the loop
    pts_out.push_back({poly[0].first, poly[0].second});
}

// Core scanline fill: polygon, gap between scanlines
static void fs_fill_polygon(
    const std::vector<std::pair<float,float>>& poly,
    float gap, float size,
    uint8_t r8, uint8_t g8, uint8_t b8,
    LaserObject& obj)
{
    if (poly.empty() || gap <= 0.f) return;
    (void)r8; (void)g8; (void)b8;

    // Bounding box
    float ymin =  1e9f, ymax = -1e9f;
    for (auto& p : poly) { ymin = std::min(ymin, p.second); ymax = std::max(ymax, p.second); }

    float step = std::max(gap, 0.001f);
    int n_lines = static_cast<int>((ymax - ymin) / step) + 2;
    // Clamp to avoid absurd point counts
    if (n_lines > 4096) n_lines = 4096;

    bool first_pt = true;
    bool row_dir = true; // alternate direction per scanline (boustrophedon)

    for (int li = 0; li < n_lines; ++li) {
        float y = ymin + li * step;
        auto xs = fs_scanline_intersect(poly, y);
        if (xs.size() < 2) continue;

        // Point density: one point every half-gap
        float pt_spacing = std::max(gap * 0.5f, 0.001f) / std::max(size, 0.01f) * size;
        pt_spacing = std::max(pt_spacing, 0.002f);

        // Process pairs of intersections
        for (int pi = 0; pi + 1 < (int)xs.size(); pi += 2) {
            float xl = xs[pi];
            float xr = xs[pi+1];
            if (xl > xr) std::swap(xl, xr);
            float row_len = xr - xl;
            int n_pts = std::max(2, static_cast<int>(row_len / pt_spacing) + 1);

            // Add blanked travel point before row (unless very first point)
            if (!first_pt) {
                float tx = row_dir ? xl : xr;
                obj.pts.push_back({tx, y});
                // Mark as blanked by not including it in lit pts: we use
                // a workaround: add as a duplicate (laser dwells blanked between segments).
                // Since show_engine renders Line as a pure polyline, we duplicate
                // the first point of the new row to force a "blank" transition.
                // We just push a second identical point (minimal dwell) here.
            }

            float x_start = row_dir ? xl : xr;
            float x_end   = row_dir ? xr : xl;

            for (int k = 0; k < n_pts; ++k) {
                float t = (n_pts > 1) ? static_cast<float>(k) / (n_pts - 1) : 0.f;
                float px = x_start + t * (x_end - x_start);
                obj.pts.push_back({px, y});
                first_pt = false;
            }
        }
        row_dir = !row_dir;
    }
}

// Simple 5x7 stroke font: returns line segments for ASCII 32-90 (space..Z and 0-9)
// Each letter is defined in a 5-wide x 7-tall grid, normalized to [0,1]x[0,1]
// Returns vector of {x0,y0,x1,y1} segments in local space (0..1 range, y up)
static std::vector<std::array<float,4>> fs_letter_segs(char ch)
{
    // y is inverted (0=top, 1=bottom in definition, we flip later)
    using S = std::array<float,4>;
    auto s = [](float x0,float y0,float x1,float y1) -> S {
        return {x0/4.f, 1.f-(y0/6.f), x1/4.f, 1.f-(y1/6.f)};
    };
    switch (ch) {
    case 'A': return {s(0,6,2,0),s(4,6,2,0),s(0,3,4,3)};
    case 'B': return {s(0,0,0,6),s(0,0,3,0),s(3,0,4,1),s(4,1,4,2),s(4,2,3,3),
                      s(3,3,0,3),s(3,3,4,4),s(4,4,4,5),s(4,5,3,6),s(3,6,0,6)};
    case 'C': return {s(4,1,3,0),s(3,0,1,0),s(1,0,0,1),s(0,1,0,5),s(0,5,1,6),
                      s(1,6,3,6),s(3,6,4,5)};
    case 'D': return {s(0,0,0,6),s(0,0,2,0),s(2,0,4,2),s(4,2,4,4),s(4,4,2,6),s(2,6,0,6)};
    case 'E': return {s(0,0,0,6),s(0,0,4,0),s(0,3,3,3),s(0,6,4,6)};
    case 'F': return {s(0,0,0,6),s(0,0,4,0),s(0,3,3,3)};
    case 'G': return {s(4,1,3,0),s(3,0,1,0),s(1,0,0,1),s(0,1,0,5),s(0,5,1,6),
                      s(1,6,3,6),s(3,6,4,5),s(4,5,4,3),s(4,3,2,3)};
    case 'H': return {s(0,0,0,6),s(4,0,4,6),s(0,3,4,3)};
    case 'I': return {s(0,0,4,0),s(2,0,2,6),s(0,6,4,6)};
    case 'J': return {s(4,0,4,5),s(4,5,3,6),s(3,6,1,6),s(1,6,0,5)};
    case 'K': return {s(0,0,0,6),s(0,3,4,0),s(0,3,4,6)};
    case 'L': return {s(0,0,0,6),s(0,6,4,6)};
    case 'M': return {s(0,6,0,0),s(0,0,2,3),s(2,3,4,0),s(4,0,4,6)};
    case 'N': return {s(0,6,0,0),s(0,0,4,6),s(4,6,4,0)};
    case 'O': return {s(1,0,3,0),s(3,0,4,1),s(4,1,4,5),s(4,5,3,6),
                      s(3,6,1,6),s(1,6,0,5),s(0,5,0,1),s(0,1,1,0)};
    case 'P': return {s(0,0,0,6),s(0,0,3,0),s(3,0,4,1),s(4,1,4,2),s(4,2,3,3),s(3,3,0,3)};
    case 'Q': return {s(1,0,3,0),s(3,0,4,1),s(4,1,4,5),s(4,5,3,6),
                      s(3,6,1,6),s(1,6,0,5),s(0,5,0,1),s(0,1,1,0),s(2,4,4,6)};
    case 'R': return {s(0,0,0,6),s(0,0,3,0),s(3,0,4,1),s(4,1,4,2),s(4,2,3,3),
                      s(3,3,0,3),s(2,3,4,6)};
    case 'S': return {s(4,1,3,0),s(3,0,1,0),s(1,0,0,1),s(0,1,0,2),s(0,2,1,3),
                      s(1,3,3,3),s(3,3,4,4),s(4,4,4,5),s(4,5,3,6),s(3,6,1,6),s(1,6,0,5)};
    case 'T': return {s(0,0,4,0),s(2,0,2,6)};
    case 'U': return {s(0,0,0,5),s(0,5,1,6),s(1,6,3,6),s(3,6,4,5),s(4,5,4,0)};
    case 'V': return {s(0,0,2,6),s(4,0,2,6)};
    case 'W': return {s(0,0,1,6),s(1,6,2,3),s(2,3,3,6),s(3,6,4,0)};
    case 'X': return {s(0,0,4,6),s(4,0,0,6)};
    case 'Y': return {s(0,0,2,3),s(4,0,2,3),s(2,3,2,6)};
    case 'Z': return {s(0,0,4,0),s(4,0,0,6),s(0,6,4,6)};
    case '0': return {s(1,0,3,0),s(3,0,4,1),s(4,1,4,5),s(4,5,3,6),
                      s(3,6,1,6),s(1,6,0,5),s(0,5,0,1),s(0,1,1,0),s(1,1,3,5)};
    case '1': return {s(1,1,2,0),s(2,0,2,6)};
    case '2': return {s(0,1,1,0),s(1,0,3,0),s(3,0,4,1),s(4,1,4,2),s(4,2,0,6),s(0,6,4,6)};
    case '3': return {s(0,1,1,0),s(1,0,3,0),s(3,0,4,1),s(4,1,4,2),s(4,2,3,3),
                      s(3,3,4,4),s(4,4,4,5),s(4,5,3,6),s(3,6,1,6),s(1,6,0,5)};
    case '4': return {s(0,0,0,3),s(0,3,4,3),s(4,0,4,6)};
    case '5': return {s(4,0,0,0),s(0,0,0,3),s(0,3,3,3),s(3,3,4,4),
                      s(4,4,4,5),s(4,5,3,6),s(3,6,1,6),s(1,6,0,5)};
    case '6': return {s(4,1,3,0),s(3,0,1,0),s(1,0,0,1),s(0,1,0,5),s(0,5,1,6),
                      s(1,6,3,6),s(3,6,4,5),s(4,5,4,4),s(4,4,3,3),s(3,3,0,3)};
    case '7': return {s(0,0,4,0),s(4,0,2,6)};
    case '8': return {s(1,0,3,0),s(3,0,4,1),s(4,1,4,2),s(4,2,3,3),s(3,3,4,4),
                      s(4,4,4,5),s(4,5,3,6),s(3,6,1,6),s(1,6,0,5),s(0,5,0,4),
                      s(0,4,1,3),s(1,3,0,2),s(0,2,0,1),s(0,1,1,0)};
    case '9': return {s(0,5,1,6),s(1,6,3,6),s(3,6,4,5),s(4,5,4,1),s(4,1,3,0),
                      s(3,0,1,0),s(1,0,0,1),s(0,1,0,2),s(0,2,1,3),s(1,3,4,3)};
    default:  return {};  // space or unknown
    }
}

// Generate filled shape as a single LaserObject (Line type, many pts)
// cx, cy: center in normalized canvas space (-1..1)
static void place_filled_shape_at(FrameEditorState& fe, float cx, float cy)
{
    using FS = FrameEditorState::FilledShapeState;
    using Shape = FS::Shape;
    auto& fs = fe.filled_shape;

    float rot_rad = fs.rot * 3.14159265358979323846f / 180.f;
    float sz = std::max(fs.size, 0.01f);
    float gap = std::max(fs.fill_line_gap, 0.001f) * sz;

    uint8_t r8 = static_cast<uint8_t>(std::clamp(fs.col_r, 0.f, 1.f) * 255.f);
    uint8_t g8 = static_cast<uint8_t>(std::clamp(fs.col_g, 0.f, 1.f) * 255.f);
    uint8_t b8 = static_cast<uint8_t>(std::clamp(fs.col_b, 0.f, 1.f) * 255.f);

    // Build polygon boundary in local space, then rotate+translate to world space
    std::vector<std::pair<float,float>> poly;

    static constexpr float kPi = 3.14159265358979323846f;

    auto build_ngon = [&](int n, float r_outer) {
        std::vector<std::pair<float,float>> verts;
        for (int i = 0; i < n; ++i) {
            float a = kPi * 2.f * i / n - kPi * 0.5f;
            verts.push_back({r_outer * std::cos(a), r_outer * std::sin(a)});
        }
        return verts;
    };

    auto build_star = [&](int points, float r_outer, float r_inner) {
        std::vector<std::pair<float,float>> verts;
        int total = points * 2;
        for (int i = 0; i < total; ++i) {
            float a = kPi * 2.f * i / total - kPi * 0.5f;
            float r = (i % 2 == 0) ? r_outer : r_inner;
            verts.push_back({r * std::cos(a), r * std::sin(a)});
        }
        return verts;
    };

    switch (fs.shape) {
    case Shape::FilledRect: {
        poly = {{-sz,-sz},{sz,-sz},{sz,sz},{-sz,sz}};
        break;
    }
    case Shape::FilledCircle: {
        // Approximate circle with 64-gon
        poly = build_ngon(64, sz);
        break;
    }
    case Shape::FilledTriangle: {
        poly = build_ngon(3, sz);
        break;
    }
    case Shape::FilledPolygon: {
        int sides = std::clamp(fs.polygon_sides, 3, 16);
        poly = build_ngon(sides, sz);
        break;
    }
    case Shape::FilledStar: {
        int pts = std::clamp(fs.star_points, 3, 12);
        poly = build_star(pts, sz, sz * 0.4f);
        break;
    }
    case Shape::FilledText: {
        // Each letter gets its own object placed side by side
        const std::string& txt = fs.text_content;
        float letter_w = fs.text_size;
        float total_w = letter_w * txt.size() + fs.letter_gap * (txt.size() > 1 ? txt.size()-1 : 0);
        float x_start = cx - total_w * 0.5f;

        for (size_t li2 = 0; li2 < txt.size(); ++li2) {
            char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(txt[li2])));
            auto segs = fs_letter_segs(ch);
            if (segs.empty() && ch == ' ') {
                x_start += letter_w + fs.letter_gap;
                continue;
            }
            LaserObject tobj;
            tobj.id   = fe.next_object_id++;
            tobj.type = LaserObjectType::Line;
            tobj.r    = fs.col_r; tobj.g = fs.col_g; tobj.b = fs.col_b;
            tobj.size = 0.01f;

            float lx0 = x_start;
            float ly0 = cy - fs.text_size * 0.5f;

            for (auto& seg : segs) {
                // seg: {x0,y0,x1,y1} in [0,1]x[0,1] letter space
                float wx0, wy0, wx1, wy1;
                fs_rotate_pt(lx0 + seg[0]*letter_w - cx,
                             ly0 + seg[1]*fs.text_size - cy,
                             rot_rad, cx, cy, wx0, wy0);
                fs_rotate_pt(lx0 + seg[2]*letter_w - cx,
                             ly0 + seg[3]*fs.text_size - cy,
                             rot_rad, cx, cy, wx1, wy1);
                // Add a pen-up gap before each stroke
                if (!tobj.pts.empty()) {
                    // duplicate last point as "blank" dwell
                    tobj.pts.push_back(tobj.pts.back());
                }
                tobj.pts.push_back({wx0, wy0});
                tobj.pts.push_back({wx1, wy1});
            }
            if (!tobj.pts.empty())
                fe.objects.push_back(std::move(tobj));
            x_start += letter_w + fs.letter_gap;
        }
        return;  // Text places multiple objects; done
    }
    }

    if (poly.empty()) return;

    // Apply rotation and translation to polygon
    auto world_poly = fs_make_polygon(poly, rot_rad, cx, cy);

    LaserObject obj;
    obj.id   = fe.next_object_id++;
    obj.type = LaserObjectType::Line;
    obj.r    = fs.col_r;
    obj.g    = fs.col_g;
    obj.b    = fs.col_b;
    obj.size = sz;

    if (fs.outline_only) {
        fs_emit_outline(world_poly, r8, g8, b8, obj.pts);
    } else {
        // First emit the scanline fill
        fs_fill_polygon(world_poly, gap, sz, r8, g8, b8, obj);
        // Then trace the outline on top for clean edges
        if (!obj.pts.empty()) {
            // Add blanked-travel gap by duplicating last point
            obj.pts.push_back(obj.pts.back());
        }
        fs_emit_outline(world_poly, r8, g8, b8, obj.pts);
    }

    if (!obj.pts.empty())
        fe.objects.push_back(std::move(obj));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Filled Shapes panel
//  Returns: 0 = Place at Center clicked, 1 = Place at... clicked, -1 = nothing
// ─────────────────────────────────────────────────────────────────────────────
static int draw_filled_shape_panel(FrameEditorState& fe)
{
    using FS = FrameEditorState::FilledShapeState;
    using Shape = FS::Shape;
    auto& fs = fe.filled_shape;

    ImGui::SetNextWindowSizeConstraints(ImVec2(280.f, 360.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(350.f, 520.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Filled Shapes", &fs.open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return -1;
    }

    int result = -1;

    // Shape combo
    const char* shape_names[] = {
        "Filled Rect", "Filled Circle", "Filled Triangle",
        "Filled Star", "Filled Polygon", "Filled Text"
    };
    int shape_idx = static_cast<int>(fs.shape);
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##fs_shape", &shape_idx, shape_names, 6))
        fs.shape = static_cast<Shape>(shape_idx);

    ImGui::Spacing();

    // Shape-specific params
    if (fs.shape == Shape::FilledPolygon) {
        ImGui::SetNextItemWidth(150.f);
        ImGui::SliderInt("Sides##fs", &fs.polygon_sides, 3, 16);
    }
    if (fs.shape == Shape::FilledStar) {
        ImGui::SetNextItemWidth(150.f);
        ImGui::SliderInt("Points##fs", &fs.star_points, 3, 12);
    }
    if (fs.shape == Shape::FilledText) {
        // Use a local static buffer; copy in/out to std::string
        static char fs_text_buf[128] = "LASER";
        static bool fs_text_synced = false;
        if (!fs_text_synced) {
            std::strncpy(fs_text_buf, fs.text_content.c_str(), sizeof(fs_text_buf)-1);
            fs_text_buf[sizeof(fs_text_buf)-1] = '\0';
            fs_text_synced = true;
        }
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::InputText("Text##fs", fs_text_buf, sizeof(fs_text_buf))) {
            fs.text_content = fs_text_buf;
        }
        ImGui::SetNextItemWidth(120.f);
        ImGui::DragFloat("Letter Gap##fs", &fs.letter_gap, 0.005f, 0.0f, 0.5f, "%.3f");
    }

    ImGui::Separator();

    // Common params
    ImGui::SetNextItemWidth(150.f);
    ImGui::DragFloat("Size##fs", &fs.size, 0.005f, 0.05f, 1.0f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Normalized half-size (0.05..1.0)");

    ImGui::SetNextItemWidth(150.f);
    ImGui::DragFloat("Rotation##fs", &fs.rot, 1.f, 0.f, 360.f, "%.1f deg");

    ImGui::SetNextItemWidth(150.f);
    ImGui::DragFloat("Scanline spacing##fs", &fs.fill_line_gap, 0.001f, 0.005f, 0.1f, "%.4f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Gap between fill scanlines (relative to size)");

    ImGui::Checkbox("Outline only##fs", &fs.outline_only);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Draw only the boundary outline, no fill");

    ImGui::Separator();

    float col3[3] = {fs.col_r, fs.col_g, fs.col_b};
    if (ImGui::ColorEdit3("Color##fs", col3)) {
        fs.col_r = col3[0]; fs.col_g = col3[1]; fs.col_b = col3[2];
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Place at Center
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.55f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.70f, 0.20f, 1.f));
    if (ImGui::Button("Place at Center##fs", ImVec2(-1.f, 0.f))) {
        result = 0;
    }
    ImGui::PopStyleColor(2);

    ImGui::Spacing();

    // Place at... (next click)
    ImVec4 teal_col  = {0.05f, 0.50f, 0.55f, 1.f};
    ImVec4 teal_hov  = {0.08f, 0.65f, 0.70f, 1.f};
    if (fs.pending_place) {
        teal_col = {0.55f, 0.30f, 0.05f, 1.f};
        teal_hov = {0.70f, 0.40f, 0.08f, 1.f};
    }
    ImGui::PushStyleColor(ImGuiCol_Button,        teal_col);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, teal_hov);
    const char* place_lbl = fs.pending_place ? "Click canvas to place...##fs" : "Place at...##fs";
    if (ImGui::Button(place_lbl, ImVec2(-1.f, 0.f))) {
        fs.pending_place = !fs.pending_place;
        if (fs.pending_place) result = 1;
    }
    ImGui::PopStyleColor(2);

    if (fs.pending_place)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Click on the canvas to place the shape");

    ImGui::End();
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Premade Shapes panel — drawn as a floating ImGui window when tool is active
//  Returns true if the Generate button was pressed (caller should push undo).
// ─────────────────────────────────────────────────────────────────────────────
static bool draw_premade_shapes_panel(FrameEditorState& fe)
{
    using FT = FrameEditorState::PremadeShapesState::FuncType;
    using OT = FrameEditorState::PremadeShapesState::ObjType;

    FrameEditorState::PremadeShapesState& ps = fe.premade_shapes;

    ImGui::SetNextWindowSizeConstraints(ImVec2(300.f, 400.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(360.f, 580.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Premade Shapes", &ps.open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        return false;
    }

    // ── Function selector ───────────────────────────────────────────────────
    const char* func_names[] = {
        "Linear  (y=ax+b)",
        "Quadratic  (y=ax2+bx+c)",
        "Cubic  (y=ax3+bx2+cx+d)",
        "Sine  (y=a*sin(bx+c)+d)",
        "Cosine  (y=a*cos(bx+c)+d)",
        "Circle  (parametric)",
        "Spiral  (parametric)",
        "Lissajous  (parametric)"
    };
    int func_idx = static_cast<int>(ps.func);
    ImGui::SetNextItemWidth(-1.f);
    if (ImGui::Combo("##func_type", &func_idx, func_names, 8))
        ps.func = static_cast<FT>(func_idx);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Select the mathematical function to place objects along");

    ImGui::SeparatorText("Function Parameters");

    // ── Function-specific parameter sliders ─────────────────────────────────
    ImGui::SetNextItemWidth(200.f);
    switch (ps.func) {
    case FT::Linear:
        ImGui::DragFloat("Slope (a)##pa",    &ps.param_a, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("Offset (b)##pb",   &ps.param_b, 0.01f, -10.f, 10.f, "%.3f");
        break;
    case FT::Quadratic:
        ImGui::DragFloat("a (x2)##pa",       &ps.param_a, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("b (x)##pb",        &ps.param_b, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("c (offset)##pc",   &ps.param_c, 0.01f, -10.f, 10.f, "%.3f");
        break;
    case FT::Cubic:
        ImGui::DragFloat("a (x3)##pa",       &ps.param_a, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("b (x2)##pb",       &ps.param_b, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("c (x)##pc",        &ps.param_c, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("d (offset)##pd",   &ps.param_d, 0.01f, -10.f, 10.f, "%.3f");
        break;
    case FT::Sine:
    case FT::Cosine:
        ImGui::DragFloat("Amplitude (a)##pa", &ps.param_a, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("Frequency (b)##pb", &ps.param_b, 0.01f, -10.f, 10.f, "%.3f");
        ImGui::DragFloat("Phase deg (c)##pc", &ps.param_c, 1.f,  -360.f,360.f, "%.1f");
        ImGui::DragFloat("Vert shift (d)##pd",&ps.param_d, 0.01f, -10.f, 10.f, "%.3f");
        break;
    case FT::Circle:
        ImGui::DragFloat("Radius (a)##pa",    &ps.param_a, 0.01f, 0.01f, 10.f, "%.3f");
        break;
    case FT::Spiral:
        ImGui::DragFloat("Radius mult (a)##pa",&ps.param_a, 0.01f, 0.01f, 10.f, "%.3f");
        ImGui::DragFloat("Turns (b)##pb",      &ps.param_b, 0.1f,  0.25f, 20.f, "%.2f");
        break;
    case FT::Lissajous:
        ImGui::DragFloat("X freq (A)##la",     &ps.lissajous_a,     0.1f,  0.f, 20.f, "%.1f");
        ImGui::DragFloat("Y freq (B)##lb",     &ps.lissajous_b,     0.1f,  0.f, 20.f, "%.1f");
        ImGui::DragFloat("Phase deg##ld",      &ps.lissajous_delta, 1.f, -360.f, 360.f, "%.1f");
        break;
    }

    // ── Sampling range ───────────────────────────────────────────────────────
    ImGui::SeparatorText("Sampling");

    ImGui::SetNextItemWidth(200.f);
    ImGui::SliderInt("Count##sc", &ps.sample_count, 2, 64);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Number of objects to place (2..64)");
    ps.sample_count = std::clamp(ps.sample_count, 2, 64);

    // x_min / x_max only make sense for non-parametric functions
    bool parametric = (ps.func == FT::Circle || ps.func == FT::Spiral || ps.func == FT::Lissajous || ps.func == FT::Linear);
    if (parametric) ImGui::BeginDisabled(true);
    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("X min##xmin", &ps.x_min, 0.01f, -20.f, 20.f, "%.2f");
    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("X max##xmax", &ps.x_max, 0.01f, -20.f, 20.f, "%.2f");
    if (parametric) ImGui::EndDisabled();

    // ── Object type ──────────────────────────────────────────────────────────
    ImGui::SeparatorText("Object");

    const char* obj_names[] = { "Dot", "Line", "Circle", "Square", "Triangle" };
    int obj_idx = static_cast<int>(ps.obj_type);
    ImGui::SetNextItemWidth(140.f);
    if (ImGui::Combo("Type##ot", &obj_idx, obj_names, 5))
        ps.obj_type = static_cast<OT>(obj_idx);

    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("Size##osize", &ps.obj_size, 0.001f, 0.01f, 0.5f, "%.3f");
    ps.obj_size = std::clamp(ps.obj_size, 0.001f, 0.5f);

    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("Rotation##orot", &ps.obj_rot, 1.f, 0.f, 360.f, "%.1f deg");
    ImGui::Checkbox("Follow tangent##otang", &ps.obj_rot_tangent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Automatically rotate each object to follow the curve direction");

    // ── Scale & position ─────────────────────────────────────────────────────
    ImGui::SeparatorText("Output Transform");

    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("Scale X##sx", &ps.scale_x, 0.01f, 0.1f, 2.f, "%.2f");
    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("Scale Y##sy", &ps.scale_y, 0.01f, 0.1f, 2.f, "%.2f");
    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("Offset X##ox", &ps.offset_x, 0.01f, -1.f, 1.f, "%.2f");
    ImGui::SetNextItemWidth(200.f);
    ImGui::DragFloat("Offset Y##oy", &ps.offset_y, 0.01f, -1.f, 1.f, "%.2f");

    // ── Color ────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Color");

    float col[3] = {ps.col_r, ps.col_g, ps.col_b};
    if (ImGui::ColorEdit3("Color##col1", col)) {
        ps.col_r = col[0]; ps.col_g = col[1]; ps.col_b = col[2];
    }
    ImGui::Checkbox("Gradient##grad", &ps.use_gradient);
    if (ps.use_gradient) {
        float col2[3] = {ps.col2_r, ps.col2_g, ps.col2_b};
        if (ImGui::ColorEdit3("End Color##col2", col2)) {
            ps.col2_r = col2[0]; ps.col2_g = col2[1]; ps.col2_b = col2[2];
        }
    }

    // ── Preview ──────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Preview");
    ImGui::Checkbox("Show curve preview##prev", &ps.show_curve_preview);

    // Draw a small inline curve preview in this panel
    if (ps.show_curve_preview) {
        ImVec2 preview_size(ImGui::GetContentRegionAvail().x, 80.f);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##ps_prev", preview_size);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p0, ImVec2(p0.x + preview_size.x, p0.y + preview_size.y),
                          IM_COL32(15, 15, 20, 255));
        dl->AddRect(p0, ImVec2(p0.x + preview_size.x, p0.y + preview_size.y),
                    IM_COL32(60, 60, 80, 255));

        // Generate preview points (simplified — just center positions)
        std::vector<LaserObject> preview_objs;
        preview_objs.reserve(static_cast<size_t>(ps.sample_count));
        uint64_t dummy_id = 0;
        generate_premade_shapes(ps, preview_objs, dummy_id);

        float pw = preview_size.x - 4.f;
        float ph = preview_size.y - 4.f;
        float pcx = p0.x + preview_size.x * 0.5f;
        float pcy = p0.y + preview_size.y * 0.5f;

        for (int i = 0; i < static_cast<int>(preview_objs.size()); ++i) {
            const auto& pobj = preview_objs[i];
            if (pobj.pts.empty()) continue;
            float t = (preview_objs.size() > 1)
                ? static_cast<float>(i) / static_cast<float>(preview_objs.size() - 1)
                : 0.5f;
            float pr, pg, pb_col;
            if (ps.use_gradient) {
                pr = ps.col_r + (ps.col2_r - ps.col_r) * t;
                pg = ps.col_g + (ps.col2_g - ps.col_g) * t;
                pb_col = ps.col_b + (ps.col2_b - ps.col_b) * t;
            } else {
                pr = ps.col_r; pg = ps.col_g; pb_col = ps.col_b;
            }
            ImU32 pcol = IM_COL32(static_cast<int>(pr*255), static_cast<int>(pg*255),
                                  static_cast<int>(pb_col*255), 220);

            // Map normalized [-1,1] to preview rect
            float sx = pcx + pobj.pts[0].x * pw * 0.5f;
            float sy = pcy - pobj.pts[0].y * ph * 0.5f;  // flip Y for screen space
            dl->AddCircleFilled(ImVec2(sx, sy), 2.5f, pcol);

            // Draw connecting line between consecutive samples
            if (i > 0 && !preview_objs[i-1].pts.empty()) {
                float px2 = pcx + preview_objs[i-1].pts[0].x * pw * 0.5f;
                float py2 = pcy - preview_objs[i-1].pts[0].y * ph * 0.5f;
                dl->AddLine(ImVec2(px2, py2), ImVec2(sx, sy),
                            IM_COL32(80,80,100,120), 1.f);
            }
        }
    }

    // ── Rebuild live preview PointBuffer every frame ─────────────────────────
    {
        // Re-generate preview center points from the current parameters.
        // Use same logic as generate_premade_shapes but only collect pts[0] of each
        // sample and string them into a PointBuffer for canvas overlay.
        std::vector<LaserObject> prev_objs;
        prev_objs.reserve(static_cast<size_t>(ps.sample_count));
        uint64_t dummy_id = 0;
        generate_premade_shapes(ps, prev_objs, dummy_id);

        ps.preview_pts.clear();
        ps.preview_pts.reserve(prev_objs.size());
        for (const auto& obj : prev_objs) {
            if (obj.pts.empty()) continue;
            // Use centre point of each object (pts[0])
            LaserPoint lp;
            lp.x = static_cast<int16_t>(std::clamp(obj.pts[0].x, -1.f, 1.f) * 32767.f);
            lp.y = static_cast<int16_t>(std::clamp(obj.pts[0].y, -1.f, 1.f) * 32767.f);
            lp.r = static_cast<uint8_t>(std::min(255.f, obj.r * 255.f));
            lp.g = static_cast<uint8_t>(std::min(255.f, obj.g * 255.f));
            lp.b = static_cast<uint8_t>(std::min(255.f, obj.b * 255.f));
            lp.blanked = false;
            ps.preview_pts.push_back(lp);
        }
    }

    // ── Generate button ──────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.50f, 0.12f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.70f, 0.18f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.03f, 0.35f, 0.08f, 1.f));
    bool generate = ImGui::Button("Generate##ps_gen", ImVec2(-1.f, 0.f));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Place %d objects along the curve into the current frame", ps.sample_count);

    ImGui::End();
    return generate;
}

void panel_frame_editor(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    (void)cbs;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.08f, 1.f));

    if (!ImGui::Begin(kWinFrameEditor, &ctx.frame_editor_open)) {
        ImGui::End();
        ImGui::PopStyleColor();
        return;
    }

    ctx.fe_window_focused = ImGui::IsWindowFocused(
        ImGuiFocusedFlags_ChildWindows | ImGuiFocusedFlags_RootWindow);

    FrameEditorState& fe = ctx.frame_editor;

    // Frame editor undo/redo helper — push before every destructive operation
    auto fe_push_undo = [&]() {
        fe.fe_undo_stack.push_back(fe.objects);
        if (static_cast<int>(fe.fe_undo_stack.size()) > FrameEditorState::kMaxUndoDepth)
            fe.fe_undo_stack.erase(fe.fe_undo_stack.begin());
        fe.fe_redo_stack.clear();
    };

    // Handle Ctrl+Z / Ctrl+Y / Ctrl+A for frame editor when this window is focused
    if (ctx.fe_window_focused) {
        ImGuiIO& feio = ImGui::GetIO();
        if (feio.KeyCtrl && !feio.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
                if (feio.KeyShift) {
                    if (!fe.fe_redo_stack.empty()) {
                        fe.fe_undo_stack.push_back(fe.objects);
                        fe.objects = std::move(fe.fe_redo_stack.back());
                        fe.fe_redo_stack.pop_back();
                        fe.selected_ids.clear();
                    }
                } else {
                    if (!fe.fe_undo_stack.empty()) {
                        fe.fe_redo_stack.push_back(fe.objects);
                        fe.objects = std::move(fe.fe_undo_stack.back());
                        fe.fe_undo_stack.pop_back();
                        fe.selected_ids.clear();
                    }
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
                if (!fe.fe_redo_stack.empty()) {
                    fe.fe_undo_stack.push_back(fe.objects);
                    fe.objects = std::move(fe.fe_redo_stack.back());
                    fe.fe_redo_stack.pop_back();
                    fe.selected_ids.clear();
                }
            }
            // Ctrl+A — select all objects in the frame editor
            if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
                fe.selected_ids.clear();
                for (const auto& obj : fe.objects)
                    fe.selected_ids.push_back(obj.id);
                // Switch to Select tool so the selection is immediately usable
                fe.active_tool = FrameEditorState::Tool::Select;
            }
        }
    }
    using AL   = FrameEditorState::ActiveLayer;
    using SM   = FrameEditorState::SymmetryMode;
    using Tool = FrameEditorState::Tool;
    using IT   = FrameEditorState::InterpType;

    // =========================================================================
    //  TOP BAR: layer tabs | tool toolbar | play/record controls
    // =========================================================================
    {
        auto layer_tab = [&](const char* label, AL layer, const char* tooltip) {
            bool active = (fe.active_layer == layer);
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImGui::ColorConvertFloat4ToU32(theme::accent()));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImGui::ColorConvertFloat4ToU32(theme::accent()));
            }
            if (ImGui::Button(label, ImVec2(72.f, 0.f)))
                fe.active_layer = layer;
            if (active) ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
            ImGui::SameLine(0.f, 2.f);
        };
        layer_tab("FX",       AL::FX,       "FX Layer: Global dim + FX stack (intensity and geometry effects)");
        layer_tab("Keyframe", AL::Keyframe, "Keyframe Layer: draw and edit laser vector objects");
    }

    ImGui::SameLine(0.f, 10.f);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0.f, 10.f);

    ImGui::Separator();

    // =========================================================================
    //  CONTENT AREA
    // =========================================================================
    ImVec2 avail      = ImGui::GetContentRegionAvail();
    float  tl_reserve = fe.show_keyframe_editor ? 92.f : 0.f;
    float  content_h  = avail.y - tl_reserve;
    if (content_h < 40.f) content_h = 40.f;

    // ── Combined FX panel (Global + Frame FX in one spreadsheet view) ───────
    if (fe.active_layer == AL::FX) {
        float fx_panel_w = ImGui::GetMainViewport()->WorkSize.x * 2.f / 3.f;
        ImGui::BeginChild("##fe_fxpanel", ImVec2(fx_panel_w, content_h), ImGuiChildFlags_None);

        auto& gl  = ctx.programmer_global;
        auto& fxl = ctx.programmer_fx_layer;

        // ── Global geometry controls ─────────────────────────────────────────
        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("Dim##gl_dim", &gl.global_dim, 0.f, 1.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Global intensity (master dim)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.f);
        ImGui::DragFloat("Size##gl_size", &gl.size, 0.01f, 0.f, 4.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Uniform geometry scale (1=normal)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.f);
        ImGui::DragFloat("Spread##gl_sp", &gl.spread, 0.01f, 0.f, 4.f, "%.2f");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("X-axis stretch (1=normal)");

        ImGui::Spacing();

        // ─────────────────────────────────────────────────────────────────────
        //  GLOBAL FX table  — ChamSys column order: FX | Speed | Size | Spread | Parts | Segs | Dir | Blend | Duty | Width | Del
        // ─────────────────────────────────────────────────────────────────────
        {
            static int s_gl_new_type = 0;
            static const char* s_gl_type_names[] = {
                "Sine","Square","Saw","Triangle","Flicker","RampUp","RampDown","Bump","Chase","Strobe"
            };
            static const char* s_blend_names[] = {"Add","Sub","Abs","Mul"};
            static const char* s_dir_names[]   = {"Sync","Fwd","Rev","C-Out","C-In","Alt","Rand"};

            ImGui::TextUnformatted("Global FX");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.f);
            ImGui::Combo("##gl_new_type", &s_gl_new_type, s_gl_type_names, IM_ARRAYSIZE(s_gl_type_names));
            ImGui::SameLine();
            if (ImGui::SmallButton("+##gl_add")) {
                GlobalFxEntry ne;
                ne.type    = static_cast<GlobalFxType>(s_gl_new_type);
                ne.blend   = FxBlendMode::Absolute;
                ne.rate    = 60.f / 60.f; ne.depth = 1.f; ne.duty = 0.5f;
                ne.offset  = 0.f; ne.parts = 1; ne.segs = 1; ne.enabled = true;
                gl.fx.push_back(ne);
            }

            constexpr ImGuiTableFlags kTbl =
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
            float gl_tbl_h = std::min(28.f + 24.f * (float)std::max(1, (int)gl.fx.size()), 400.f);
            if (ImGui::BeginTable("##glfx_tbl", 13, kTbl, ImVec2(-1.f, gl_tbl_h))) {
                ImGui::TableSetupColumn("En",     ImGuiTableColumnFlags_WidthFixed, 18.f);
                ImGui::TableSetupColumn("FX",     ImGuiTableColumnFlags_WidthFixed, 72.f);
                ImGui::TableSetupColumn("Speed",  ImGuiTableColumnFlags_WidthFixed, 70.f);
                ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("Spread", ImGuiTableColumnFlags_WidthFixed, 46.f);
                ImGui::TableSetupColumn("Parts",  ImGuiTableColumnFlags_WidthFixed, 36.f);
                ImGui::TableSetupColumn("Segs",   ImGuiTableColumnFlags_WidthFixed, 36.f);
                ImGui::TableSetupColumn("Dir",    ImGuiTableColumnFlags_WidthFixed, 52.f);
                ImGui::TableSetupColumn("Blend",  ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("Duty",   ImGuiTableColumnFlags_WidthFixed, 40.f);
                ImGui::TableSetupColumn("Width",  ImGuiTableColumnFlags_WidthFixed, 44.f);
                ImGui::TableSetupColumn("XFade",  ImGuiTableColumnFlags_WidthFixed, 42.f);
                ImGui::TableSetupColumn("##del",  ImGuiTableColumnFlags_WidthFixed, 18.f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                int remove_gl = -1;
                for (int gi = 0; gi < (int)gl.fx.size(); ++gi) {
                    auto& gfe = gl.fx[gi];
                    ImGui::TableNextRow();
                    ImGui::PushID(gi);

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Checkbox("##en", &gfe.enabled);

                    ImGui::TableSetColumnIndex(1);
                    int cur_type = static_cast<int>(gfe.type);
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::Combo("##type", &cur_type, s_gl_type_names, IM_ARRAYSIZE(s_gl_type_names)))
                        gfe.type = static_cast<GlobalFxType>(cur_type);

                    ImGui::TableSetColumnIndex(2);  // Speed (BPM)
                    {
                        float b = gfe.rate * 60.f;  // Hz->BPM  (bpm = hz * 60)
                        float period_s = (gfe.rate > 0.001f) ? (60.f / b) : 999.f;  // period = 60/bpm
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::DragFloat("##spd", &b, 1.f, 1.f, 6000.f, "%.0f BPM"))
                            gfe.rate = b / 60.f;  // Hz = bpm / 60
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Speed: %.0f BPM  |  Period: %.1f ms", b, period_s * 1000.f);
                    }

                    ImGui::TableSetColumnIndex(3);  // Size (depth)
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##sz", &gfe.depth, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Size — FX amplitude");

                    ImGui::TableSetColumnIndex(4);  // Spread (dir_width)
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##spr", &gfe.dir_width, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spread — phase offset across objects\n0=sync  1=full wave");

                    ImGui::TableSetColumnIndex(5);  // Parts
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragInt("##pts", &gfe.parts, 0.1f, 1, 16);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parts — wave cycles across group\n1=one wave  2=two waves");

                    ImGui::TableSetColumnIndex(6);  // Segs
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragInt("##segs", &gfe.segs, 0.1f, 1, 32);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Segs — objects per group (share same phase)\n1=each object independent  2=pairs");

                    ImGui::TableSetColumnIndex(7);  // Direction
                    int cur_dir = static_cast<int>(gfe.direction);
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::Combo("##dir", &cur_dir, s_dir_names, IM_ARRAYSIZE(s_dir_names)))
                        gfe.direction = static_cast<FxDirection>(cur_dir);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Sync=all same  Fwd=forward  Rev=reverse\n"
                        "C-Out=centre out  C-In=centre in  Alt=alternating  Rand=random");

                    ImGui::TableSetColumnIndex(8);  // Blend
                    int cur_blend = static_cast<int>(gfe.blend);
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::Combo("##blend", &cur_blend, s_blend_names, IM_ARRAYSIZE(s_blend_names)))
                        gfe.blend = static_cast<FxBlendMode>(cur_blend);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add/Sub/Abs/Mul blend mode");

                    ImGui::TableSetColumnIndex(9);  // Duty
                    {
                        bool duty_type = gfe.type == GlobalFxType::Square
                                      || gfe.type == GlobalFxType::Chase
                                      || gfe.type == GlobalFxType::Strobe;
                        if (!duty_type) ImGui::BeginDisabled();
                        ImGui::SetNextItemWidth(-1.f);
                        ImGui::DragFloat("##duty", &gfe.duty, 0.01f, 0.f, 1.f, "%.2f");
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duty cycle (Square/Chase/Strobe only)");
                        if (!duty_type) ImGui::EndDisabled();
                    }

                    ImGui::TableSetColumnIndex(10); // Width gate
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##wid", &gfe.width, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Width — fraction of objects in ON state\n1.0=all on  0.1=10% on");

                    ImGui::TableSetColumnIndex(11); // CrossFade
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##xf", &gfe.crossfade, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("CrossFade: 0=snap  1=full smooth");

                    ImGui::TableSetColumnIndex(12);
                    if (ImGui::SmallButton("X")) remove_gl = gi;

                    ImGui::PopID();
                }
                if (remove_gl >= 0) gl.fx.erase(gl.fx.begin() + remove_gl);
                if (gl.fx.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("(empty — use + to add)");
                }
                ImGui::EndTable();
            }
        }

        ImGui::Spacing();

        // ─────────────────────────────────────────────────────────────────────
        //  FRAME FX table — ChamSys column order: FX | Speed | Size | Spread | Parts | Segs | Dir | Ph | Geo Sz | Geo Sp | Width | Del
        // ─────────────────────────────────────────────────────────────────────
        {
            static int s_fx_new_type = 0;
            static const char* s_fx_type_names[] = {
                "PanX","PanY","Rotate","Scale",
                "BounceX","BounceY","ShakeX","ShakeY",
                "ColCycle","ColPulse","RainbowTrl","Spiral",
                "Col2","Col3","ColFlick","Strobe",
                "Col4","Col5","RotateCont"
            };
            static const char* s_fxdir_names[] = {"Sync","Fwd","Rev","C-Out","C-In","Alt","Rand"};

            ImGui::TextUnformatted("Frame FX");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.f);
            ImGui::Combo("##fx_new_type", &s_fx_new_type, s_fx_type_names, IM_ARRAYSIZE(s_fx_type_names));
            ImGui::SameLine();
            if (ImGui::SmallButton("+##fx_add")) {
                FrameFxEntry ne;
                ne.type    = static_cast<FrameFxType>(s_fx_new_type);
                ne.rate    = 1.f; ne.depth = 0.1f;
                ne.offset  = 0.f; ne.parts = 1; ne.segs = 1; ne.enabled = true;
                fxl.fx.push_back(ne);
            }

            constexpr ImGuiTableFlags kTbl =
                ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
            float fx_tbl_h = std::min(28.f + 24.f * (float)std::max(1, (int)fxl.fx.size()), 400.f);
            if (ImGui::BeginTable("##fffx_tbl", 15, kTbl, ImVec2(-1.f, fx_tbl_h))) {
                ImGui::TableSetupColumn("En",     ImGuiTableColumnFlags_WidthFixed, 18.f);
                ImGui::TableSetupColumn("FX",     ImGuiTableColumnFlags_WidthFixed, 80.f);
                ImGui::TableSetupColumn("Speed",  ImGuiTableColumnFlags_WidthFixed, 70.f);
                ImGui::TableSetupColumn("Size",   ImGuiTableColumnFlags_WidthFixed, 44.f);
                ImGui::TableSetupColumn("Spread", ImGuiTableColumnFlags_WidthFixed, 46.f);
                ImGui::TableSetupColumn("Parts",  ImGuiTableColumnFlags_WidthFixed, 36.f);
                ImGui::TableSetupColumn("Segs",   ImGuiTableColumnFlags_WidthFixed, 36.f);
                ImGui::TableSetupColumn("Dir",    ImGuiTableColumnFlags_WidthFixed, 52.f);
                ImGui::TableSetupColumn("Ph",     ImGuiTableColumnFlags_WidthFixed, 38.f);
                ImGui::TableSetupColumn("Geo Sz", ImGuiTableColumnFlags_WidthFixed, 42.f);
                ImGui::TableSetupColumn("Geo Sp", ImGuiTableColumnFlags_WidthFixed, 42.f);
                ImGui::TableSetupColumn("Width",  ImGuiTableColumnFlags_WidthFixed, 44.f);
                ImGui::TableSetupColumn("XFade",  ImGuiTableColumnFlags_WidthFixed, 42.f);
                ImGui::TableSetupColumn("Clr",    ImGuiTableColumnFlags_WidthFixed, 32.f);
                ImGui::TableSetupColumn("##del",  ImGuiTableColumnFlags_WidthFixed, 18.f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                int remove_fx = -1;
                for (int fi = 0; fi < (int)fxl.fx.size(); ++fi) {
                    auto& ffe = fxl.fx[fi];
                    ImGui::TableNextRow();
                    ImGui::PushID(fi + 1000);

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Checkbox("##en", &ffe.enabled);

                    ImGui::TableSetColumnIndex(1);  // FX type
                    int cur_ftype = static_cast<int>(ffe.type);
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::Combo("##type", &cur_ftype, s_fx_type_names, IM_ARRAYSIZE(s_fx_type_names)))
                        ffe.type = static_cast<FrameFxType>(cur_ftype);

                    ImGui::TableSetColumnIndex(2);  // Speed (BPM)
                    {
                        float b = ffe.rate * 60.f;  // Hz->BPM  (bpm = hz * 60)
                        float period_s = (ffe.rate > 0.001f) ? (60.f / b) : 999.f;  // period = 60/bpm
                        ImGui::SetNextItemWidth(-1.f);
                        if (ImGui::DragFloat("##spd", &b, 1.f, 1.f, 6000.f, "%.0f BPM"))
                            ffe.rate = b / 60.f;  // Hz = bpm / 60
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Speed: %.0f BPM  |  Period: %.1f ms", b, period_s * 1000.f);
                    }

                    ImGui::TableSetColumnIndex(3);  // Size (depth/amplitude)
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##sz", &ffe.depth, 0.005f, 0.f, 2.f, "%.3f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Size — FX amplitude (0=none  1=full)");

                    ImGui::TableSetColumnIndex(4);  // Spread (dir_width)
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##spr", &ffe.dir_width, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Spread — phase offset across objects\n0=sync  1=full wave");

                    ImGui::TableSetColumnIndex(5);  // Parts
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragInt("##pts", &ffe.parts, 0.1f, 1, 16);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Parts — wave cycles across group");

                    ImGui::TableSetColumnIndex(6);  // Segs
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragInt("##segs", &ffe.segs, 0.1f, 1, 32);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Segs — objects per group (share same phase)");

                    ImGui::TableSetColumnIndex(7);  // Direction
                    int cur_fxdir = static_cast<int>(ffe.direction);
                    ImGui::SetNextItemWidth(-1.f);
                    if (ImGui::Combo("##dir", &cur_fxdir, s_fxdir_names, IM_ARRAYSIZE(s_fxdir_names)))
                        ffe.direction = static_cast<FxDirection>(cur_fxdir);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Sync=all same  Fwd=forward  Rev=reverse\n"
                        "C-Out=centre out  C-In=centre in  Alt=alternating  Rand=random");

                    ImGui::TableSetColumnIndex(8);  // Phase offset
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##ph", &ffe.offset, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Phase offset (0..1)");

                    ImGui::TableSetColumnIndex(9);  // Geometry size
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##gsz", &ffe.size, 0.01f, 0.f, 4.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Geo Size — uniform scale before FX (1=normal)");

                    ImGui::TableSetColumnIndex(10); // Geometry X-spread
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##gsp", &ffe.spread, 0.01f, 0.f, 4.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Geo Spread — X-axis stretch before FX (1=normal)");

                    ImGui::TableSetColumnIndex(11); // Width gate
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##wid", &ffe.width, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Width — fraction of objects in ON state\n1.0=all on  0.1=10% on");

                    ImGui::TableSetColumnIndex(12); // CrossFade
                    ImGui::SetNextItemWidth(-1.f);
                    ImGui::DragFloat("##xf", &ffe.crossfade, 0.01f, 0.f, 1.f, "%.2f");
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("CrossFade: 0=snap  1=full smooth");

                    ImGui::TableSetColumnIndex(13); // Colors (color FX only)
                    {
                        bool is_color_fx = (ffe.type == FrameFxType::Col2
                                         || ffe.type == FrameFxType::Col3
                                         || ffe.type == FrameFxType::Col4
                                         || ffe.type == FrameFxType::Col5
                                         || ffe.type == FrameFxType::ColFlick
                                         || ffe.type == FrameFxType::ColorCycle
                                         || ffe.type == FrameFxType::ColorPulse
                                         || ffe.type == FrameFxType::RainbowTrail);
                        if (!is_color_fx) { ImGui::BeginDisabled(); }
                        ImVec4 btn_col = ffe.use_custom_colors
                            ? ImVec4(ffe.col_a_r, ffe.col_a_g, ffe.col_a_b, 1.f)
                            : ImVec4(0.4f, 0.4f, 0.4f, 1.f);
                        ImGui::PushStyleColor(ImGuiCol_Button, btn_col);
                        char pop_id[32]; std::snprintf(pop_id, sizeof(pop_id), "##clrpop%d", fi);
                        if (ImGui::SmallButton("C##clr")) ImGui::OpenPopup(pop_id);
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Custom color palette for this FX");
                        if (ImGui::BeginPopup(pop_id)) {
                            ImGui::TextUnformatted("Color FX Palette");
                            ImGui::Separator();
                            ImGui::Checkbox("Use custom colors##ucc", &ffe.use_custom_colors);
                            if (!ffe.use_custom_colors) ImGui::BeginDisabled();
                            ImGui::TextUnformatted("Color A:"); ImGui::SameLine();
                            ImGui::ColorEdit3("##ca", &ffe.col_a_r, ImGuiColorEditFlags_NoLabel);
                            ImGui::TextUnformatted("Color B:"); ImGui::SameLine();
                            ImGui::ColorEdit3("##cb", &ffe.col_b_r, ImGuiColorEditFlags_NoLabel);
                            if (ffe.type == FrameFxType::Col3 || ffe.type == FrameFxType::Col4 || ffe.type == FrameFxType::Col5) {
                                ImGui::TextUnformatted("Color C:"); ImGui::SameLine();
                                ImGui::ColorEdit3("##cc", &ffe.col_c_r, ImGuiColorEditFlags_NoLabel);
                            }
                            if (ffe.type == FrameFxType::Col4 || ffe.type == FrameFxType::Col5) {
                                ImGui::TextUnformatted("Color D:"); ImGui::SameLine();
                                ImGui::ColorEdit3("##cd", &ffe.col_d_r, ImGuiColorEditFlags_NoLabel);
                            }
                            if (ffe.type == FrameFxType::Col5) {
                                ImGui::TextUnformatted("Color E:"); ImGui::SameLine();
                                ImGui::ColorEdit3("##ce", &ffe.col_e_r, ImGuiColorEditFlags_NoLabel);
                            }
                            if (!ffe.use_custom_colors) ImGui::EndDisabled();
                            ImGui::EndPopup();
                        }
                        if (!is_color_fx) { ImGui::EndDisabled(); }
                    }

                    ImGui::TableSetColumnIndex(14);
                    if (ImGui::SmallButton("X")) remove_fx = fi;

                    ImGui::PopID();
                }
                if (remove_fx >= 0) fxl.fx.erase(fxl.fx.begin() + remove_fx);
                if (fxl.fx.empty()) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextDisabled("(empty — use + to add)");
                }
                ImGui::EndTable();
            }
        }

        ImGui::EndChild();
        goto fe_timeline_draw;
    }

    // ── Keyframe layer ────────────────────────────────────────────────────────
    {
        float left_w    = 180.f;
        float toolbar_w = 82.f;   // 2 x 36px cells + borders + padding
        float canvas_w  = avail.x - left_w - toolbar_w - 8.f;
        if (canvas_w < 64.f) canvas_w = 64.f;

        // ── Object list + properties (left sidebar) ───────────────────────────
        ImGui::BeginChild("##fe_left", ImVec2(left_w, content_h),
                          ImGuiChildFlags_Borders);

        ImGui::TextDisabled("Objects (%d)", (int)fe.objects.size());
        ImGui::Separator();

        float list_h = content_h * 0.55f - 24.f;
        if (list_h < 36.f) list_h = 36.f;
        ImGui::BeginChild("##fe_obj_list", ImVec2(-1.f, list_h),
                          ImGuiChildFlags_Borders);

        if (fe.objects.empty()) {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.f);
            ImGui::TextDisabled("(empty)");
            ImGui::TextDisabled("Draw with a tool");
            ImGui::TextDisabled("to add objects.");
        }

        for (int oi = 0; oi < (int)fe.objects.size(); ++oi) {
            auto& obj = fe.objects[oi];
            bool is_sel = false;
            for (auto id : fe.selected_ids) if (id == obj.id) { is_sel = true; break; }

            bool hidden = false;
            for (auto id : fe.hidden_ids) if (id == obj.id) { hidden = true; break; }

            ImGui::PushID((int)oi);

            if (hidden) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,0.4f,0.4f,1.f));

            char row_lbl[64];
            std::snprintf(row_lbl, sizeof(row_lbl), "%s %s %d",
                          fe_type_icon(obj.type), fe_type_name(obj.type), oi + 1);

            bool clicked = ImGui::Selectable(row_lbl, is_sel,
                ImGuiSelectableFlags_None, ImVec2(left_w - 30.f, 0.f));

            if (hidden) ImGui::PopStyleColor();

            if (clicked) {
                if (!ImGui::GetIO().KeyCtrl) fe.selected_ids.clear();
                if (is_sel && ImGui::GetIO().KeyCtrl) {
                    fe.selected_ids.erase(
                        std::remove(fe.selected_ids.begin(), fe.selected_ids.end(), obj.id),
                        fe.selected_ids.end());
                } else {
                    fe.selected_ids.push_back(obj.id);
                    fe.edit_r = obj.r; fe.edit_g = obj.g; fe.edit_b = obj.b;
                    fe.edit_thickness = obj.thickness;
                    fe.edit_size      = obj.size;
                }
            }

            // Eye/hide toggle at right of row
            ImGui::SameLine(left_w - 26.f, 0.f);
            if (ImGui::SmallButton(hidden ? "H##ev" : "V##ev")) {
                if (hidden) {
                    fe.hidden_ids.erase(
                        std::remove(fe.hidden_ids.begin(), fe.hidden_ids.end(), obj.id),
                        fe.hidden_ids.end());
                } else {
                    fe.hidden_ids.push_back(obj.id);
                }
            }

            ImGui::PopID();
        }
        ImGui::EndChild(); // obj list

        // Properties
        ImGui::Separator();
        ImGui::TextDisabled("Properties");

        LaserObject* sel_obj = nullptr;
        for (auto& obj : fe.objects)
            for (auto id : fe.selected_ids)
                if (id == obj.id) { sel_obj = &obj; break; }

        // ── Color mode-aware editor helper ────────────────────────────────────
        auto render_col_edit = [&](float* rgb) -> bool {
            float pw = left_w - 8.f;
            bool changed = false;
            using CM = UIState::ColorInputMode;
            switch (state.color_input_mode) {
            case CM::RGBPercent:
                ImGui::SetNextItemWidth(pw);
                changed = ImGui::ColorEdit3("##col", rgb,
                    ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float |
                    ImGuiColorEditFlags_DisplayRGB);
                break;
            case CM::RGBAbs:
                ImGui::SetNextItemWidth(pw);
                changed = ImGui::ColorEdit3("##col", rgb,
                    ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Uint8 |
                    ImGuiColorEditFlags_DisplayRGB);
                break;
            case CM::CMY: {
                float c = 1.f-rgb[0], m = 1.f-rgb[1], y = 1.f-rgb[2];
                float sw2 = (pw - 4.f) / 3.f;
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.f,0.45f,0.45f,0.7f));
                ImGui::SetNextItemWidth(sw2);
                bool cc = ImGui::DragFloat("##C",&c,0.005f,0.f,1.f,"C%.2f");
                ImGui::PopStyleColor();
                ImGui::SameLine(0,2);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f,0.f,0.45f,0.7f));
                ImGui::SetNextItemWidth(sw2);
                bool mc = ImGui::DragFloat("##M",&m,0.005f,0.f,1.f,"M%.2f");
                ImGui::PopStyleColor();
                ImGui::SameLine(0,2);
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.45f,0.45f,0.f,0.7f));
                ImGui::SetNextItemWidth(sw2);
                bool yc = ImGui::DragFloat("##Y",&y,0.005f,0.f,1.f,"Y%.2f");
                ImGui::PopStyleColor();
                if (cc||mc||yc) { rgb[0]=1.f-c; rgb[1]=1.f-m; rgb[2]=1.f-y; changed=true; }
                break;
            }
            case CM::HSI:
                ImGui::SetNextItemWidth(pw);
                changed = ImGui::ColorEdit3("##col", rgb,
                    ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float |
                    ImGuiColorEditFlags_DisplayHSV);
                break;
            }
            return changed;
        };

        // ── Color settings button + mode label ───────────────────────────────
        {
            static const char* kModeShort[] = {"RGB%","ABS","CMY","HSI"};
            int mi = static_cast<int>(state.color_input_mode);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f,0.2f,0.3f,1.f));
            if (ImGui::SmallButton(kModeShort[mi])) state.color_settings_open = !state.color_settings_open;
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Color input mode — click to open Color Settings");
        }

        if (sel_obj) {
            float col3[3] = { sel_obj->r, sel_obj->g, sel_obj->b };
            if (render_col_edit(col3)) {
                for (auto& obj : fe.objects)
                    for (auto id : fe.selected_ids)
                        if (id == obj.id) { obj.r=col3[0]; obj.g=col3[1]; obj.b=col3[2]; }
                fe.edit_r=col3[0]; fe.edit_g=col3[1]; fe.edit_b=col3[2];
            }
            ImGui::TextDisabled("Color");

            ImGui::SetNextItemWidth(left_w - 8.f);
            if (ImGui::SliderFloat("##fe_thick", &sel_obj->thickness,
                                   0.5f, 5.f, "Thick %.1f")) {
                fe.edit_thickness = sel_obj->thickness;
                for (auto& obj : fe.objects)
                    for (auto id : fe.selected_ids)
                        if (id == obj.id) obj.thickness = sel_obj->thickness;
            }

            if (sel_obj->type == LaserObjectType::Dot ||
                sel_obj->type == LaserObjectType::Circle) {
                ImGui::SetNextItemWidth(left_w - 8.f);
                if (ImGui::SliderFloat("##fe_size", &sel_obj->size, 0.01f, 0.3f, "Size %.3f")) {
                    fe.edit_size = sel_obj->size;
                    for (auto& obj : fe.objects)
                        for (auto id : fe.selected_ids)
                            if (id == obj.id) obj.size = sel_obj->size;
                }
            }

            if (sel_obj->type == LaserObjectType::Text) {
                static char s_text_edit_buf[256] = {};
                if (s_text_edit_buf[0] == '\0') {
                    std::strncpy(s_text_edit_buf, sel_obj->text.c_str(),
                                 sizeof(s_text_edit_buf) - 1);
                    s_text_edit_buf[sizeof(s_text_edit_buf)-1] = '\0';
                }
                ImGui::SetNextItemWidth(left_w - 8.f);
                if (ImGui::InputText("##fe_textedit", s_text_edit_buf,
                                     sizeof(s_text_edit_buf))) {
                    sel_obj->text = s_text_edit_buf;
                }
                ImGui::TextDisabled("Text content");
            }

            ImGui::Dummy(ImVec2(0.f, 4.f));
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f,0.06f,0.06f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f,0.10f,0.10f,1.f));
            if (ImGui::Button("Delete##feds", ImVec2(-1.f, 0.f))) {
                fe_push_undo();
                fe.objects.erase(
                    std::remove_if(fe.objects.begin(), fe.objects.end(),
                        [&](const LaserObject& o) {
                            for (auto id : fe.selected_ids) if (id == o.id) return true;
                            return false;
                        }),
                    fe.objects.end());
                fe.selected_ids.clear();
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::TextDisabled("(nothing selected)");
            float col3[3] = { fe.edit_r, fe.edit_g, fe.edit_b };
            if (render_col_edit(col3)) {
                fe.edit_r=col3[0]; fe.edit_g=col3[1]; fe.edit_b=col3[2];
                fe.draw_color = {col3[0], col3[1], col3[2], 1.f};
            }
            ImGui::TextDisabled("Draw Color");
        }

        // ── Color palette grid — fills remaining left-panel space ─────────────
        {
            static int s_swatch_cols = 8;
            ImGui::SetNextItemWidth(100.f);
            ImGui::SliderInt("Cols##sw", &s_swatch_cols, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SetItemTooltip("Number of columns in the color palette grid.");
            ImGui::Separator();
            float avail_h = ImGui::GetContentRegionAvail().y - 4.f;
            if (avail_h < 40.f) avail_h = 40.f;
            float btn_w = (left_w - 8.f - (s_swatch_cols - 1) * 2.f) / (float)s_swatch_cols;
            float sq    = std::min(btn_w, 48.f);  // keep swatches square regardless of panel width
            for (int i = 0; i < UIState::kNumSwatches; ++i) {
                auto& sw = state.color_swatches[i];
                ImVec4 col = sw.used
                    ? ImVec4(sw.r, sw.g, sw.b, 1.f)
                    : ImVec4(0.12f, 0.12f, 0.14f, 1.f);
                ImGui::PushID(i + 5000);
                ImGui::PushStyleColor(ImGuiCol_Button,
                    state.rem_mode ? ImVec4(col.x*0.6f+0.4f, col.y*0.4f, col.z*0.4f, 1.f) : col);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(std::min(col.x*1.3f+0.1f,1.f), std::min(col.y*1.3f+0.1f,1.f), std::min(col.z*1.3f+0.1f,1.f), 1.f));
                if (ImGui::Button("##sw", ImVec2(sq, sq))) {
                    if (state.rem_mode) {
                        if (i >= 8) sw = {};  // only clear user slots (8+)
                        state.rem_mode = false;
                    } else if (ctx.rec_armed && !sw.used) {
                        // Record current color into this empty slot
                        sw = { fe.edit_r, fe.edit_g, fe.edit_b, true };
                    } else if (sw.used) {
                        float r=sw.r, g=sw.g, b=sw.b;
                        if (!fe.selected_ids.empty()) {
                            for (auto& obj : fe.objects)
                                for (auto id : fe.selected_ids)
                                    if (id == obj.id) { obj.r=r; obj.g=g; obj.b=b; }
                        }
                        fe.edit_r=r; fe.edit_g=g; fe.edit_b=b;
                        fe.draw_color={r,g,b,1.f};
                    }
                }
                if (ImGui::IsItemHovered()) {
                    if (sw.used)
                        ImGui::SetTooltip("R:%.0f%% G:%.0f%% B:%.0f%%\n%s",
                            sw.r*100.f, sw.g*100.f, sw.b*100.f,
                            state.rem_mode ? "Click to REMOVE" : "Click to apply");
                    else if (ctx.rec_armed)
                        ImGui::SetTooltip("Click to RECORD current color here");
                }
                ImGui::PopStyleColor(2);
                ImGui::PopID();
                if ((i + 1) % s_swatch_cols != 0) ImGui::SameLine(0.f, 2.f);
            }
        }

        // ── Color settings floating window ────────────────────────────────────
        if (state.color_settings_open) {
            ImGui::SetNextWindowSize(ImVec2(300.f, 180.f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Color Settings##clrset", &state.color_settings_open,
                             ImGuiWindowFlags_NoCollapse)) {
                ImGui::SeparatorText("Color Input Mode");
                using CM = UIState::ColorInputMode;
                static const char* kModeNames[] = {
                    "RGB % (0..100% per channel, standard)",
                    "RGB Absolute (0..255 integers)",
                    "CMY (Cyan / Magenta / Yellow)",
                    "HSI (Hue / Saturation / Intensity = HSV)"
                };
                for (int mi = 0; mi < 4; ++mi) {
                    bool sel = (static_cast<int>(state.color_input_mode) == mi);
                    if (ImGui::RadioButton(kModeNames[mi], sel))
                        state.color_input_mode = static_cast<CM>(mi);
                }
                ImGui::Spacing();
                ImGui::SeparatorText("Palette");
                ImGui::TextDisabled("Use REC to record current color to an empty slot.");
                ImGui::TextDisabled("Use REM + click a slot to clear it.");
            }
            ImGui::End();
        }

        ImGui::EndChild(); // left panel

        ImGui::SameLine(0.f, 4.f);

        // ── Toolbox sidebar (Photoshop/GIMP-style 2-col icon grid) ───────────
        ImGui::BeginChild("##fe_toolbox", ImVec2(toolbar_w, content_h), ImGuiChildFlags_Borders);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(3.f, 3.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.f, 2.f));

        const float kCellSz = 36.f;

        // Tool button lambda for the sidebar
        auto tbtn = [&](const char* icon, Tool t, const char* tip, const char* shortcut = nullptr) {
            bool active = (fe.active_tool == t);
            ImGui::PushID(static_cast<int>(t));
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.f,   0.898f, 1.f,  0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f,  0.95f,  1.f,  1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.f,   0.f,    0.f,  1.f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.14f, 0.17f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.22f, 0.28f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.85f, 0.88f, 0.90f, 1.f));
            }
            if (ImGui::Button(icon, ImVec2(kCellSz, kCellSz))) {
                fe.active_tool = t;
                fe.wip_points.clear();
                fe.drawing = false;
                fe.lasso_active = false;
                fe.lasso_pts.clear();
                fe.arc_click_count = 0;
                fe.rot_dragging = false;
                fe.scale_dragging = false;
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
            if (ImGui::IsItemHovered()) {
                if (shortcut)
                    ImGui::SetTooltip("%s  [%s]", tip, shortcut);
                else
                    ImGui::SetTooltip("%s", tip);
            }
        };

        // Symmetry button lambda
        auto sbtn = [&](const char* icon, SM mode, const char* tip) {
            bool active = (fe.symmetry == mode);
            ImGui::PushID(1000 + static_cast<int>(mode));
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(1.f,  0.42f, 0.21f, 0.85f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.f,  0.55f, 0.30f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.f,  0.f,   0.f,   1.f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.12f, 0.14f, 0.17f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.22f, 0.28f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.85f, 0.88f, 0.90f, 1.f));
            }
            if (ImGui::Button(icon, ImVec2(kCellSz, kCellSz))) {
                fe.symmetry = mode;
                fe_apply_symmetry_to_objects(fe, 0.f, 0.f);
            }
            ImGui::PopStyleColor(3);
            ImGui::PopID();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };

        // ── Section: Manipulation ─────────────────────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.62f, 1.f));
        ImGui::TextUnformatted("TOOLS");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Row 1: Select / NodeEdit
        tbtn("[V]", Tool::Select,    "Select objects",               "V"); ImGui::SameLine(0.f, 3.f);
        tbtn("<>",  Tool::NodeEdit,  "Node Edit — move control pts", "A");
        // Row 2: Move / Rotate
        tbtn("+",   Tool::Move,      "Move selection",               "G"); ImGui::SameLine(0.f, 3.f);
        tbtn("~>",  Tool::Rotate,    "Rotate",                       "R");
        // Row 3: Scale / Pivot
        tbtn("><",  Tool::Scale,     "Scale",                        "S"); ImGui::SameLine(0.f, 3.f);
        tbtn("(+)", Tool::Pivot,     "Set pivot point");
        // Row 4: GroupScale / PremadeShapes
        tbtn("[]>", Tool::GroupScale,    "Group Scale — bbox handles"); ImGui::SameLine(0.f, 3.f);
        tbtn("~~",  Tool::PremadeShapes, "Premade Shapes — place along curves");

        // ── Section: Draw ─────────────────────────────────────────────────────
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.62f, 1.f));
        ImGui::TextUnformatted("DRAW");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Row: FilledShape / Pen
        tbtn("##", Tool::FilledShape, "Filled Shapes");             ImGui::SameLine(0.f, 3.f);
        tbtn("^",  Tool::Pen,         "Pen (4-pt Bezier)",          "P");
        // Row: Line / Circle
        tbtn("--", Tool::Line,        "Line",                       "L"); ImGui::SameLine(0.f, 3.f);
        tbtn("(O)", Tool::Circle,     "Circle",                     "C");
        // Row: Rect / Arc
        tbtn("[ ]", Tool::Rect,       "Rectangle",                  "Q"); ImGui::SameLine(0.f, 3.f);
        tbtn("(~",  Tool::Arc,        "Arc (3-click)",              "U");
        // Row: Polygon / Bezier
        tbtn("/\\", Tool::Polygon,    "Polygon",                    "O"); ImGui::SameLine(0.f, 3.f);
        tbtn("~^",  Tool::Bezier,     "Bezier Spline",              "B");
        // Row: Star / Dot
        tbtn("*",   Tool::Star,       "Star",                       "*"); ImGui::SameLine(0.f, 3.f);
        tbtn("[.]", Tool::Dot,        "Dot",                        "D");
        // Row: Text / Lasso
        tbtn("Aa",  Tool::Text,       "Text",                       "T"); ImGui::SameLine(0.f, 3.f);
        tbtn("@",   Tool::Lasso,      "Lasso Select");
        // Row: Polyline / ImportImage
        tbtn("/Z",  Tool::Polyline,   "Polyline");                        ImGui::SameLine(0.f, 3.f);
        tbtn("IM",  Tool::ImportImage, "Import Image (raster -> laser vectors)");

        // ── Section: Symmetry ─────────────────────────────────────────────────
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.55f, 0.62f, 1.f));
        ImGui::TextUnformatted("SYM");
        ImGui::PopStyleColor();
        ImGui::Separator();

        // Row: None / MirrorX
        sbtn("NON", SM::None,     "No symmetry");                           ImGui::SameLine(0.f, 3.f);
        sbtn("X",   SM::MirrorX,  "Mirror horizontally (left/right)");
        // Row: MirrorY / MirrorXY
        sbtn("Y",   SM::MirrorY,  "Mirror vertically (top/bottom)");        ImGui::SameLine(0.f, 3.f);
        sbtn("XY",  SM::MirrorXY, "Mirror both axes (4 quadrants)");
        // Row: Radial2 / Radial3
        sbtn("r2",  SM::Radial2,  "2-fold radial symmetry (180 deg)");      ImGui::SameLine(0.f, 3.f);
        sbtn("r3",  SM::Radial3,  "3-fold radial symmetry (120 deg)");
        // Row: Radial4 / Radial6
        sbtn("r4",  SM::Radial4,  "4-fold radial symmetry (90 deg)");       ImGui::SameLine(0.f, 3.f);
        sbtn("r6",  SM::Radial6,  "6-fold radial symmetry (60 deg)");
        // Row: Radial8 / Radial12
        sbtn("r8",  SM::Radial8,  "8-fold radial symmetry (45 deg)");       ImGui::SameLine(0.f, 3.f);
        sbtn("r12", SM::Radial12, "12-fold radial symmetry (30 deg)");

        ImGui::Spacing();
        // ACT/PAS full-width toggle
        {
            bool act = fe.symmetry_act_mode;
            ImGui::PushStyleColor(ImGuiCol_Button,
                act ? ImVec4(0.50f, 0.25f, 0.05f, 1.f) : ImVec4(0.15f, 0.15f, 0.18f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                act ? ImVec4(0.70f, 0.35f, 0.08f, 1.f) : ImVec4(0.25f, 0.25f, 0.30f, 1.f));
            if (ImGui::Button(act ? "ACT##sym_act" : "PAS##sym_act", ImVec2(-1.f, 0.f)))
                fe.symmetry_act_mode = !act;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(act
                    ? "ACT: edits propagate live to all symmetry copies"
                    : "PAS: symmetry applied at draw-time only");
        }

        ImGui::PopStyleVar(2);
        ImGui::EndChild(); // toolbox sidebar

        ImGui::SameLine(0.f, 4.f);

        // ── Main Canvas ───────────────────────────────────────────────────────
        ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
        float  canvas_side = std::min(canvas_w, content_h);
        if (canvas_side < 64.f) canvas_side = 64.f;
        ImVec2 canvas_size = { canvas_side, canvas_side };

        ImGui::InvisibleButton("##fe_canvas", canvas_size,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool canvas_hovered = ImGui::IsItemHovered();
        bool lmb_clicked    = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        bool rmb_clicked    = ImGui::IsItemClicked(ImGuiMouseButton_Right);
        bool lmb_released   = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        ImVec2 mouse_pos    = ImGui::GetIO().MousePos;
        bool ctrl_held      = ImGui::GetIO().KeyCtrl;

        // Coordinate transform: normalised [-1,1] <-> screen
        float pad  = 10.f;
        float dw   = canvas_size.x - 2.f * pad;
        float dh   = canvas_size.y - 2.f * pad;
        float sq   = std::min(dw, dh);
        float ccx  = canvas_pos.x + pad + dw * 0.5f;
        float ccy  = canvas_pos.y + pad + dh * 0.5f;
        float half = sq * 0.5f;

        auto n2s = [&](float nx, float ny) -> ImVec2 {
            return { ccx + nx * half, ccy - ny * half };
        };
        auto s2nx = [&](float sx) -> float { return (sx - ccx) / half; };
        auto s2ny = [&](float sy) -> float { return -(sy - ccy) / half; };

        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(canvas_pos,
            { canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y },
            IM_COL32(2, 2, 6, 255));
        dl->AddRect(canvas_pos,
            { canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y },
            IM_COL32(190, 190, 190, 180));

        // Grid
        {
            float gv[] = { -1.f, -0.5f, 0.f, 0.5f, 1.f };
            for (float g : gv) {
                ImU32 c = (g == 0.f) ? IM_COL32(75,75,98,255) : IM_COL32(40,40,58,200);
                dl->AddLine(n2s(-1.f, g), n2s(1.f, g), c, 1.f);
                dl->AddLine(n2s(g, -1.f), n2s(g, 1.f), c, 1.f);
            }
            // Safe-zone circle
            dl->AddCircle(n2s(0.f,0.f), half * 0.95f, IM_COL32(55,55,78,120), 64, 1.f);
        }

        // Advance animation playback
        if (fe.anim_playing && !fe.anim_frames.empty()) {
            fe.anim_current_time += ImGui::GetIO().DeltaTime;
            if (fe.anim_current_time > fe.anim_duration) fe.anim_current_time = 0.f;
        }

        // Resolve which objects to render (live or interpolated)
        const std::vector<LaserObject>* render_objs = &fe.objects;
        std::vector<LaserObject> interp_objs;
        bool kf_preview = fe.anim_playing ||
            (fe.show_keyframe_editor && (int)fe.anim_frames.size() >= 2);
        if (kf_preview && (int)fe.anim_frames.size() >= 2) {
            const FrameEditorState::AnimFrame* fA = &fe.anim_frames.front();
            const FrameEditorState::AnimFrame* fB = &fe.anim_frames.back();
            float t = fe.anim_current_time;
            for (int fi = 0; fi + 1 < (int)fe.anim_frames.size(); ++fi) {
                if (t >= fe.anim_frames[fi].time_s && t <= fe.anim_frames[fi+1].time_s) {
                    fA = &fe.anim_frames[fi];
                    fB = &fe.anim_frames[fi+1];
                    break;
                }
            }
            float span  = fB->time_s - fA->time_s;
            float alpha = (span > 1e-6f) ? (t - fA->time_s) / span : 0.f;
            interp_objs = fe_interp_anim_frames(*fA, *fB, alpha);
            render_objs = &interp_objs;
        }

        // Draw a single LaserObject
        auto draw_obj = [&](const LaserObject& obj, float alpha_mul, bool is_sel_obj) {
            bool hidden = false;
            for (auto id : fe.hidden_ids) if (id == obj.id) { hidden = true; break; }
            if (hidden) return;

            ImU32 col = IM_COL32(
                (int)(obj.r * 255.f),
                (int)(obj.g * 255.f),
                (int)(obj.b * 255.f),
                (int)(255.f * alpha_mul));
            ImU32 scol = IM_COL32(255, 200, 0, (int)(220.f * alpha_mul));
            float th   = obj.thickness;

            switch (obj.type) {
            case LaserObjectType::Line:
                for (int k = 0; k + 1 < (int)obj.pts.size(); ++k) {
                    ImVec2 a = n2s(obj.pts[k].x, obj.pts[k].y);
                    ImVec2 b = n2s(obj.pts[k+1].x, obj.pts[k+1].y);
                    dl->AddLine(a, b, col, th);
                    if (is_sel_obj) dl->AddLine(a, b, scol, 0.5f);
                }
                break;
            case LaserObjectType::Dot:
                if (!obj.pts.empty()) {
                    ImVec2 sp = n2s(obj.pts[0].x, obj.pts[0].y);
                    float  r  = obj.size * half;
                    dl->AddCircleFilled(sp, r, col);
                    dl->AddCircle(sp, r * 1.65f, IM_COL32(
                        (int)(obj.r*255),(int)(obj.g*255),(int)(obj.b*255),(int)(70*alpha_mul)),
                        24, 1.f);
                    if (is_sel_obj) dl->AddCircle(sp, r + 5.f, scol, 24, 1.5f);
                }
                break;
            case LaserObjectType::Circle:
                if (obj.pts.size() >= 2) {
                    ImVec2 ctr = n2s(obj.pts[0].x, obj.pts[0].y);
                    float  dx  = obj.pts[1].x - obj.pts[0].x;
                    float  dy  = obj.pts[1].y - obj.pts[0].y;
                    float  r   = std::sqrt(dx*dx + dy*dy) * half;
                    dl->AddCircle(ctr, r, col, 64, th);
                    if (is_sel_obj) dl->AddCircle(ctr, r, scol, 64, 0.5f);
                }
                break;
            case LaserObjectType::Bezier:
                for (int seg = 0; seg + 3 < (int)obj.pts.size(); seg += 3) {
                    ImVec2 p0 = n2s(obj.pts[seg+0].x, obj.pts[seg+0].y);
                    ImVec2 p1 = n2s(obj.pts[seg+1].x, obj.pts[seg+1].y);
                    ImVec2 p2 = n2s(obj.pts[seg+2].x, obj.pts[seg+2].y);
                    ImVec2 p3 = n2s(obj.pts[seg+3].x, obj.pts[seg+3].y);
                    dl->AddBezierCubic(p0, p1, p2, p3, col, th, 32);
                    if (is_sel_obj) dl->AddBezierCubic(p0, p1, p2, p3, scol, 0.5f, 32);
                    dl->AddLine(p0, p1, IM_COL32(75,75,115,(int)(90*alpha_mul)), 1.f);
                    dl->AddLine(p3, p2, IM_COL32(75,75,115,(int)(90*alpha_mul)), 1.f);
                }
                break;
            case LaserObjectType::Arc:
                if (obj.pts.size() >= 3) {
                    ImVec2 cen = n2s(obj.pts[0].x, obj.pts[0].y);
                    float rdx = obj.pts[1].x - obj.pts[0].x;
                    float rdy = obj.pts[1].y - obj.pts[0].y;
                    float rn  = std::sqrt(rdx*rdx + rdy*rdy);
                    float a0  = std::atan2(-(obj.pts[1].y-obj.pts[0].y),
                                            obj.pts[1].x-obj.pts[0].x);
                    float a1  = std::atan2(-(obj.pts[2].y-obj.pts[0].y),
                                            obj.pts[2].x-obj.pts[0].x);
                    dl->PathArcTo(cen, rn * half, a0, a1, 32);
                    dl->PathStroke(col, ImDrawFlags_None, th);
                }
                break;
            case LaserObjectType::Text:
                if (!obj.pts.empty()) {
                    ImVec2 sp = n2s(obj.pts[0].x, obj.pts[0].y);
                    dl->AddText(sp, col, obj.text.c_str());
                    if (is_sel_obj) {
                        ImVec2 ts = ImGui::CalcTextSize(obj.text.c_str());
                        dl->AddRect({sp.x-2.f,sp.y-2.f},
                                    {sp.x+ts.x+2.f,sp.y+ts.y+2.f}, scol, 0.f, 0, 1.f);
                    }
                }
                break;
            default: break;
            }

            // Control point handles for selected objects
            if (is_sel_obj) {
                for (const auto& pt : obj.pts) {
                    ImVec2 sp = n2s(pt.x, pt.y);
                    dl->AddRectFilled({sp.x-4.f,sp.y-4.f},{sp.x+4.f,sp.y+4.f},
                                      IM_COL32(255,200,0,200));
                    dl->AddRect({sp.x-4.f,sp.y-4.f},{sp.x+4.f,sp.y+4.f},
                                IM_COL32(0,0,0,180));
                }
            }
        };

        // Render all objects
        for (const auto& obj : *render_objs) {
            bool is_sel = false;
            if (!kf_preview)
                for (auto id : fe.selected_ids) if (id == obj.id) { is_sel = true; break; }
            draw_obj(obj, 1.f, is_sel);
        }

        // Live Premade Shapes preview — ghost overlay on canvas
        if (fe.active_tool == Tool::PremadeShapes &&
            !fe.premade_shapes.preview_pts.empty())
        {
            const PointBuffer& pprev = fe.premade_shapes.preview_pts;
            for (size_t pi = 0; pi + 1 < pprev.size(); ++pi) {
                const auto& pa = pprev[pi];
                const auto& pb = pprev[pi + 1];
                if (pa.blanked || pb.blanked) continue;
                ImVec2 sa = n2s(pa.nx(), pa.ny());
                ImVec2 sb = n2s(pb.nx(), pb.ny());
                dl->AddLine(sa, sb, IM_COL32(0, 220, 180, 150), 1.5f);
            }
            for (const auto& pp : pprev) {
                if (!pp.blanked)
                    dl->AddCircleFilled(n2s(pp.nx(), pp.ny()), 2.f,
                                        IM_COL32(0, 255, 200, 120));
            }
        }

        // Empty canvas placeholder — shown when no objects exist and no WIP in progress
        if (render_objs->empty() && fe.wip_points.empty() && !fe.lasso_active) {
            const char* hint = (fe.active_tool == FrameEditorState::Tool::Select
                                || fe.active_tool == FrameEditorState::Tool::NodeEdit
                                || fe.active_tool == FrameEditorState::Tool::Move
                                || fe.active_tool == FrameEditorState::Tool::Rotate
                                || fe.active_tool == FrameEditorState::Tool::Scale
                                || fe.active_tool == FrameEditorState::Tool::Pivot
                                || fe.active_tool == FrameEditorState::Tool::GroupScale)
                ? "No objects -- select a draw tool (Pen, Line, Circle...) and click to draw"
                : "No objects -- click on the canvas to start drawing";
            ImVec2 ts   = ImGui::CalcTextSize(hint);
            ImVec2 cp   = ImVec2(canvas_pos.x + (canvas_size.x - ts.x) * 0.5f,
                                 canvas_pos.y + (canvas_size.y - ts.y) * 0.5f);
            // Clamp so text stays inside canvas even when narrow
            cp.x = std::max(cp.x, canvas_pos.x + 4.f);
            cp.y = std::max(cp.y, canvas_pos.y + 4.f);
            dl->AddText(cp, IM_COL32(90, 90, 110, 200), hint);
        }

        // Box selection overlay
        if (fe.box_selecting) {
            ImVec2 bs0 = n2s(fe.box_x0, fe.box_y0);
            ImVec2 bs1 = n2s(fe.box_x1, fe.box_y1);
            float bx0 = std::min(bs0.x,bs1.x), by0 = std::min(bs0.y,bs1.y);
            float bx1 = std::max(bs0.x,bs1.x), by1 = std::max(bs0.y,bs1.y);
            dl->AddRectFilled({bx0,by0},{bx1,by1}, IM_COL32(0,180,255,18));
            dl->AddRect({bx0,by0},{bx1,by1}, IM_COL32(0,180,255,200), 0.f, 0, 1.f);
        }

        // Symmetry center crosshair
        if (fe.symmetry != SM::None) {
            ImVec2 sp = n2s(fe.symmetry_center_x, fe.symmetry_center_y);
            float arm = 10.f;
            ImU32 sc  = IM_COL32(255,200,50,200);
            dl->AddLine({sp.x-arm,sp.y},{sp.x+arm,sp.y}, sc, 1.f);
            dl->AddLine({sp.x,sp.y-arm},{sp.x,sp.y+arm}, sc, 1.f);
            dl->AddCircle(sp, 4.f, sc, 0, 1.f);
        }

        // WIP points + rubber-band + ghost symmetry copies
        if (!fe.wip_points.empty()) {
            ImU32 wc = IM_COL32(0,230,255,200);
            for (const auto& wp : fe.wip_points)
                dl->AddCircleFilled(n2s(wp.x,wp.y), 4.f, wc);
            for (int k = 0; k+1 < (int)fe.wip_points.size(); ++k)
                dl->AddLine(n2s(fe.wip_points[k].x,fe.wip_points[k].y),
                            n2s(fe.wip_points[k+1].x,fe.wip_points[k+1].y), wc, 1.5f);
            if (canvas_hovered) {
                ImVec2 last = n2s(fe.wip_points.back().x, fe.wip_points.back().y);
                dl->AddLine(last, mouse_pos, IM_COL32(0,230,255,100), 1.f);
            }
            if (fe.symmetry != SM::None) {
                std::vector<ImVec2> v2;
                for (const auto& w : fe.wip_points) v2.push_back({w.x,w.y});
                auto ss = apply_symmetry(v2, fe.symmetry,
                                         fe.symmetry_center_x, fe.symmetry_center_y);
                ImU32 gc = IM_COL32(0,230,255,76);
                for (int si = 1; si < (int)ss.size(); ++si) {
                    const auto& g = ss[si];
                    for (const auto& gp : g) dl->AddCircleFilled(n2s(gp.x,gp.y), 3.f, gc);
                    for (int k = 0; k+1 < (int)g.size(); ++k)
                        dl->AddLine(n2s(g[k].x,g[k].y), n2s(g[k+1].x,g[k+1].y), gc, 1.f);
                    if (canvas_hovered && !g.empty())
                        dl->AddLine(n2s(g.back().x,g.back().y), mouse_pos,
                                    IM_COL32(0,230,255,38), 1.f);
                }
            }
        }

        // Onion skin — draw prev frame (blue tint) and next frame (orange tint)
        if (fe.show_onion && (int)fe.anim_frames.size() >= 2) {
            const FrameEditorState::AnimFrame* prev_af = nullptr;
            const FrameEditorState::AnimFrame* next_af = nullptr;
            float ct = fe.anim_current_time;
            // Find frames bracketing current time
            for (int fi = 0; fi < (int)fe.anim_frames.size(); ++fi) {
                float ft = fe.anim_frames[fi].time_s;
                if (ft < ct) {
                    if (!prev_af || ft > prev_af->time_s) prev_af = &fe.anim_frames[fi];
                } else if (ft > ct) {
                    if (!next_af || ft < next_af->time_s) next_af = &fe.anim_frames[fi];
                }
            }
            float on_alpha = fe.onion_alpha * 0.25f;
            int   on_a     = (int)(on_alpha * 255.f);
            if (prev_af) {
                for (const auto& obj : prev_af->objects) {
                    for (int k = 0; k+1 < (int)obj.pts.size(); ++k) {
                        ImVec2 a = n2s(obj.pts[k].x, obj.pts[k].y);
                        ImVec2 b = n2s(obj.pts[k+1].x, obj.pts[k+1].y);
                        dl->AddLine(a, b, IM_COL32(80, 140, 255, on_a), 1.f);
                    }
                    if (!obj.pts.empty()) {
                        ImVec2 sp = n2s(obj.pts[0].x, obj.pts[0].y);
                        dl->AddCircleFilled(sp, 2.5f, IM_COL32(80, 140, 255, on_a));
                    }
                }
            }
            if (next_af) {
                for (const auto& obj : next_af->objects) {
                    for (int k = 0; k+1 < (int)obj.pts.size(); ++k) {
                        ImVec2 a = n2s(obj.pts[k].x, obj.pts[k].y);
                        ImVec2 b = n2s(obj.pts[k+1].x, obj.pts[k+1].y);
                        dl->AddLine(a, b, IM_COL32(255, 160, 50, on_a), 1.f);
                    }
                    if (!obj.pts.empty()) {
                        ImVec2 sp = n2s(obj.pts[0].x, obj.pts[0].y);
                        dl->AddCircleFilled(sp, 2.5f, IM_COL32(255, 160, 50, on_a));
                    }
                }
            }
        } else if (fe.show_onion && !state.preview_points.empty()) {
            // Fallback: draw live preview points at low opacity
            ImU32 oc = IM_COL32(255,255,255,(int)(fe.onion_alpha * 100.f));
            for (const auto& lpt : state.preview_points) {
                if (lpt.blanked) continue;
                dl->AddCircleFilled(n2s(lpt.nx(), lpt.ny()), 1.5f, oc);
            }
        }

        // ── Lasso polygon in-progress overlay ────────────────────────────────────
        if (fe.lasso_active && (int)fe.lasso_pts.size() >= 2) {
            for (int k = 0; k+1 < (int)fe.lasso_pts.size(); ++k) {
                ImVec2 la = n2s(fe.lasso_pts[k].x, fe.lasso_pts[k].y);
                ImVec2 lb = n2s(fe.lasso_pts[k+1].x, fe.lasso_pts[k+1].y);
                float ldx = lb.x-la.x, ldy = lb.y-la.y;
                float llen = std::sqrt(ldx*ldx+ldy*ldy);
                if (llen > 0.f) {
                    float step = 6.f;
                    for (float lt = 0.f; lt < llen; lt += step*2.f) {
                        float lt2 = std::min(lt+step, llen);
                        dl->AddLine(
                            {la.x+ldx*(lt/llen),  la.y+ldy*(lt/llen)},
                            {la.x+ldx*(lt2/llen), la.y+ldy*(lt2/llen)},
                            IM_COL32(50,220,80,200), 1.5f);
                    }
                }
            }
            if (canvas_hovered && !fe.lasso_pts.empty()) {
                ImVec2 llast = n2s(fe.lasso_pts.back().x, fe.lasso_pts.back().y);
                dl->AddLine(llast, mouse_pos, IM_COL32(50,220,80,80), 1.f);
            }
        }

        // ── Rotate tool gizmo overlay ─────────────────────────────────────────────
        if (fe.active_tool == Tool::Rotate && !fe.selected_ids.empty()) {
            float gbbx0=1e9f, gbby0=1e9f, gbbx1=-1e9f, gbby1=-1e9f;
            for (const auto& obj : fe.objects) {
                bool sel = false;
                for (auto id : fe.selected_ids) if (id==obj.id) { sel=true; break; }
                if (!sel) continue;
                for (const auto& pt : obj.pts) {
                    if (pt.x<gbbx0) gbbx0=pt.x; if (pt.x>gbbx1) gbbx1=pt.x;
                    if (pt.y<gbby0) gbby0=pt.y; if (pt.y>gbby1) gbby1=pt.y;
                }
            }
            if (!fe.pivot_custom && gbbx0 < 1e9f) {
                fe.pivot_x = (gbbx0+gbbx1)*0.5f;
                fe.pivot_y = (gbby0+gbby1)*0.5f;
            }
            ImVec2 piv = n2s(fe.pivot_x, fe.pivot_y);
            float bbd = std::sqrt((gbbx1-gbbx0)*(gbbx1-gbbx0)+(gbby1-gbby0)*(gbby1-gbby0));
            float bbr = std::max(bbd*0.5f*half+20.f, 18.f);
            int nsegs = 48;
            for (int k = 0; k < nsegs; ++k) {
                if (k % 4 == 3) continue;
                float ga0 = (float)k     / (float)nsegs * 6.28318f;
                float ga1 = (float)(k+1) / (float)nsegs * 6.28318f;
                dl->AddLine(
                    {piv.x+bbr*cosf(ga0), piv.y+bbr*sinf(ga0)},
                    {piv.x+bbr*cosf(ga1), piv.y+bbr*sinf(ga1)},
                    IM_COL32(255,200,0,160), 1.f);
            }
            ImVec2 hdl = {piv.x, piv.y - bbr};
            dl->AddCircleFilled(hdl, 6.f, IM_COL32(255,200,0,220));
            dl->AddCircle(hdl, 6.f, IM_COL32(0,0,0,180));
            float garm = 8.f;
            dl->AddLine({piv.x-garm,piv.y},{piv.x+garm,piv.y}, IM_COL32(255,200,0,200), 1.5f);
            dl->AddLine({piv.x,piv.y-garm},{piv.x,piv.y+garm}, IM_COL32(255,200,0,200), 1.5f);
            dl->AddCircle(piv, 4.f, IM_COL32(255,200,0,200), 0, 1.f);
        }

        // ── Scale tool handles overlay ─────────────────────────────────────────────
        if (fe.active_tool == Tool::Scale && !fe.selected_ids.empty()) {
            float sbbx0=1e9f, sbby0=1e9f, sbbx1=-1e9f, sbby1=-1e9f;
            for (const auto& obj : fe.objects) {
                bool sel = false;
                for (auto id : fe.selected_ids) if (id==obj.id) { sel=true; break; }
                if (!sel) continue;
                for (const auto& pt : obj.pts) {
                    if (pt.x<sbbx0) sbbx0=pt.x; if (pt.x>sbbx1) sbbx1=pt.x;
                    if (pt.y<sbby0) sbby0=pt.y; if (pt.y>sbby1) sbby1=pt.y;
                }
            }
            if (sbbx0 < 1e9f) {
                float scx = (sbbx0+sbbx1)*0.5f, scy = (sbby0+sbby1)*0.5f;
                float shpts[8][2] = {
                    {sbbx0,sbby0},{scx,sbby0},{sbbx1,sbby0},
                    {sbbx1,scy},
                    {sbbx1,sbby1},{scx,sbby1},{sbbx0,sbby1},
                    {sbbx0,scy}
                };
                ImVec2 ss0 = n2s(sbbx0,sbby0), ss1 = n2s(sbbx1,sbby1);
                dl->AddRect(ss0, ss1, IM_COL32(255,200,0,100), 0.f, 0, 1.f);
                for (int h = 0; h < 8; ++h) {
                    ImVec2 sp = n2s(shpts[h][0], shpts[h][1]);
                    dl->AddRectFilled({sp.x-4.f,sp.y-4.f},{sp.x+4.f,sp.y+4.f}, IM_COL32(255,200,0,220));
                    dl->AddRect({sp.x-4.f,sp.y-4.f},{sp.x+4.f,sp.y+4.f}, IM_COL32(0,0,0,180));
                }
            }
        }

        // ── Pivot tool gizmo ──────────────────────────────────────────────────────
        if (fe.active_tool == Tool::Pivot) {
            ImVec2 pp = n2s(fe.pivot_x, fe.pivot_y);
            float parm = 10.f;
            dl->AddLine({pp.x-parm,pp.y},{pp.x+parm,pp.y}, IM_COL32(255,150,0,220), 2.f);
            dl->AddLine({pp.x,pp.y-parm},{pp.x,pp.y+parm}, IM_COL32(255,150,0,220), 2.f);
            dl->AddCircle(pp, parm*0.6f, IM_COL32(255,150,0,220), 0, 1.5f);
        }

        // ── GroupScale tool gizmo overlay ─────────────────────────────────────────
        if (fe.active_tool == Tool::GroupScale && !fe.selected_ids.empty()) {
            // Compute live bounding box
            float gsbx0=1e9f,gsby0=1e9f,gsbx1=-1e9f,gsby1=-1e9f;
            for (const auto& obj : fe.objects) {
                bool sel=false;
                for (auto id:fe.selected_ids) if(id==obj.id){sel=true;break;}
                if (!sel) continue;
                for (const auto& pt:obj.pts) {
                    if(pt.x<gsbx0)gsbx0=pt.x; if(pt.x>gsbx1)gsbx1=pt.x;
                    if(pt.y<gsby0)gsby0=pt.y; if(pt.y>gsby1)gsby1=pt.y;
                }
            }
            if (gsbx0 < 1e9f) {
                float pad_n = 0.03f; // small canvas-space padding around bbox
                gsbx0-=pad_n; gsby0-=pad_n; gsbx1+=pad_n; gsby1+=pad_n;
                float gscx = (gsbx0+gsbx1)*0.5f, gscy = (gsby0+gsby1)*0.5f;
                // 8 handle positions: 0=TL,1=TR,2=BR,3=BL, 4=T,5=R,6=B,7=L
                float gshx[8] = {gsbx0,gsbx1,gsbx1,gsbx0, gscx,gsbx1,gscx,gsbx0};
                float gshy[8] = {gsby1,gsby1,gsby0,gsby0, gsby1,gscy,gsby0,gscy};
                // Draw dashed bbox outline
                ImVec2 gs_tl = n2s(gsbx0,gsby1), gs_tr = n2s(gsbx1,gsby1);
                ImVec2 gs_br = n2s(gsbx1,gsby0), gs_bl = n2s(gsbx0,gsby0);
                float dash=6.f, gap=4.f;
                auto draw_dashed = [&](ImVec2 a, ImVec2 b, ImU32 col) {
                    float dx=b.x-a.x, dy=b.y-a.y;
                    float len=std::sqrt(dx*dx+dy*dy);
                    if(len<1e-3f) return;
                    float ux=dx/len, uy=dy/len;
                    float t=0.f;
                    while(t<len) {
                        float t1=std::min(t+dash,len);
                        dl->AddLine({a.x+ux*t,a.y+uy*t},{a.x+ux*t1,a.y+uy*t1},col,1.5f);
                        t+=dash+gap;
                    }
                };
                ImU32 gc = IM_COL32(0,230,200,200);
                draw_dashed(gs_tl,gs_tr,gc);
                draw_dashed(gs_tr,gs_br,gc);
                draw_dashed(gs_br,gs_bl,gc);
                draw_dashed(gs_bl,gs_tl,gc);
                // Draw 8 handles
                for (int h=0; h<8; ++h) {
                    ImVec2 sp = n2s(gshx[h],gshy[h]);
                    bool is_corner = (h<4);
                    ImU32 hc = is_corner ? IM_COL32(0,230,200,230) : IM_COL32(120,230,200,200);
                    dl->AddCircleFilled(sp, 5.f, hc);
                    dl->AddCircle(sp, 5.f, IM_COL32(0,0,0,200), 0, 1.f);
                }
            }
        }

        // ── Canvas input handling ─────────────────────────────────────────────────
        bool canvas_input_active = (canvas_hovered ||
            (fe.obj_dragging   && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
            (fe.box_selecting  && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
            (fe.rot_dragging   && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
            (fe.scale_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
            (fe.gs_dragging    && ImGui::IsMouseDown(ImGuiMouseButton_Left)) ||
            (fe.lasso_active   && ImGui::IsMouseDown(ImGuiMouseButton_Left)));

        if (canvas_input_active) {
            float nmx = s2nx(mouse_pos.x);
            float nmy = s2ny(mouse_pos.y);

            // Helper: create and register a new LaserObject
            auto add_obj = [&](LaserObjectType type, const std::vector<ImVec2>& v2pts) {
                fe_push_undo();
                LaserObject obj;
                obj.id        = fe.next_object_id++;
                obj.type      = type;
                obj.r         = fe.edit_r;
                obj.g         = fe.edit_g;
                obj.b         = fe.edit_b;
                obj.thickness = fe.edit_thickness;
                obj.size      = fe.edit_size;
                for (const auto& p : v2pts) obj.pts.push_back({p.x, p.y});
                fe.objects.push_back(obj);
                fe.selected_ids = { obj.id };
            };

            // Ctrl+click -> set symmetry center
            if (ctrl_held && lmb_clicked && fe.symmetry != SM::None) {
                fe.symmetry_center_x = nmx;
                fe.symmetry_center_y = nmy;
            } else {
                // Global keyboard shortcuts (canvas focus)
                if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
                    fe.active_tool = Tool::Select;
                    fe.wip_points.clear(); fe.drawing = false;
                    fe.lasso_active = false; fe.lasso_pts.clear();
                }
                // A (without Ctrl) -> Node Edit tool; Ctrl+A is handled above
                if (!ctrl_held && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
                    fe.active_tool = Tool::NodeEdit;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                // Move: G (primary) or W (legacy)
                if (ImGui::IsKeyPressed(ImGuiKey_G, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_W, false)) {
                    fe.active_tool = Tool::Move;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                // Rotate: R (primary) or E (legacy)
                if (ImGui::IsKeyPressed(ImGuiKey_R, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_E, false)) {
                    fe.active_tool = Tool::Rotate;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
                    fe.active_tool = Tool::Scale;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                    fe.active_tool = Tool::Pivot;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_P, false)) {
                    fe.active_tool = Tool::Pen;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_L, false)) {
                    fe.active_tool = Tool::Line;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
                    fe.active_tool = Tool::Circle;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
                    fe.active_tool = Tool::Rect;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_U, false)) {
                    fe.active_tool = Tool::Arc;
                    fe.wip_points.clear(); fe.drawing = false; fe.arc_click_count = 0;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
                    fe.active_tool = Tool::Polygon;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_B, false)) {
                    fe.active_tool = Tool::Bezier;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_D, false)) {
                    fe.active_tool = Tool::Dot;
                    fe.wip_points.clear(); fe.drawing = false;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_T, false)) {
                    fe.active_tool = Tool::Text;
                    fe.wip_points.clear(); fe.drawing = false;
                }

                // Delete / Backspace -> delete selected
                if (!fe.selected_ids.empty() &&
                    (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                     ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
                    fe_push_undo();
                    fe.objects.erase(
                        std::remove_if(fe.objects.begin(), fe.objects.end(),
                            [&](const LaserObject& o) {
                                for (auto id : fe.selected_ids) if (id==o.id) return true;
                                return false;
                            }),
                        fe.objects.end());
                    fe.selected_ids.clear();
                }

                // Escape -> deselect / cancel WIP
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    fe.selected_ids.clear();
                    fe.wip_points.clear(); fe.drawing = false;
                    fe.lasso_active = false; fe.lasso_pts.clear();
                    fe.arc_click_count = 0;
                }

                // Ctrl+D -> duplicate selected
                if (ctrl_held && ImGui::IsKeyPressed(ImGuiKey_D, false) &&
                    !fe.selected_ids.empty()) {
                    fe_push_undo();
                    std::vector<LaserObject> dups;
                    for (const auto& obj : fe.objects) {
                        bool sel = false;
                        for (auto id : fe.selected_ids) if (id==obj.id) { sel=true; break; }
                        if (!sel) continue;
                        LaserObject d = obj;
                        d.id = fe.next_object_id++;
                        for (auto& pt : d.pts) { pt.x += 0.04f; pt.y -= 0.04f; }
                        dups.push_back(d);
                    }
                    fe.selected_ids.clear();
                    for (const auto& d : dups) {
                        fe.objects.push_back(d);
                        fe.selected_ids.push_back(d.id);
                    }
                }

                // Copy / Paste
                static std::vector<LaserObject> fe_clipboard_;
                {
                    ImGuiIO& io = ImGui::GetIO();
                    // Copy selected objects
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) && !fe.selected_ids.empty()) {
                        fe_clipboard_ = {};
                        for (auto& obj : fe.objects) {
                            bool sel = std::find(fe.selected_ids.begin(), fe.selected_ids.end(), obj.id) != fe.selected_ids.end();
                            if (sel) fe_clipboard_.push_back(obj);
                        }
                    }
                    // Paste with offset
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !fe_clipboard_.empty()) {
                        fe_push_undo();
                        fe.selected_ids.clear();
                        for (auto copy : fe_clipboard_) {
                            copy.id = fe.next_object_id++;
                            for (auto& pt : copy.pts) { pt.x += 0.05f; pt.y += 0.05f; }
                            fe.selected_ids.push_back(copy.id);
                            fe.objects.push_back(copy);
                        }
                    }
                }

                // F -> fit view
                if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
                    fe.canvas_origin = {0.f, 0.f};
                    fe.canvas_zoom   = 1.f;
                }

                // X/Y axis constraint for Move tool
                if (fe.active_tool == Tool::Move) {
                    if (ImGui::IsKeyPressed(ImGuiKey_X, false))
                        fe.move_constrain_x = !fe.move_constrain_x;
                    if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                        fe.move_constrain_y = !fe.move_constrain_y;
                }

                // Arrow key nudge when Move tool is active and objects are selected
                if (fe.active_tool == Tool::Move && !fe.selected_ids.empty()) {
                    // Shift = coarse nudge (500 units), normal = fine nudge (50 units)
                    // Values in normalised -1..1 space (32767 = full range)
                    const float kNudgeFine   = 50.f  / 32767.f;
                    const float kNudgeCoarse = 500.f / 32767.f;
                    const float kNudge = ImGui::GetIO().KeyShift ? kNudgeCoarse : kNudgeFine;
                    float dx = 0.f, dy = 0.f;
                    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow,  true)) dx = -kNudge;
                    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) dx =  kNudge;
                    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,    true)) dy =  kNudge;
                    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow,  true)) dy = -kNudge;
                    if (dx != 0.f || dy != 0.f) {
                        fe_push_undo();
                        for (auto& obj : fe.objects) {
                            bool is_sel = false;
                            for (auto sid : fe.selected_ids)
                                if (sid == obj.id) { is_sel = true; break; }
                            if (!is_sel) continue;
                            for (auto& pt : obj.pts) {
                                pt.x = std::clamp(pt.x + dx, -1.f, 1.f);
                                pt.y = std::clamp(pt.y + dy, -1.f, 1.f);
                            }
                        }
                    }
                }

                // SELECT tool
                if (fe.active_tool == Tool::Select) {
                    if (lmb_clicked) {
                        float best_d = 14.f;
                        int   best_i = -1;
                        for (int oi = 0; oi < (int)fe.objects.size(); ++oi) {
                            const auto& o = fe.objects[oi];
                            bool hid = false;
                            for (auto id : fe.hidden_ids) if (id==o.id) { hid=true; break; }
                            if (hid) continue;
                            for (const auto& pt : o.pts) {
                                ImVec2 sp = n2s(pt.x, pt.y);
                                float  dx = sp.x - mouse_pos.x, dy = sp.y - mouse_pos.y;
                                float  d  = std::sqrt(dx*dx + dy*dy);
                                if (d < best_d) { best_d = d; best_i = oi; }
                            }
                        }
                        if (best_i >= 0) {
                            if (!ctrl_held) fe.selected_ids.clear();
                            fe.selected_ids.push_back(fe.objects[best_i].id);
                            auto& o = fe.objects[best_i];
                            fe.edit_r=o.r; fe.edit_g=o.g; fe.edit_b=o.b;
                            fe.edit_thickness=o.thickness; fe.edit_size=o.size;
                            fe_push_undo();
                            fe.obj_dragging      = true;
                            fe.obj_drag_start_mx = nmx;
                            fe.obj_drag_start_my = nmy;
                            fe.obj_drag_orig_pts.clear();
                            for (auto id : fe.selected_ids)
                                for (const auto& obj : fe.objects)
                                    if (obj.id == id)
                                        fe.obj_drag_orig_pts.push_back(obj.pts);
                        } else {
                            if (!ctrl_held) fe.selected_ids.clear();
                            fe.box_selecting = true;
                            fe.box_x0 = nmx; fe.box_y0 = nmy;
                            fe.box_x1 = nmx; fe.box_y1 = nmy;
                        }
                    }
                    if (fe.box_selecting) { fe.box_x1 = nmx; fe.box_y1 = nmy; }
                    if (fe.obj_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        float ddx = nmx - fe.obj_drag_start_mx;
                        float ddy = nmy - fe.obj_drag_start_my;
                        int di = 0;
                        for (auto id : fe.selected_ids) {
                            for (auto& obj : fe.objects) {
                                if (obj.id == id && di < (int)fe.obj_drag_orig_pts.size()) {
                                    const auto& orig = fe.obj_drag_orig_pts[di];
                                    obj.pts.resize(orig.size());
                                    for (int k = 0; k < (int)orig.size(); ++k) {
                                        obj.pts[k].x = orig[k].x + ddx;
                                        obj.pts[k].y = orig[k].y + ddy;
                                    }
                                    ++di;
                                }
                            }
                        }
                    }
                    if (lmb_released) {
                        if (fe.box_selecting) {
                            float bx0 = std::min(fe.box_x0,fe.box_x1);
                            float bx1b= std::max(fe.box_x0,fe.box_x1);
                            float by0 = std::min(fe.box_y0,fe.box_y1);
                            float by1b= std::max(fe.box_y0,fe.box_y1);
                            for (const auto& obj : fe.objects) {
                                for (const auto& pt : obj.pts) {
                                    if (pt.x>=bx0&&pt.x<=bx1b&&pt.y>=by0&&pt.y<=by1b) {
                                        fe.selected_ids.push_back(obj.id);
                                        break;
                                    }
                                }
                            }
                            fe.box_selecting = false;
                        }
                        fe.obj_dragging = false;
                    }
                }
                // NODE EDIT tool
                else if (fe.active_tool == Tool::NodeEdit) {
                    if (lmb_clicked) {
                        float best_d = 8.f;
                        int   best_oi = -1, best_pi = -1;
                        for (int oi = 0; oi < (int)fe.objects.size(); ++oi) {
                            const auto& o = fe.objects[oi];
                            for (int pi = 0; pi < (int)o.pts.size(); ++pi) {
                                ImVec2 sp = n2s(o.pts[pi].x, o.pts[pi].y);
                                float dx = sp.x-mouse_pos.x, dy = sp.y-mouse_pos.y;
                                float d  = std::sqrt(dx*dx+dy*dy);
                                if (d < best_d) { best_d=d; best_oi=oi; best_pi=pi; }
                            }
                        }
                        if (best_oi >= 0) {
                            fe_push_undo();
                            fe.cp_drag_obj_idx = best_oi;
                            fe.cp_drag_pt_idx  = best_pi;
                            fe.cp_drag_orig_x  = fe.objects[best_oi].pts[best_pi].x;
                            fe.cp_drag_orig_y  = fe.objects[best_oi].pts[best_pi].y;
                            if (!ctrl_held) fe.selected_ids.clear();
                            fe.selected_ids.push_back(fe.objects[best_oi].id);
                        } else {
                            fe.cp_drag_obj_idx = -1;
                            fe.cp_drag_pt_idx  = -1;
                        }
                    }
                    if (fe.cp_drag_obj_idx >= 0 &&
                        fe.cp_drag_obj_idx < (int)fe.objects.size() &&
                        fe.cp_drag_pt_idx  >= 0 &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        auto& obj = fe.objects[fe.cp_drag_obj_idx];
                        if (fe.cp_drag_pt_idx < (int)obj.pts.size()) {
                            obj.pts[fe.cp_drag_pt_idx].x = nmx;
                            obj.pts[fe.cp_drag_pt_idx].y = nmy;
                        }
                        // Real-time symmetry update while dragging a control point
                        if (fe.symmetry != FrameEditorState::SymmetryMode::None) {
                            if (fe.symmetry_act_mode)
                                fe_apply_symmetry_to_all_objects(fe);
                            else
                                fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                        }
                    }
                    if (lmb_released && fe.cp_drag_obj_idx >= 0) {
                        fe.cp_drag_obj_idx = -1;
                        fe.cp_drag_pt_idx  = -1;
                        if (fe.symmetry_act_mode)
                            fe_apply_symmetry_to_all_objects(fe);
                        else
                            fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                    }
                }
                // MOVE tool
                else if (fe.active_tool == Tool::Move) {
                    if (lmb_clicked) {
                        float best_d = 14.f; int best_i = -1;
                        for (int oi = 0; oi < (int)fe.objects.size(); ++oi) {
                            bool hid = false;
                            for (auto id : fe.hidden_ids)
                                if (id==fe.objects[oi].id) { hid=true; break; }
                            if (hid) continue;
                            for (const auto& pt : fe.objects[oi].pts) {
                                ImVec2 sp = n2s(pt.x, pt.y);
                                float dx=sp.x-mouse_pos.x, dy=sp.y-mouse_pos.y;
                                float d=std::sqrt(dx*dx+dy*dy);
                                if (d<best_d) { best_d=d; best_i=oi; }
                            }
                        }
                        if (best_i >= 0) {
                            if (!ctrl_held) fe.selected_ids.clear();
                            fe.selected_ids.push_back(fe.objects[best_i].id);
                        }
                        if (!fe.selected_ids.empty()) fe_push_undo();
                        fe.obj_dragging      = true;
                        fe.obj_drag_start_mx = nmx;
                        fe.obj_drag_start_my = nmy;
                        fe.move_constrain_x  = false;
                        fe.move_constrain_y  = false;
                        fe.obj_drag_orig_pts.clear();
                        for (auto id : fe.selected_ids)
                            for (const auto& obj : fe.objects)
                                if (obj.id == id)
                                    fe.obj_drag_orig_pts.push_back(obj.pts);
                    }
                    if (fe.obj_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        float ddx = nmx - fe.obj_drag_start_mx;
                        float ddy = nmy - fe.obj_drag_start_my;
                        if (fe.move_constrain_x) ddy = 0.f;
                        if (fe.move_constrain_y) ddx = 0.f;
                        int di = 0;
                        for (auto id : fe.selected_ids) {
                            for (auto& obj : fe.objects) {
                                if (obj.id==id && di<(int)fe.obj_drag_orig_pts.size()) {
                                    const auto& orig = fe.obj_drag_orig_pts[di];
                                    obj.pts.resize(orig.size());
                                    for (int k=0; k<(int)orig.size(); ++k) {
                                        obj.pts[k].x = orig[k].x + ddx;
                                        obj.pts[k].y = orig[k].y + ddy;
                                    }
                                    ++di;
                                }
                            }
                        }
                        // Real-time symmetry update while dragging objects
                        if (fe.symmetry != FrameEditorState::SymmetryMode::None) {
                            if (fe.symmetry_act_mode)
                                fe_apply_symmetry_to_all_objects(fe);
                            else
                                fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                        }
                    }
                    if (lmb_released) {
                        fe.obj_dragging = false;
                        if (fe.symmetry_act_mode)
                            fe_apply_symmetry_to_all_objects(fe);
                        else
                            fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                    }
                }
                // ROTATE tool
                else if (fe.active_tool == Tool::Rotate) {
                    if (!fe.pivot_custom && !fe.selected_ids.empty()) {
                        float bx0=1e9f,by0=1e9f,bx1=-1e9f,by1=-1e9f;
                        for (const auto& obj : fe.objects) {
                            bool sel=false;
                            for (auto id:fe.selected_ids) if(id==obj.id){sel=true;break;}
                            if (!sel) continue;
                            for (const auto& pt:obj.pts) {
                                if(pt.x<bx0)bx0=pt.x; if(pt.x>bx1)bx1=pt.x;
                                if(pt.y<by0)by0=pt.y; if(pt.y>by1)by1=pt.y;
                            }
                        }
                        if (bx0 < 1e9f) {
                            fe.pivot_x=(bx0+bx1)*0.5f;
                            fe.pivot_y=(by0+by1)*0.5f;
                        }
                    }
                    if (lmb_clicked && !fe.rot_dragging) {
                        if (!fe.selected_ids.empty()) fe_push_undo();
                        fe.rot_dragging    = true;
                        fe.rot_start_angle = std::atan2(nmy-fe.pivot_y, nmx-fe.pivot_x);
                        fe.rot_orig_angle  = 0.f;
                        fe.obj_drag_orig_pts.clear();
                        for (auto id : fe.selected_ids)
                            for (const auto& obj : fe.objects)
                                if (obj.id == id)
                                    fe.obj_drag_orig_pts.push_back(obj.pts);
                    }
                    if (fe.rot_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        float cur_angle = std::atan2(nmy-fe.pivot_y, nmx-fe.pivot_x);
                        float delta = cur_angle - fe.rot_start_angle;
                        if (ImGui::GetIO().KeyShift) {
                            float snap = 3.14159265f / 12.f;
                            delta = std::round(delta / snap) * snap;
                        }
                        float rc = std::cos(delta), rs = std::sin(delta);
                        float px = fe.pivot_x, py = fe.pivot_y;
                        int di = 0;
                        for (auto id : fe.selected_ids) {
                            for (auto& obj : fe.objects) {
                                if (obj.id==id && di<(int)fe.obj_drag_orig_pts.size()) {
                                    const auto& orig = fe.obj_drag_orig_pts[di];
                                    obj.pts.resize(orig.size());
                                    for (int k=0; k<(int)orig.size(); ++k) {
                                        float dx = orig[k].x-px, dy = orig[k].y-py;
                                        obj.pts[k].x = px + dx*rc - dy*rs;
                                        obj.pts[k].y = py + dx*rs + dy*rc;
                                    }
                                    ++di;
                                }
                            }
                        }
                        if (fe.symmetry != FrameEditorState::SymmetryMode::None) {
                            if (fe.symmetry_act_mode)
                                fe_apply_symmetry_to_all_objects(fe);
                            else
                                fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                        }
                        float deg = delta * (180.f / 3.14159265f);
                        char abuf[32];
                        std::snprintf(abuf, sizeof(abuf), "Rotate: %.1f deg", deg);
                        dl->AddText(
                            {canvas_pos.x+6.f,
                             canvas_pos.y+canvas_size.y-ImGui::GetTextLineHeight()*2.f-4.f},
                            IM_COL32(255,200,0,230), abuf);
                    }
                    if (lmb_released) {
                        fe.rot_dragging = false;
                        if (fe.symmetry_act_mode)
                            fe_apply_symmetry_to_all_objects(fe);
                        else
                            fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                    }
                }
                // SCALE tool
                else if (fe.active_tool == Tool::Scale) {
                    if (!fe.pivot_custom && !fe.selected_ids.empty()) {
                        float bx0=1e9f,by0=1e9f,bx1=-1e9f,by1=-1e9f;
                        for (const auto& obj : fe.objects) {
                            bool sel=false;
                            for (auto id:fe.selected_ids) if(id==obj.id){sel=true;break;}
                            if (!sel) continue;
                            for (const auto& pt:obj.pts) {
                                if(pt.x<bx0)bx0=pt.x; if(pt.x>bx1)bx1=pt.x;
                                if(pt.y<by0)by0=pt.y; if(pt.y>by1)by1=pt.y;
                            }
                        }
                        if (bx0 < 1e9f) {
                            fe.pivot_x=(bx0+bx1)*0.5f;
                            fe.pivot_y=(by0+by1)*0.5f;
                        }
                    }
                    if (lmb_clicked && !fe.scale_dragging) {
                        if (!fe.selected_ids.empty()) fe_push_undo();
                        float dx=nmx-fe.pivot_x, dy=nmy-fe.pivot_y;
                        fe.scale_start_dist=std::max(std::sqrt(dx*dx+dy*dy),0.001f);
                        fe.scale_dragging=true;
                        fe.scale_orig_pts.clear();
                        for (auto id : fe.selected_ids)
                            for (const auto& obj : fe.objects)
                                if (obj.id==id)
                                    fe.scale_orig_pts.push_back(obj.pts);
                    }
                    if (fe.scale_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        float dx=nmx-fe.pivot_x, dy=nmy-fe.pivot_y;
                        float cur_dist=std::max(std::sqrt(dx*dx+dy*dy),0.001f);
                        float factor=cur_dist/fe.scale_start_dist;
                        float px=fe.pivot_x, py=fe.pivot_y;
                        int di=0;
                        for (auto id : fe.selected_ids) {
                            for (auto& obj : fe.objects) {
                                if (obj.id==id && di<(int)fe.scale_orig_pts.size()) {
                                    const auto& orig=fe.scale_orig_pts[di];
                                    obj.pts.resize(orig.size());
                                    if (ImGui::GetIO().KeyShift) {
                                        for (int k=0; k<(int)orig.size(); ++k) {
                                            obj.pts[k].x=px+(orig[k].x-px)*factor;
                                            obj.pts[k].y=py+(orig[k].y-py)*factor;
                                        }
                                    } else {
                                        bool xdom=std::abs(dx)>std::abs(dy);
                                        for (int k=0; k<(int)orig.size(); ++k) {
                                            if (xdom) {
                                                obj.pts[k].x=px+(orig[k].x-px)*factor;
                                                obj.pts[k].y=orig[k].y;
                                            } else {
                                                obj.pts[k].x=orig[k].x;
                                                obj.pts[k].y=py+(orig[k].y-py)*factor;
                                            }
                                        }
                                    }
                                    ++di;
                                }
                            }
                        }
                        if (fe.symmetry != FrameEditorState::SymmetryMode::None) {
                            if (fe.symmetry_act_mode)
                                fe_apply_symmetry_to_all_objects(fe);
                            else
                                fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                        }
                    }
                    if (lmb_released) {
                        fe.scale_dragging = false;
                        if (fe.symmetry_act_mode)
                            fe_apply_symmetry_to_all_objects(fe);
                        else
                            fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                    }
                }
                // PIVOT tool
                else if (fe.active_tool == Tool::Pivot) {
                    if (lmb_clicked) {
                        fe.obj_dragging=true;
                        fe.pivot_custom=true;
                    }
                    if (fe.obj_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        fe.pivot_x=nmx;
                        fe.pivot_y=nmy;
                    }
                    if (lmb_released) fe.obj_dragging=false;
                }
                // GROUP SCALE tool
                else if (fe.active_tool == Tool::GroupScale && !fe.selected_ids.empty()) {
                    // Build sorted list of selected objects (same order as gs_orig_pts)
                    std::vector<LaserObject*> gs_sel_objs;
                    for (auto id : fe.selected_ids)
                        for (auto& obj : fe.objects)
                            if (obj.id == id) { gs_sel_objs.push_back(&obj); break; }

                    if (!gs_sel_objs.empty()) {
                        // Compute live bbox with padding
                        float live_bx0=1e9f,live_by0=1e9f,live_bx1=-1e9f,live_by1=-1e9f;
                        for (const auto* obj : gs_sel_objs)
                            for (const auto& pt : obj->pts) {
                                if(pt.x<live_bx0)live_bx0=pt.x; if(pt.x>live_bx1)live_bx1=pt.x;
                                if(pt.y<live_by0)live_by0=pt.y; if(pt.y>live_by1)live_by1=pt.y;
                            }
                        float pad_n = 0.03f;
                        live_bx0-=pad_n; live_by0-=pad_n; live_bx1+=pad_n; live_by1+=pad_n;
                        float live_cx = (live_bx0+live_bx1)*0.5f;
                        float live_cy = (live_by0+live_by1)*0.5f;
                        // 8 handle positions: 0=TL,1=TR,2=BR,3=BL, 4=T,5=R,6=B,7=L
                        float hx[8] = {live_bx0,live_bx1,live_bx1,live_bx0, live_cx,live_bx1,live_cx,live_bx0};
                        float hy[8] = {live_by1,live_by1,live_by0,live_by0, live_by1,live_cy,live_by0,live_cy};

                        if (lmb_clicked && !fe.gs_dragging) {
                            // Hit-test handles first (8px radius in screen space)
                            fe.gs_handle = -1;
                            float best_hd = 8.f;
                            for (int h=0; h<8; ++h) {
                                ImVec2 sp = n2s(hx[h],hy[h]);
                                float dx2=sp.x-mouse_pos.x, dy2=sp.y-mouse_pos.y;
                                float d=std::sqrt(dx2*dx2+dy2*dy2);
                                if (d < best_hd) { best_hd=d; fe.gs_handle=h; }
                            }
                            // If no handle hit, check body (inside bbox)
                            if (fe.gs_handle==-1) {
                                if (nmx>=live_bx0 && nmx<=live_bx1 && nmy>=live_by0 && nmy<=live_by1)
                                    fe.gs_handle=-1; // body drag: -1
                                else
                                    fe.gs_handle=-2; // outside: do nothing
                            }
                            if (fe.gs_handle != -2) {
                                fe.gs_dragging    = true;
                                fe.gs_drag_start_mx = nmx;
                                fe.gs_drag_start_my = nmy;
                                // Save original bbox (without padding for accurate math)
                                float raw_bx0=1e9f,raw_by0=1e9f,raw_bx1=-1e9f,raw_by1=-1e9f;
                                for (const auto* obj : gs_sel_objs)
                                    for (const auto& pt : obj->pts) {
                                        if(pt.x<raw_bx0)raw_bx0=pt.x; if(pt.x>raw_bx1)raw_bx1=pt.x;
                                        if(pt.y<raw_by0)raw_by0=pt.y; if(pt.y>raw_by1)raw_by1=pt.y;
                                    }
                                fe.gs_box_x0=raw_bx0; fe.gs_box_y0=raw_by0;
                                fe.gs_box_x1=raw_bx1; fe.gs_box_y1=raw_by1;
                                fe.gs_orig_pts.clear();
                                for (const auto* obj : gs_sel_objs)
                                    fe.gs_orig_pts.push_back(obj->pts);
                                fe_push_undo();
                            }
                        }

                        if (fe.gs_dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            float ddx = nmx - fe.gs_drag_start_mx;
                            float ddy = nmy - fe.gs_drag_start_my;
                            float obx0=fe.gs_box_x0, obx1=fe.gs_box_x1;
                            float oby0=fe.gs_box_y0, oby1=fe.gs_box_y1;
                            float ow = obx1-obx0, oh = oby1-oby0;

                            // Compute new bbox corners based on handle
                            float nbx0=obx0, nbx1=obx1, nby0=oby0, nby1=oby1;
                            if (fe.gs_handle == -1) {
                                // Body drag: translate all
                                nbx0=obx0+ddx; nbx1=obx1+ddx;
                                nby0=oby0+ddy; nby1=oby1+ddy;
                            } else if (fe.gs_handle == 0) { // TL corner
                                nbx0=obx0+ddx; nby1=oby1+ddy;
                            } else if (fe.gs_handle == 1) { // TR corner
                                nbx1=obx1+ddx; nby1=oby1+ddy;
                            } else if (fe.gs_handle == 2) { // BR corner
                                nbx1=obx1+ddx; nby0=oby0+ddy;
                            } else if (fe.gs_handle == 3) { // BL corner
                                nbx0=obx0+ddx; nby0=oby0+ddy;
                            } else if (fe.gs_handle == 4) { // T edge
                                nby1=oby1+ddy;
                            } else if (fe.gs_handle == 5) { // R edge
                                nbx1=obx1+ddx;
                            } else if (fe.gs_handle == 6) { // B edge
                                nby0=oby0+ddy;
                            } else if (fe.gs_handle == 7) { // L edge
                                nbx0=obx0+ddx;
                            }

                            // Aspect-ratio lock for corner handles when Shift held
                            bool corner_handle = (fe.gs_handle >= 0 && fe.gs_handle <= 3);
                            if (corner_handle && ImGui::GetIO().KeyShift && ow>1e-5f && oh>1e-5f) {
                                float nw = nbx1-nbx0, nh = nby1-nby0;
                                float aspect = ow/oh;
                                // Use whichever axis changed more to drive the other
                                if (std::abs(nw/ow - 1.f) >= std::abs(nh/oh - 1.f)) {
                                    float target_h = nw/aspect;
                                    if (fe.gs_handle==0||fe.gs_handle==3) nby1=nby0+target_h; // top moves
                                    else                                   nby0=nby1-target_h; // bottom moves
                                } else {
                                    float target_w = nh*aspect;
                                    if (fe.gs_handle==0||fe.gs_handle==1) nbx1=nbx0+target_w; // right moves
                                    else                                   nbx0=nbx1-target_w; // left moves
                                }
                            }

                            float nw = nbx1-nbx0, nh = nby1-nby0;
                            int di=0;
                            for (auto* obj : gs_sel_objs) {
                                if (di >= (int)fe.gs_orig_pts.size()) break;
                                const auto& orig = fe.gs_orig_pts[di];
                                obj->pts.resize(orig.size());
                                for (int k=0; k<(int)orig.size(); ++k) {
                                    if (fe.gs_handle == -1) {
                                        // Pure translation
                                        obj->pts[k].x = orig[k].x + ddx;
                                        obj->pts[k].y = orig[k].y + ddy;
                                    } else {
                                        // Map orig point from old bbox to new bbox
                                        float tx = (ow > 1e-5f) ? (orig[k].x-obx0)/ow : 0.5f;
                                        float ty = (oh > 1e-5f) ? (orig[k].y-oby0)/oh : 0.5f;
                                        obj->pts[k].x = nbx0 + tx*(nw > 1e-5f ? nw : 0.f);
                                        obj->pts[k].y = nby0 + ty*(nh > 1e-5f ? nh : 0.f);
                                    }
                                }
                                ++di;
                            }
                            if (fe.symmetry != FrameEditorState::SymmetryMode::None) {
                                if (fe.symmetry_act_mode)
                                    fe_apply_symmetry_to_all_objects(fe);
                                else
                                    fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                            }
                        }

                        if (lmb_released && fe.gs_dragging) {
                            fe.gs_dragging = false;
                            fe.gs_handle   = -2;
                            if (fe.symmetry_act_mode)
                                fe_apply_symmetry_to_all_objects(fe);
                            else
                                fe_apply_symmetry_to_objects(fe, canvas_w, content_h);
                        }
                    }
                }
                // LASSO tool
                else if (fe.active_tool == Tool::Lasso) {
                    if (lmb_clicked && !fe.lasso_active) {
                        fe.lasso_active=true;
                        fe.lasso_pts.clear();
                        fe.lasso_pts.push_back({nmx,nmy});
                    }
                    if (fe.lasso_active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        if (!fe.lasso_pts.empty()) {
                            float lx=fe.lasso_pts.back().x-nmx;
                            float ly=fe.lasso_pts.back().y-nmy;
                            if (lx*lx+ly*ly > 0.0001f)
                                fe.lasso_pts.push_back({nmx,nmy});
                        }
                    }
                    if (lmb_released && fe.lasso_active) {
                        fe.lasso_active=false;
                        if ((int)fe.lasso_pts.size()>=3) {
                            if (!ctrl_held) fe.selected_ids.clear();
                            for (const auto& obj : fe.objects) {
                                if (obj.pts.empty()) continue;
                                float cx2=0.f, cy2=0.f;
                                for (const auto& pt:obj.pts){cx2+=pt.x;cy2+=pt.y;}
                                cx2/=(float)obj.pts.size();
                                cy2/=(float)obj.pts.size();
                                int crossing=0;
                                int nv=(int)fe.lasso_pts.size();
                                for (int k=0;k<nv;++k) {
                                    const ImVec2& pa=fe.lasso_pts[k];
                                    const ImVec2& pb=fe.lasso_pts[(k+1)%nv];
                                    if ((pa.y<=cy2&&pb.y>cy2)||(pb.y<=cy2&&pa.y>cy2)) {
                                        float t2=(cy2-pa.y)/(pb.y-pa.y);
                                        if (cx2 < pa.x+t2*(pb.x-pa.x)) ++crossing;
                                    }
                                }
                                if (crossing%2==1)
                                    fe.selected_ids.push_back(obj.id);
                            }
                        }
                        fe.lasso_pts.clear();
                    }
                }
                // Helper: add object from only the primary symmetry slot, then regenerate copies
                // This prevents the Move tool from seeing drawn objects as independent sources.
                auto add_with_sym = [&](LaserObjectType type, const std::vector<ImVec2>& pts) {
                    auto sym_sets = apply_symmetry(pts, fe.symmetry,
                        fe.symmetry_center_x, fe.symmetry_center_y);
                    add_obj(type, sym_sets[0]);
                    if (fe.symmetry != FrameEditorState::SymmetryMode::None)
                        fe_apply_symmetry_to_all_objects(fe);
                };

                // PEN (Bezier) tool
                if (fe.active_tool == Tool::Pen) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        fe.drawing=true;
                        if ((int)fe.wip_points.size()%4==0) {
                            int s0=(int)fe.wip_points.size()-4;
                            std::vector<ImVec2> seg4(fe.wip_points.begin()+s0,
                                                     fe.wip_points.end());
                            add_with_sym(LaserObjectType::Bezier, seg4);
                            fe.wip_points.clear(); fe.drawing=false;
                        }
                    }
                    if (rmb_clicked){fe.wip_points.clear();fe.drawing=false;}
                }
                // LINE (Polyline) tool
                else if (fe.active_tool == Tool::Line) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        fe.drawing=true;
                    }
                    bool finish=rmb_clicked||
                        (lmb_clicked&&ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    if (finish&&!fe.wip_points.empty()) {
                        if ((int)fe.wip_points.size()>=2) {
                            std::vector<ImVec2> v2;
                            for (const auto& w:fe.wip_points) v2.push_back({w.x,w.y});
                            add_with_sym(LaserObjectType::Line, v2);
                        }
                        fe.wip_points.clear(); fe.drawing=false;
                    }
                }
                // CIRCLE tool
                else if (fe.active_tool == Tool::Circle) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        if ((int)fe.wip_points.size()==2) {
                            std::vector<ImVec2> pts2={
                                {fe.wip_points[0].x,fe.wip_points[0].y},
                                {fe.wip_points[1].x,fe.wip_points[1].y}};
                            add_with_sym(LaserObjectType::Circle, pts2);
                            fe.wip_points.clear(); fe.drawing=false;
                        } else { fe.drawing=true; }
                    }
                }
                // RECT tool
                else if (fe.active_tool == Tool::Rect) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        if ((int)fe.wip_points.size()==2) {
                            float rx0=fe.wip_points[0].x,ry0=fe.wip_points[0].y;
                            float rx1=fe.wip_points[1].x,ry1=fe.wip_points[1].y;
                            std::vector<ImVec2> rect={
                                {rx0,ry0},{rx1,ry0},{rx1,ry1},{rx0,ry1},{rx0,ry0}};
                            add_with_sym(LaserObjectType::Line, rect);
                            fe.wip_points.clear(); fe.drawing=false;
                        } else { fe.drawing=true; }
                    }
                }
                // ARC tool — 3-click: center, start-point-on-arc, end-angle-click
                // Stores exactly 3 control points matching the renderer's expectations:
                //   pts[0] = center, pts[1] = start point on circumference,
                //   pts[2] = end point on circumference at same radius
                else if (fe.active_tool == Tool::Arc) {
                    if (lmb_clicked) {
                        if (fe.arc_click_count == 0) {
                            // Click 1: set center
                            fe.wip_points.clear();
                            fe.wip_points.push_back({nmx,nmy});
                            fe.arc_click_count = 1;
                            fe.drawing = true;
                        } else if (fe.arc_click_count == 1) {
                            // Click 2: set start point on circumference (defines radius + start angle)
                            fe.wip_points.push_back({nmx,nmy});
                            fe.arc_click_count = 2;
                        } else {
                            // Click 3: set end angle — snap third point to same radius as start
                            ImVec2 cen = fe.wip_points[0];
                            ImVec2 st  = fe.wip_points[1];
                            float rn = std::sqrt((st.x-cen.x)*(st.x-cen.x)+
                                                 (st.y-cen.y)*(st.y-cen.y));
                            // Direction from center to third click
                            float dx = nmx - cen.x;
                            float dy = nmy - cen.y;
                            float d  = std::sqrt(dx*dx + dy*dy);
                            // End point on circumference at same radius
                            ImVec2 en;
                            if (d > 1e-6f) {
                                en = {cen.x + rn * dx/d, cen.y + rn * dy/d};
                            } else {
                                en = st; // degenerate: same as start
                            }
                            std::vector<ImVec2> arc_pts = { cen, st, en };
                            add_with_sym(LaserObjectType::Arc, arc_pts);
                            fe.wip_points.clear(); fe.drawing=false; fe.arc_click_count=0;
                        }
                    }
                    // Arc WIP preview: after click 2 draw a live arc from start to mouse
                    if (fe.arc_click_count == 2 && canvas_hovered && fe.wip_points.size() >= 2) {
                        ImVec2 cen = n2s(fe.wip_points[0].x, fe.wip_points[0].y);
                        ImVec2 st  = n2s(fe.wip_points[1].x, fe.wip_points[1].y);
                        float rn_s = std::sqrt((st.x-cen.x)*(st.x-cen.x)+
                                               (st.y-cen.y)*(st.y-cen.y));
                        float a0 = std::atan2(st.y - cen.y, st.x - cen.x);
                        float a1 = std::atan2(mouse_pos.y - cen.y, mouse_pos.x - cen.x);
                        // Draw circle ghost
                        dl->AddCircle(cen, rn_s, IM_COL32(0,230,255,50), 64, 1.f);
                        // Draw arc preview
                        dl->PathArcTo(cen, rn_s, a0, a1, 32);
                        dl->PathStroke(IM_COL32(0,230,255,180), ImDrawFlags_None, 1.5f);
                    }
                    if (rmb_clicked){fe.wip_points.clear();fe.drawing=false;fe.arc_click_count=0;}
                }
                // POLYGON tool
                else if (fe.active_tool == Tool::Polygon) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        if ((int)fe.wip_points.size()==2) {
                            ImVec2 cen=fe.wip_points[0];
                            ImVec2 ep =fe.wip_points[1];
                            float rad=std::sqrt((ep.x-cen.x)*(ep.x-cen.x)+
                                                (ep.y-cen.y)*(ep.y-cen.y));
                            int sides=std::max(fe.shape_sides,3);
                            float sa=std::atan2(ep.y-cen.y,ep.x-cen.x);
                            std::vector<ImVec2> poly;
                            for (int k=0;k<=sides;++k) {
                                float a=sa+6.28318f*(float)k/(float)sides;
                                poly.push_back({cen.x+rad*std::cos(a),
                                                cen.y+rad*std::sin(a)});
                            }
                            add_with_sym(LaserObjectType::Line, poly);
                            fe.wip_points.clear(); fe.drawing=false;
                        } else { fe.drawing=true; }
                    }
                }
                // STAR tool
                else if (fe.active_tool == Tool::Star) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        if ((int)fe.wip_points.size()==2) {
                            ImVec2 cen=fe.wip_points[0];
                            ImVec2 ep =fe.wip_points[1];
                            float ro=std::sqrt((ep.x-cen.x)*(ep.x-cen.x)+
                                               (ep.y-cen.y)*(ep.y-cen.y));
                            float ri=ro*fe.shape_inner;
                            int npts_s=std::max(fe.shape_sides,2)*2;
                            float sa=std::atan2(ep.y-cen.y,ep.x-cen.x);
                            std::vector<ImVec2> star;
                            for (int k=0;k<=npts_s;++k) {
                                float a=sa+3.14159265f*(float)k/(float)(npts_s/2);
                                float r=(k%2==0)?ro:ri;
                                star.push_back({cen.x+r*std::cos(a),
                                                cen.y+r*std::sin(a)});
                            }
                            add_with_sym(LaserObjectType::Line, star);
                            fe.wip_points.clear(); fe.drawing=false;
                        } else { fe.drawing=true; }
                    }
                }
                // DOT tool
                else if (fe.active_tool == Tool::Dot) {
                    if (lmb_clicked) {
                        std::vector<ImVec2> v2={{nmx,nmy}};
                        add_with_sym(LaserObjectType::Dot, v2);
                    }
                }
                // TEXT tool
                else if (fe.active_tool == Tool::Text) {
                    if (lmb_clicked && !fe.text_popup_open) {
                        fe.text_place_pos={nmx,nmy};
                        fe.text_popup_open=true;
                        std::memset(fe.text_input_buf,0,sizeof(fe.text_input_buf));
                        ImGui::OpenPopup("##fe_text_inp");
                    }
                }
                // POLYLINE tool — click to add pts, double-click or RMB to finish
                else if (fe.active_tool == Tool::Polyline) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        fe.drawing=true;
                    }
                    bool finish_pl = rmb_clicked ||
                        (lmb_clicked && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left));
                    if (finish_pl && !fe.wip_points.empty()) {
                        if ((int)fe.wip_points.size() >= 2) {
                            std::vector<ImVec2> v2;
                            for (const auto& w : fe.wip_points) v2.push_back({w.x,w.y});
                            add_with_sym(LaserObjectType::Line, v2);
                        }
                        fe.wip_points.clear(); fe.drawing=false;
                    }
                }
                // BEZIER spline tool — click pairs: each 4 pts emit one cubic segment
                else if (fe.active_tool == Tool::Bezier) {
                    if (lmb_clicked) {
                        fe.wip_points.push_back({nmx,nmy});
                        fe.drawing=true;
                        if ((int)fe.wip_points.size() % 4 == 0) {
                            int s0 = (int)fe.wip_points.size() - 4;
                            std::vector<ImVec2> seg4(fe.wip_points.begin()+s0,
                                                     fe.wip_points.end());
                            add_with_sym(LaserObjectType::Bezier, seg4);
                            fe.wip_points.clear(); fe.drawing=false;
                        }
                    }
                    if (rmb_clicked) { fe.wip_points.clear(); fe.drawing=false; }
                }
            } // end else (not ctrl+click symmetry set)
        } // end canvas_input_active

        // ── Filled Shape pending-place: handle canvas click ──────────────────
        if (fe.active_tool == Tool::FilledShape && fe.filled_shape.pending_place) {
            // Draw a placement cursor hint on the canvas
            dl->AddCircle(mouse_pos, 8.f, IM_COL32(0, 220, 180, 200), 16, 1.5f);
            if (lmb_clicked && canvas_hovered) {
                // Convert screen coords to normalized canvas space
                float nx2 = s2nx(mouse_pos.x);
                float ny2 = s2ny(mouse_pos.y);
                fe.filled_shape.pending_place = false;
                fe_push_undo();
                place_filled_shape_at(fe, nx2, ny2);
            }
        }

        // Text popup
        if (fe.text_popup_open) {
            if (ImGui::BeginPopup("##fe_text_inp")) {
                ImGui::TextDisabled("Enter text:");
                ImGui::SetNextItemWidth(200.f);
                bool commit = ImGui::InputText("##fe_tf", fe.text_input_buf,
                    sizeof(fe.text_input_buf), ImGuiInputTextFlags_EnterReturnsTrue);
                if (commit && std::strlen(fe.text_input_buf) > 0) {
                    fe_push_undo();
                    LaserObject obj;
                    obj.id   = fe.next_object_id++;
                    obj.type = LaserObjectType::Text;
                    obj.r = fe.edit_r; obj.g = fe.edit_g; obj.b = fe.edit_b;
                    obj.pts.push_back({fe.text_place_pos.x, fe.text_place_pos.y});
                    obj.text = fe.text_input_buf;
                    fe.objects.push_back(obj);
                    fe.selected_ids = { obj.id };
                    if (fe.symmetry != FrameEditorState::SymmetryMode::None)
                        fe_apply_symmetry_to_all_objects(fe);
                    fe.text_popup_open = false;
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    fe.text_popup_open = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            } else {
                fe.text_popup_open = false;
            }
        }

        // Status text at bottom of canvas
        {
            static const char* s_tnames[] = {
                "Select","NodeEdit","Move","Rotate","Scale","Pivot","GroupScale",
                "PremadeShapes",
                "FilledShape",
                "Pen","Line","Circle","Rect","Arc","Polygon","Star","Dot","Text","Lasso",
                "Polyline","Bezier",
                "ImportImage"
            };
            int ti = (int)fe.active_tool;
            const char* tname = (ti >= 0 && ti < 22) ? s_tnames[ti] : "?";
            const char* snames[] = {"","[X]","[Y]","[XY]","[r2]","[r3]","[r4]","[r6]","[r8]","[r12]"};
            int si = (int)fe.symmetry;
            char sbuf[128];
            std::snprintf(sbuf, sizeof(sbuf), "Tool:%s %s  Objs:%d  Sel:%d",
                tname,
                (si>=0&&si<10)?snames[si]:"",
                (int)fe.objects.size(), (int)fe.selected_ids.size());
            dl->AddText(
                {canvas_pos.x+6.f, canvas_pos.y+canvas_size.y-ImGui::GetTextLineHeight()-4.f},
                IM_COL32(110,110,130,200), sbuf);
        }
    } // end Keyframe layer

fe_timeline_draw:
    // =========================================================================
    //  KEYFRAME TIMELINE (bottom bar)
    // =========================================================================
    if (fe.show_keyframe_editor) {
        ImGui::Dummy(ImVec2(0.f, 2.f));

        float tl_w = ImGui::GetContentRegionAvail().x;
        float tl_h = 52.f;

        ImVec2 tl_pos = ImGui::GetCursorScreenPos();
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        tdl->AddRectFilled(tl_pos, {tl_pos.x + tl_w, tl_pos.y + tl_h},
                           IM_COL32(10,10,16,255));
        tdl->AddRect(tl_pos, {tl_pos.x + tl_w, tl_pos.y + tl_h},
                     IM_COL32(55,55,75,200));

        ImGui::InvisibleButton("##fe_anim_tl", ImVec2(tl_w, tl_h));
        bool tl_hov = ImGui::IsItemHovered();
        bool tl_clk = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        ImVec2 tl_mp = ImGui::GetIO().MousePos;

        float dur = std::max(fe.anim_duration, 0.1f);
        auto t2x = [&](float t) -> float { return tl_pos.x + (t / dur) * tl_w; };
        auto x2t = [&](float x) -> float {
            return std::clamp((x - tl_pos.x) / tl_w * dur, 0.f, dur);
        };

        // Tick marks
        for (float ts = 0.f; ts <= dur + 0.01f; ts += 0.5f) {
            float tx = t2x(ts);
            bool whole = (std::fmod(ts + 0.01f, 1.f) < 0.02f);
            tdl->AddLine({tx, tl_pos.y}, {tx, tl_pos.y + (whole ? 10.f : 5.f)},
                         IM_COL32(110,110,130,180), 1.f);
            if (whole) {
                char lbl[8]; std::snprintf(lbl, sizeof(lbl), "%.0fs", ts);
                tdl->AddText({tx+2.f, tl_pos.y+1.f}, IM_COL32(130,130,150,200), lbl);
            }
        }

        // Playhead
        {
            float ph_x = t2x(fe.anim_current_time);
            tdl->AddLine({ph_x, tl_pos.y}, {ph_x, tl_pos.y + tl_h},
                         IM_COL32(0,230,255,220), 2.f);
            static bool s_ph_drag = false;
            if (tl_hov && tl_clk) s_ph_drag = true;
            if (s_ph_drag && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                fe.anim_current_time = x2t(tl_mp.x);
            } else {
                s_ph_drag = false;
            }
        }

        // AnimFrame diamonds
        static int s_af_drag   = -1;
        static int s_af_rclick = -1;
        float dr = 6.f;
        float ky = tl_pos.y + tl_h * 0.65f;

        for (int fi = 0; fi < (int)fe.anim_frames.size(); ++fi) {
            float kx = t2x(fe.anim_frames[fi].time_s);
            ImU32 kc = (fe.anim_selected_frame == fi)
                ? ImGui::ColorConvertFloat4ToU32(theme::accent())
                : IM_COL32(200,180,50,255);
            tdl->AddQuadFilled({kx,ky-dr},{kx+dr,ky},{kx,ky+dr},{kx-dr,ky}, kc);
            float ddx = tl_mp.x - kx, ddy = tl_mp.y - ky;
            bool hit = (std::abs(ddx) + std::abs(ddy)) < dr + 3.f;
            if (hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                fe.anim_selected_frame  = fi;
                s_af_drag               = fi;
                fe_push_undo();
                fe.objects              = fe.anim_frames[fi].objects;
                fe.anim_current_time    = fe.anim_frames[fi].time_s;
            }
            if (hit && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                fe.anim_selected_frame = fi;
                s_af_rclick = fi;
                ImGui::OpenPopup("##af_ctx");
            }
        }

        if (s_af_drag >= 0 && s_af_drag < (int)fe.anim_frames.size()) {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                fe.anim_frames[s_af_drag].time_s = x2t(tl_mp.x);
            else s_af_drag = -1;
        }

        if (ImGui::BeginPopup("##af_ctx")) {
            if (s_af_rclick >= 0 && s_af_rclick < (int)fe.anim_frames.size()) {
                ImGui::TextDisabled("Frame @ %.2fs", fe.anim_frames[s_af_rclick].time_s);
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    fe.anim_frames.erase(fe.anim_frames.begin() + s_af_rclick);
                    if (fe.anim_selected_frame == s_af_rclick) fe.anim_selected_frame = -1;
                    s_af_rclick = -1;
                }
            }
            ImGui::EndPopup();
        }

        // Timeline toolbar
        ImGui::Dummy(ImVec2(0.f, 2.f));

        if (fe.anim_playing) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f,0.50f,0.05f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f,0.65f,0.10f,1.f));
            if (ImGui::Button("Stop##tl")) fe.anim_playing = false;
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f,0.45f,0.10f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f,0.60f,0.15f,1.f));
            if (ImGui::Button("Play##tl")) {
                fe.anim_playing = true;
                if (fe.anim_current_time >= fe.anim_duration) fe.anim_current_time = 0.f;
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::SameLine(0, 4);

        if (ImGui::Button("+KF##af")) {
            FrameEditorState::AnimFrame af;
            af.time_s  = fe.anim_current_time;
            af.objects = fe.objects;
            auto it = fe.anim_frames.begin();
            while (it != fe.anim_frames.end() && it->time_s < af.time_s) ++it;
            int ni = (int)(it - fe.anim_frames.begin());
            fe.anim_frames.insert(it, std::move(af));
            fe.anim_selected_frame = ni;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add keyframe at current time");

        ImGui::SameLine(0, 4);

        bool has_af = (fe.anim_selected_frame >= 0 &&
                       fe.anim_selected_frame < (int)fe.anim_frames.size());
        if (!has_af) ImGui::BeginDisabled();
        if (ImGui::Button("DelKF##af")) {
            fe.anim_frames.erase(fe.anim_frames.begin() + fe.anim_selected_frame);
            fe.anim_selected_frame = -1;
        }
        if (!has_af) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete selected keyframe");


        ImGui::SameLine(0, 8);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine(0, 8);

        ImGui::PushItemWidth(80.f);
        DragTimingFloat("Dur##af", &fe.anim_duration, 0.1f, 0.5f, 600.f, state.bpm);
        ImGui::PopItemWidth();

        ImGui::SameLine(0, 8);

        char tbuf[32];
        std::snprintf(tbuf, sizeof(tbuf), "%.2f / %.2fs", fe.anim_current_time, fe.anim_duration);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::accent()));
        ImGui::TextUnformatted(tbuf);
        ImGui::PopStyleColor();

        // Also show the keyframe toggle button so user can close timeline
        ImGui::SameLine(0, 8);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine(0, 8);
        if (ImGui::Button("Hide TL##fe_tl")) fe.show_keyframe_editor = false;
    } else {
        // Show "Timeline" button when hidden
        if (ImGui::Button("Timeline##fe_tl_show")) fe.show_keyframe_editor = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show keyframe timeline");
    }

    // ── Premade Shapes floating panel (only when tool is active) ────────────
    if (fe.active_layer == AL::Keyframe &&
        fe.active_tool  == Tool::PremadeShapes)
    {
        // Returns true when [Generate] is clicked.
        // Push undo here (fe_push_undo is a local lambda) then run generation.
        if (draw_premade_shapes_panel(fe)) {
            fe_push_undo();
            generate_premade_shapes(fe.premade_shapes, fe.objects, fe.next_object_id);
        }

        // If the user closed the panel via the X button, switch back to Select
        if (!fe.premade_shapes.open) {
            fe.active_tool = Tool::Select;
            fe.premade_shapes.preview_pts.clear();
            fe.premade_shapes.open = true; // Reset for next time the tool is activated
        }
    }

    // ── Filled Shapes floating panel (only when tool is active) ─────────────
    if (fe.active_layer == AL::Keyframe &&
        fe.active_tool  == Tool::FilledShape)
    {
        // If the panel was previously closed via its X button, switch away from
        // the FilledShape tool so the panel doesn't immediately re-open.
        if (!fe.filled_shape.open) {
            fe.filled_shape.open = true; // reset for next activation
            fe.active_tool = Tool::Select;
        } else {
            // Returns -1 if nothing placed, 0 if Place at Center, 1 if pending_place set
            int place_result = draw_filled_shape_panel(fe);
            if (place_result == 0) {
                fe_push_undo();
                place_filled_shape_at(fe, 0.f, 0.f);
            }
            // place_result == 1 means pending_place flag was set; actual placement
            // happens inside the Keyframe canvas block (see pending_place handler above)
        }
    }

    // ── Image Import floating panel (only when tool is active) ─────────────
    if (fe.active_layer == AL::Keyframe &&
        fe.active_tool  == Tool::ImportImage)
    {
        // If the panel was previously closed via its X button, switch away from
        // the ImportImage tool so the panel doesn't immediately re-open.
        if (!fe.image_import.open) {
            fe.image_import.open = true; // reset for next activation
            fe.active_tool = Tool::Select;
        } else if (draw_image_import_panel(fe.image_import)) {
            // User clicked [Place in Frame] — convert PointBuffer to a LaserObject (Line polyline)
            if (!fe.image_import.preview_pts.empty()) {
                fe_push_undo();
                LaserObject obj;
                obj.id        = fe.next_object_id++;
                obj.type      = LaserObjectType::Line;
                obj.r         = fe.image_import.cfg.col_r;
                obj.g         = fe.image_import.cfg.col_g;
                obj.b         = fe.image_import.cfg.col_b;
                obj.thickness = 1.f;
                obj.size      = 0.005f;
                // Store all lit (non-blanked) points as control points
                for (const auto& lp : fe.image_import.preview_pts) {
                    if (!lp.blanked)
                        obj.pts.push_back({ lp.nx(), lp.ny() });
                }
                fe.objects.push_back(obj);
                fe.selected_ids = { obj.id };
            }
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

// ─────────────────────────────────────────────────────────────────────────────
//  PBCONF popup helper
// ─────────────────────────────────────────────────────────────────────────────
static void draw_pbconf_popup(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    if (ctx.pbconf_open_id < 0) return;
    int slot_idx = ctx.pbconf_open_id - 1;
    if (slot_idx < 0 || slot_idx >= UIState::kMaxPlaybacks) { ctx.pbconf_open_id = -1; return; }

    UIState::PlaybackConf& conf = state.pb_conf[slot_idx];
    char popup_title[32];
    std::snprintf(popup_title, sizeof(popup_title), "PB %d Config##pbcfg", ctx.pbconf_open_id);

    ImGui::SetNextWindowSize(ImVec2(380.f, 0.f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(340.f, 260.f), ImVec2(600.f, FLT_MAX));

    static UIState::PlaybackConf s_edit;
    static int s_edit_id = -1;
    if (s_edit_id != ctx.pbconf_open_id) {
        s_edit    = conf;
        s_edit_id = ctx.pbconf_open_id;
    }
    // OpenPopup must be called every frame while the popup should be visible
    // (calling it when already open is a no-op in ImGui)
    ImGui::OpenPopup(popup_title);

    bool open = true;
    if (ImGui::BeginPopupModal(popup_title, &open,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(ImVec4(0.f, 0.9f, 1.f, 1.f), "PB %d Configuration", ctx.pbconf_open_id);
        ImGui::Spacing();

        const char* dmx_modes[] = { "Off", "1-Channel", "2-Channel" };
        int dmx_mode_idx = static_cast<int>(s_edit.dmx_mode);
        ImGui::SetNextItemWidth(140.f);
        if (ImGui::Combo("Mode##dmxmode", &dmx_mode_idx, dmx_modes, 3))
            s_edit.dmx_mode = static_cast<UIState::PlaybackConf::DmxMode>(dmx_mode_idx);

        if (s_edit.dmx_mode != UIState::PlaybackConf::DmxMode::Off) {
            ImGui::SetNextItemWidth(80.f);
            ImGui::DragInt("Universe##dmx_uni", &s_edit.dmx_universe, 1.f, 0, 15);
            ImGui::SetNextItemWidth(80.f);
            ImGui::DragInt("Channel##dmx_ch",  &s_edit.dmx_channel,  1.f, 1, 512);
            s_edit.dmx_channel = std::clamp(s_edit.dmx_channel, 1, 512);
            ImGui::SetNextItemWidth(160.f);
            ImGui::SliderInt("Threshold##dmx_thr", &s_edit.dmx_threshold, 0, 255,
                             "value > %d = active", ImGuiSliderFlags_AlwaysClamp);
            if (s_edit.dmx_mode == UIState::PlaybackConf::DmxMode::TwoChannel) {
                ImGui::TextDisabled("Ch1 = intensity (0-255)");
                ImGui::TextDisabled("Ch2 = GO (rising edge triggers advance)");
            }
        }

        ImGui::SeparatorText("End of Cuelist");
        {
            bool loop = (s_edit.end_behavior == UIState::PlaybackConf::EndBehavior::Loop);
            if (ImGui::RadioButton("Stop##end_stop", !loop))
                s_edit.end_behavior = UIState::PlaybackConf::EndBehavior::Stop;
            ImGui::SameLine();
            if (ImGui::RadioButton("Loop##end_loop", loop))
                s_edit.end_behavior = UIState::PlaybackConf::EndBehavior::Loop;
        }

        ImGui::SeparatorText("Playback Options");
        ImGui::Checkbox("Fade on first trigger##fade_first", &s_edit.fade_on_first_trigger);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When OFF (default), first cue snaps in immediately.\n"
                              "When ON, first cue uses its configured fade-in.");

        ImGui::SameLine(0, 12);
        ImGui::SetNextItemWidth(90.f);
        ImGui::DragFloat("1st Trigger Fade (s)##ftf", &s_edit.first_trigger_fade_s,
                         0.05f, 0.f, 60.f, "%.2f s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Override fade-in time (seconds) for the very first GO.\n"
                              "0 = respect 'Fade on first trigger' setting above.");

        ImGui::Checkbox("Remember cuelist position##rcp", &s_edit.remember_cuelist_position);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When OFF (default): playback resets to cue 1 when stopped.\n"
                              "When ON: position is remembered and resumes from the same cue.");

        ImGui::SeparatorText("BPM Sync");
        ImGui::Checkbox("GO at BPM##go_bpm", &s_edit.go_at_bpm);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically advance cue on each BPM beat");
        ImGui::SameLine(0, 12);
        ImGui::Checkbox("FX at BPM##fx_bpm", &s_edit.fx_at_bpm);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Run all FX at BPM tempo (rate=1 = 1 cycle per beat)");

        ImGui::SeparatorText("Output Assignment");
        if (state.patched_outputs.empty()) {
            ImGui::TextDisabled("No outputs patched.");
        } else {
            ImGui::TextDisabled("(none checked = all outputs)");
            for (const auto& po : state.patched_outputs) {
                bool assigned = std::find(s_edit.output_stream_ids.begin(),
                                          s_edit.output_stream_ids.end(),
                                          po.id) != s_edit.output_stream_ids.end();
                char lbl[64];
                std::snprintf(lbl, sizeof(lbl), "%s##os_%d", po.name.c_str(), po.id);
                if (ImGui::Checkbox(lbl, &assigned)) {
                    if (assigned) {
                        s_edit.output_stream_ids.push_back(po.id);
                    } else {
                        s_edit.output_stream_ids.erase(
                            std::remove(s_edit.output_stream_ids.begin(),
                                        s_edit.output_stream_ids.end(), po.id),
                            s_edit.output_stream_ids.end());
                    }
                }
            }
        }

        ImGui::SeparatorText("Keyboard GO Trigger");
        {
            char key_label[32] = "None";
            if (s_edit.keyboard_key != 0) {
                const char* kn = ImGui::GetKeyName(static_cast<ImGuiKey>(s_edit.keyboard_key));
                if (kn) std::snprintf(key_label, sizeof(key_label), "%s", kn);
            }
            ImGui::Text("Key: %s", key_label);
            ImGui::SameLine();
            if (s_edit.key_capturing) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.f, 1.f), "[press key...]");
                for (int k = static_cast<int>(ImGuiKey_Tab);
                     k < static_cast<int>(ImGuiKey_ReservedForModCtrl); ++k) {
                    if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k))) {
                        s_edit.keyboard_key  = k;
                        s_edit.key_capturing = false;
                        break;
                    }
                }
            } else {
                if (ImGui::Button("Capture##kc")) s_edit.key_capturing = true;
                ImGui::SameLine();
                if (ImGui::Button("Clear##kcl")) { s_edit.keyboard_key = 0; }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Apply##pba", ImVec2(100.f, 0.f))) {
            conf = s_edit;
            s_edit.key_capturing = false;
            if (cbs.on_playback_config) cbs.on_playback_config(ctx.pbconf_open_id, s_edit);
            ctx.pbconf_open_id = -1; s_edit_id = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine(0, 8.f);
        if (ImGui::Button("Cancel##pbc", ImVec2(100.f, 0.f))) {
            s_edit.key_capturing = false;
            ctx.pbconf_open_id = -1; s_edit_id = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (!open) { ctx.pbconf_open_id = -1; s_edit_id = -1; }
}

// ─────────────────────────────────────────────────────────────────────────────
//  SHOW view: 8-column x 5-row card grid
// ─────────────────────────────────────────────────────────────────────────────
static void draw_show_view_grid(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs,
                                 ImVec2 work_pos, ImVec2 work_size,
                                 float top_offset, float playback_h)
{
    static constexpr float kCardW = 140.f;
    static constexpr float kCardH = 160.f;
    static constexpr int   kCols  = 8;
    (void)kCols;

    // Per-slot rename state for the show-view grid
    static int  s_grid_rename_slot = -1;
    static char s_grid_rename_buf[64] = {};

    float grid_y = work_pos.y + top_offset;
    float grid_h = work_size.y - top_offset - playback_h;

    ImGuiWindowFlags gflags =
        ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::SetNextWindowPos(ImVec2(work_pos.x, grid_y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(work_size.x, grid_h), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(8.f, 8.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(4.f, 4.f));

    if (!ImGui::Begin("Playbacks##show_view_grid", nullptr, gflags)) {
        ImGui::PopStyleVar(4);
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar(4);

    ImGui::BeginChild("##show_scroll", ImVec2(-1.f, -1.f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);

    for (int i = 0; i < UIState::kMaxPlaybacks; ++i) {
        const UIState::PlaybackSnap& pb = state.snap.playbacks[i];
        bool unused = (pb.id == -1);

        // Start a new row every 8 cards
        if (i > 0 && (i % 8) != 0) ImGui::SameLine(0.f, 4.f);

        ImGui::PushID(i);

        ImVec4 card_bg = unused
            ? ImVec4(0.07f, 0.07f, 0.08f, 1.f)
            : (pb.active
               ? ImVec4(theme::accent().x * 0.20f, theme::accent().y * 0.20f,
                        theme::accent().z * 0.20f, 1.f)
               : ImVec4(0.12f, 0.13f, 0.16f, 1.f));

        ImGui::PushStyleColor(ImGuiCol_ChildBg, card_bg);
        ImGui::BeginChild("##pb_c", ImVec2(kCardW, kCardH), ImGuiChildFlags_Borders);

        // Name label — inline rename when s_grid_rename_slot == i
        if (s_grid_rename_slot == i) {
            ImGui::SetNextItemWidth(kCardW - ImGui::GetStyle().WindowPadding.x * 2.f);
            if (ImGui::InputText("##grid_rename", s_grid_rename_buf, sizeof(s_grid_rename_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                if (cbs.on_playback_rename && !unused)
                    cbs.on_playback_rename(pb.id, std::string(s_grid_rename_buf));
                s_grid_rename_slot = -1;
            }
            // Cancel on click outside
            if (!ImGui::IsItemActive() && !ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                s_grid_rename_slot = -1;
            }
        } else {
            ImVec4 tcol = unused ? ImVec4(0.25f, 0.25f, 0.28f, 1.f)
                         : (pb.active ? theme::accent()
                                      : theme::text_secondary());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(tcol));
            char nbuf[24];
            if (unused || pb.name.empty()) std::snprintf(nbuf, sizeof(nbuf), "PB %d", i + 1);
            else                           std::snprintf(nbuf, sizeof(nbuf), "%.14s", pb.name.c_str());
            ImGui::TextUnformatted(nbuf);
            ImGui::PopStyleColor();
            if (!unused && ImGui::IsItemHovered())
                ImGui::SetTooltip("Right-click to rename");
            // Right-click context menu on name label
            if (!unused && ImGui::BeginPopupContextItem("##grid_pb_name_ctx")) {
                if (ImGui::MenuItem("Rename")) {
                    s_grid_rename_slot = i;
                    std::strncpy(s_grid_rename_buf, pb.name.c_str(), sizeof(s_grid_rename_buf) - 1);
                    s_grid_rename_buf[sizeof(s_grid_rename_buf) - 1] = '\0';
                }
                ImGui::EndPopup();
            }
        }

        // CFG button — right side of name row
        {
            float cfg_w = 22.f;
            ImGui::SameLine(kCardW - cfg_w - ImGui::GetStyle().WindowPadding.x - 2.f);
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.18f, 0.22f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.35f, 1.f));
            if (ImGui::Button("CFG##cfg", ImVec2(cfg_w, 16.f)))
                ctx.pbconf_open_id = i + 1;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Configure playback %d: DMX trigger, loop mode, keyboard key", i + 1);
            ImGui::PopStyleColor(2);
        }

        ImGui::Separator();

        float item_h = ImGui::GetFrameHeight();
        float third_w = (kCardW - ImGui::GetStyle().WindowPadding.x * 2.f - 4.f) / 3.f;
        third_w = std::max(third_w, 14.f);

        // GO button (intercepts ctx.rec_armed to record instead of play; STP/REC removed)
        if (unused) ImGui::BeginDisabled(true);
        bool rec_go = !unused && ctx.rec_armed;
        if (rec_go) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.80f, 0.08f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.12f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.05f, 0.05f, 1.f));
        } else if (!unused && pb.active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.55f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.70f, 0.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.02f, 0.40f, 0.07f, 1.f));
        }
        if (ImGui::Button(rec_go ? "REC##go_c" : "GO##go_c", ImVec2(-1.f, item_h))) {
            if (rec_go && cbs.on_record_frame_to_playback) {
                FullCueEntry fce;
                fce.name = "Cue " + std::to_string(pb.id);
                fce.keyframe_layer.objects       = ctx.frame_editor.objects;
                fce.keyframe_layer.symmetry_mode = static_cast<int>(ctx.frame_editor.symmetry);
                fce.global_layer = ctx.programmer_global;
                fce.fx_layer     = ctx.programmer_fx_layer;
                fce.trigger.type = TriggerType::Follow;
                fce.timing.hold  = 2.f;
                // Populate per-stream FX from saved programmer feeds.
                // Only require that the feed has FX — objects may be empty if a stream
                // was selected purely for FX programming.
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    if (!feed.fx.fx.empty()) {
                        try {
                            int sid = std::stoi(key);
                            fce.per_stream_fx[sid] = feed.fx;
                        } catch (...) { /* skip combo keys like "1,2" */ }
                    }
                }
                // Save per-stream keyframe layers for multi-head programming
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    try {
                        int sid = std::stoi(key);
                        if (!feed.objects.empty()) {
                            KeyframeLayer kf;
                            kf.objects = feed.objects;
                            fce.per_stream_kf[sid] = kf;
                        }
                    } catch (...) {}
                }
                cbs.on_record_frame_to_playback(pb.id, fce);
                ctx.rec_armed = false;
            } else if (!unused && cbs.on_playback_go) {
                cbs.on_playback_go(pb.id);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(rec_go
                ? "Record programmer content to playback %d"
                : "GO: advance to next cue on playback %d", i + 1);
        if (rec_go || (!unused && pb.active)) ImGui::PopStyleColor(3);
        if (unused) ImGui::EndDisabled();

        // Current cue name
        {
            char cbuf[18] = "---";
            if (!unused && !pb.current_cue_name.empty())
                std::snprintf(cbuf, sizeof(cbuf), "%.14s", pb.current_cue_name.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
            ImGui::TextUnformatted(cbuf);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                if (!unused && !pb.current_cue_name.empty())
                    ImGui::SetTooltip("Active cue: %s", pb.current_cue_name.c_str());
                else
                    ImGui::SetTooltip("No active cue -- press GO to start");
            }
        }

        // FIRE button — replaces intensity slider; green when inactive, red when active
        if (unused) ImGui::BeginDisabled(true);
        {
            char fire_label_c[32];
            std::snprintf(fire_label_c, sizeof(fire_label_c), "FIRE##pbc_%d", i);
            if (!unused && pb.active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.15f, 0.15f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.65f, 0.15f, 1.0f));
            }
            if (ImGui::Button(fire_label_c, ImVec2(-1.f, 32.f))) {
                if (!unused && pb.active) {
                    if (cbs.on_playback_stop)
                        cbs.on_playback_stop(pb.id);
                } else if (!unused && cbs.on_playback_go) {
                    cbs.on_playback_go(pb.id);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(!unused && pb.active
                    ? "FIRE: active — click to STOP and release output"
                    : "FIRE: click to start playback");
            ImGui::PopStyleColor(3);
        }
        if (unused) ImGui::EndDisabled();

        // Cue count + current cue position
        if (!unused && pb.cue_count > 0) {
            char ci_buf[32];
            std::snprintf(ci_buf, sizeof(ci_buf), "Cue %d of %d", pb.current_cue + 1, pb.cue_count);
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
            ImGui::TextUnformatted(ci_buf);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Position in cuestack: cue %d of %d total\nDouble-click playback name to open cuestack",
                                  pb.current_cue + 1, pb.cue_count);
        } else if (!unused) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
            ImGui::TextUnformatted("No cues");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("No cues recorded -- use REC to add cues");
        } else {
            // Unused slot: hint for the operator
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.25f, 0.25f, 0.28f, 1.f));
            ImGui::TextUnformatted("(empty slot)");
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Empty playback slot -- press NEW in the playback bar to assign");
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();  // ChildBg

        // Active green border
        if (!unused && pb.active) {
            ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(p0, p1,
                ImGui::ColorConvertFloat4ToU32(theme::accent()), 3.f, 0, 2.f);
        }
        // Double-click opens cuestack
        if (!unused && ImGui::IsItemHovered() &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ctx.cuestack_selected_pb = pb.id;
            ctx.panel_timeline_open  = true;
            ctx.cuestack_open        = true;
        }
        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Playback bar — always visible strip at screen bottom
// ─────────────────────────────────────────────────────────────────────────────
void panel_playback_bar(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    static constexpr int kSlots = UIState::kMaxPlaybacks;
    static constexpr float kBarH = 200.f;  // default/initial height

    // Per-slot rename state
    static int   s_rename_slot = -1;
    static char  s_rename_buf[64] = {};

    ImGuiViewport* vp = ImGui::GetMainViewport();

    // Anchored window — pinned to the bottom every frame so it can't drift
    ImGuiWindowFlags bar_flags =
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse     | ImGuiWindowFlags_NoMove;

    static constexpr float kTlStripH = 36.f;  // timeline quick-fire strip height
    float init_h = kBarH + kTlStripH + ImGui::GetFrameHeight() + 4.f;
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - init_h),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(vp->WorkSize.x, init_h),
        ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.f);

    if (!ImGui::Begin("Playbacks##playback_bar", nullptr, bar_flags)) {
        ImGui::PopStyleVar(3);
        ImGui::End();
        return;
    }
    ImGui::PopStyleVar(3);

    // How wide should each of the 10 slots be?
    // Reserve ~60px on the right for the master fader.
    const float master_fader_w = 60.f;
    const float spacing        = ImGui::GetStyle().ItemSpacing.x;
    float avail_w = ImGui::GetContentRegionAvail().x - master_fader_w - spacing;
    float slot_w  = (avail_w - spacing * (float)(kSlots - 1)) / (float)kSlots;
    slot_w = std::max(slot_w, 80.f);  // never narrower than 80px

    // Track whether we've shown the NEW button yet (only on first empty slot)
    bool new_button_shown = false;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 2.f));

    for (int i = 0; i < kSlots; ++i) {
        const UIState::PlaybackSnap& pb = state.snap.playbacks[i];
        bool unused = (pb.id == -1);

        // Push ID scope per slot
        ImGui::PushID(i);

        // Highlight active slots with a different background
        if (!unused && pb.active) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg,
                ImVec4(theme::accent().x * 0.18f,
                       theme::accent().y * 0.18f,
                       theme::accent().z * 0.18f,
                       1.f));
        } else if (unused) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.08f, 1.f));
        } else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.12f, 0.15f, 1.f));
        }

        ImGui::BeginChild("##pb_slot", ImVec2(slot_w, 0.f), ImGuiChildFlags_Borders);

        float item_h = ImGui::GetFrameHeight();

        // ── Slot header: number + name ─────────────────────────────────────
        char header_buf[64];
        if (unused) {
            std::snprintf(header_buf, sizeof(header_buf), "PB %d", i + 1);
        } else if (!pb.name.empty()) {
            std::snprintf(header_buf, sizeof(header_buf), "%s", pb.name.c_str());
        } else {
            std::snprintf(header_buf, sizeof(header_buf), "PB %d", i + 1);
        }

        // Rename: activate on double-click of header
        if (s_rename_slot == i) {
            ImGui::SetNextItemWidth(slot_w - 8.f);
            if (ImGui::InputText("##rename", s_rename_buf, sizeof(s_rename_buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
                if (cbs.on_playback_rename && !unused)
                    cbs.on_playback_rename(pb.id, std::string(s_rename_buf));
                s_rename_slot = -1;
            }
            if (!ImGui::IsItemActive() && !ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                s_rename_slot = -1;
            }
        } else {
            if (!unused && pb.active) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(theme::accent()));
            } else if (unused) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.35f, 0.35f, 0.40f, 1.f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(theme::text_secondary()));
            }
            ImGui::TextUnformatted(header_buf);
            ImGui::PopStyleColor();
            if (!unused && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Double-click: open cuestack\nRight-click: rename / options");
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    ctx.cuestack_selected_pb = pb.id;
                    ctx.panel_timeline_open  = true;
                    ctx.cuestack_open        = true;
                }
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Right)) {
                    s_rename_slot = i;
                    std::strncpy(s_rename_buf, pb.name.c_str(), sizeof(s_rename_buf) - 1);
                    s_rename_buf[sizeof(s_rename_buf) - 1] = '\0';
                }
            }
            // Right-click context menu on the header
            if (!unused && ImGui::BeginPopupContextItem("##pb_hdr_ctx")) {
                if (ImGui::MenuItem("Rename")) {
                    s_rename_slot = i;
                    std::strncpy(s_rename_buf, pb.name.c_str(), sizeof(s_rename_buf) - 1);
                    s_rename_buf[sizeof(s_rename_buf) - 1] = '\0';
                }
                if (ImGui::MenuItem("Open Cuestack")) {
                    ctx.cuestack_selected_pb = pb.id;
                    ctx.panel_timeline_open  = true;
                    ctx.cuestack_open        = true;
                }
                ImGui::EndPopup();
            }
        }

        // ── Row 2: GO / STOP / REC buttons ────────────────────────────────
        // ── Controls (full width — fader replaced with FIRE button below) ──
        float ctrl_col_w = slot_w - 8.f;
        if (ctrl_col_w < 20.f) ctrl_col_w = 20.f;

        ImGui::BeginGroup();

        // GO button — full control-column width, green when active
        bool rec_armed_slot = !unused && ctx.rec_armed;
        if (rec_armed_slot) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.05f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.10f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.45f, 0.02f, 0.02f, 1.f));
        } else if (!unused && pb.active) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.05f, 0.55f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.10f, 0.70f, 0.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.02f, 0.40f, 0.07f, 1.f));
        }
        if (unused) ImGui::BeginDisabled(true);
        if (ImGui::Button(rec_armed_slot ? "REC##go" : "GO##go", ImVec2(ctrl_col_w, item_h))) {
            if (rec_armed_slot && cbs.on_record_frame_to_playback) {
                FullCueEntry fce;
                fce.name = "Cue " + std::to_string(pb.id);
                fce.keyframe_layer.objects       = ctx.frame_editor.objects;
                fce.keyframe_layer.symmetry_mode = static_cast<int>(ctx.frame_editor.symmetry);
                fce.global_layer = ctx.programmer_global;
                fce.fx_layer     = ctx.programmer_fx_layer;
                fce.trigger.type = TriggerType::Follow;
                fce.timing.hold  = 2.f;
                // Populate per-stream FX from saved programmer feeds.
                // Only require that the feed has FX — objects may be empty if a stream
                // was selected purely for FX programming.
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    if (!feed.fx.fx.empty()) {
                        try {
                            int sid = std::stoi(key);
                            fce.per_stream_fx[sid] = feed.fx;
                        } catch (...) { /* skip combo keys like "1,2" */ }
                    }
                }
                // Save per-stream keyframe layers for multi-head programming
                for (const auto& [key, feed] : ctx.programmer_feeds) {
                    try {
                        int sid = std::stoi(key);
                        if (!feed.objects.empty()) {
                            KeyframeLayer kf;
                            kf.objects = feed.objects;
                            fce.per_stream_kf[sid] = kf;
                        }
                    } catch (...) {}
                }
                cbs.on_record_frame_to_playback(pb.id, fce);
                ctx.rec_armed = false;
            } else if (!unused && cbs.on_playback_go) {
                cbs.on_playback_go(pb.id);
            }
        }
        if (unused) ImGui::EndDisabled();
        if (rec_armed_slot || (!unused && pb.active)) ImGui::PopStyleColor(3);

        // BACK (<) + CFG buttons (replaced BLN button)
        if (unused) ImGui::BeginDisabled(true);

        {
            float btn_w = (ctrl_col_w - 4.f) * 0.5f;
            std::string id_str = std::to_string(pb.id);
            if (ImGui::Button(("< ##back" + id_str).c_str(), ImVec2(btn_w, item_h))) {
                if (cbs.on_playback_cue_back) cbs.on_playback_cue_back(pb.id);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Go back to previous cue");
            ImGui::SameLine(0, 4.f);
            if (ImGui::Button(("CFG##cfg" + id_str).c_str(), ImVec2(btn_w, item_h)))
                ctx.pbconf_open_id = pb.id;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Open playback configuration");
        }

        ImGui::SameLine(0, 3.f);

        char cue_buf[48];
        if (unused || pb.current_cue_name.empty()) {
            std::snprintf(cue_buf, sizeof(cue_buf), "---");
        } else if (!unused && pb.fade_in_dur > 0.f) {
            float rem = pb.fade_in_dur - pb.fade_elapsed;
            std::snprintf(cue_buf, sizeof(cue_buf), "%.8s \xE2\x86\x92 %.1fs",
                          pb.current_cue_name.c_str(), std::max(0.f, rem));
        } else {
            std::snprintf(cue_buf, sizeof(cue_buf), "%.10s", pb.current_cue_name.c_str());
        }
        bool fading = !unused && pb.fade_in_dur > 0.f && pb.fade_elapsed < pb.fade_in_dur;
        ImGui::PushStyleColor(ImGuiCol_Text, fading
            ? ImGui::ColorConvertFloat4ToU32(ImVec4(1.f, 0.85f, 0.3f, 1.f))
            : ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
        ImGui::TextUnformatted(cue_buf);
        ImGui::PopStyleColor();

        if (unused) ImGui::EndDisabled();

        // NEW button — first unused slot only
        if (unused && !new_button_shown) {
            new_button_shown = true;
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::ColorConvertFloat4ToU32(theme::accent()));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(theme::accent().x * 1.2f,
                       theme::accent().y * 1.2f,
                       theme::accent().z * 1.2f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.05f, 0.05f, 0.05f, 1.f));
            if (ImGui::Button("NEW##new_pb", ImVec2(ctrl_col_w, item_h))) {
                if (cbs.on_playback_new) cbs.on_playback_new();
            }
            ImGui::PopStyleColor(3);
        }

        ImGui::EndGroup();

        // FIRE button — use whatever height is actually left after the controls group.
        // Clamp to available space so the button never clips out of the child region.
        if (unused) ImGui::BeginDisabled(true);
        {
            float fire_avail = ImGui::GetContentRegionAvail().y
                               - ImGui::GetStyle().ItemSpacing.y;
            // Never request more height than is available; never go below 12px.
            float fire_h = (fire_avail > 12.f) ? fire_avail : 12.f;
            char fire_label[32];
            std::snprintf(fire_label, sizeof(fire_label), "FIRE##pb_%d", i);
            if (!unused && pb.active) {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.8f, 0.1f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.0f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.15f, 0.15f, 1.0f));
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.6f, 0.1f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.15f, 0.65f, 0.15f, 1.0f));
            }
            if (ImGui::Button(fire_label, ImVec2(-1.f, fire_h))) {
                if (!unused && pb.active) {
                    if (cbs.on_playback_stop)
                        cbs.on_playback_stop(pb.id);
                } else if (!unused && cbs.on_playback_go) {
                    cbs.on_playback_go(pb.id);
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(!unused && pb.active
                    ? "FIRE: active — click to STOP and release output"
                    : "FIRE: click to start playback");
            ImGui::PopStyleColor(3);
        }
        if (unused) ImGui::EndDisabled();

        ImGui::EndChild();
        ImGui::PopStyleColor();  // ChildBg
        ImGui::PopID();

        if (i < kSlots - 1)
            ImGui::SameLine(0, spacing);
    }

    // ── MASTER vertical fader (right side) ─────────────────────────────────
    ImGui::SameLine(0, spacing);
    ImGui::BeginChild("##pb_master", ImVec2(master_fader_w - 2.f, 0.f),
                      ImGuiChildFlags_Borders);
    {
        float avail_h = ImGui::GetContentRegionAvail().y;
        ImGui::TextUnformatted("MSTR");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Master Intensity: scales the output of all active playbacks\n100%% = full output, 0%% = blackout");
        float fader_h = avail_h - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.y;
        fader_h = std::max(fader_h, 20.f);
        float master_val = state.master_intensity;
        ImGui::SetNextItemWidth(-1.f);
        if (ImGui::SliderFloat("##master_pb", &master_val, 0.f, 1.f,
                               "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            state.master_intensity = master_val;
            if (cbs.on_master_intensity) cbs.on_master_intensity(master_val);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Master Intensity: %.0f%%\nScales all playback outputs", master_val * 100.f);
    }
    ImGui::EndChild();

    ImGui::PopStyleVar();  // ItemSpacing

    // ── Timeline quick-fire strip ─────────────────────────────────────────────
    if (!state.timelines.empty()) {
        ImGui::Separator();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 3.f));
        ImGui::TextDisabled("Timelines:");
        ImGui::SameLine(0, 6);
        for (const auto& tl : state.timelines) {
            ImGui::PushID(tl.id.c_str());
            bool playing = (tl.state == TimelineState::Playing);
            bool armed   = (tl.state == TimelineState::Armed);
            // Color: green = playing, yellow = armed/paused, default = idle
            if (playing)     ImGui::PushStyleColor(ImGuiCol_Button, theme::kGreen);
            else if (armed)  ImGui::PushStyleColor(ImGuiCol_Button, theme::kYellow);
            const char* label = tl.name.empty() ? "???" : tl.name.c_str();
            float btn_w = std::max(60.f, ImGui::CalcTextSize(label).x + 16.f);
            if (ImGui::Button(label, { btn_w, 0.f })) {
                // Toggle play/stop like a playback GO button
                if (playing || tl.state == TimelineState::Paused) {
                    if (cbs.on_timeline_stop) cbs.on_timeline_stop(tl.id);
                } else {
                    if (cbs.on_timeline_play) cbs.on_timeline_play(tl.id);
                }
            }
            if (ImGui::IsItemHovered()) {
                const char* st = playing ? "PLAYING — click to stop"
                               : armed   ? "ARMED — click to play"
                               :           "Idle — click to play";
                ImGui::SetTooltip("%s\nTC: %s | %s",
                    st,
                    tc_to_string(tl.position_frames, tl.fps).c_str(),
                    tl.link_mode ? "Internal" : ("EXT: " + tl.tc_slot).c_str());
            }
            if (playing || armed) ImGui::PopStyleColor();
            ImGui::SameLine(0, 4);
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }

    ImGui::End();

    // PBCONF popup (always drawn so modal stays open)
    draw_pbconf_popup(state, ctx, cbs);

}

} // namespace idhmfis
