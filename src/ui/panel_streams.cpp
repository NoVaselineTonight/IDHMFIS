// panel_streams.cpp — STREAMS selection window.
// Two-section layout:
//   1. OUTPUTS — patched laser/NDI/HDMI outputs (grid, drag-reorderable)
//   2. GROUPS  — numbered group slots (always visible); REC+click empty = record;
//                click occupied = activate group (select heads + restore programmer)

#include "layout.h"
#include "imgui.h"
#include "theme.h"
#include "../core/types.h"

#include <cstdio>
#include <algorithm>

namespace idhmfis {

static constexpr const char* kWinStreams  = "STREAMS##streams_win";
static constexpr const char* kGrpModalId  = "Record Output Group##rec_grp_modal";

// How many group slots are shown even when empty
static constexpr int kGroupSlotCount = 16;

static ImVec4 stream_type_color_s(OutputStreamType t) {
    switch (t) {
    case OutputStreamType::Laser: return ImVec4(0.0f, 1.0f, 0.45f, 1.0f);
    case OutputStreamType::NDI:   return ImVec4(0.0f, 0.9f, 1.0f,  1.0f);
    case OutputStreamType::HDMI:  return ImVec4(1.0f, 0.7f, 0.0f,  1.0f);
    }
    return ImVec4(0.7f, 0.7f, 0.7f, 1.0f);
}

static const char* stream_type_label_s(OutputStreamType t) {
    switch (t) {
    case OutputStreamType::Laser: return "L";
    case OutputStreamType::NDI:   return "N";
    case OutputStreamType::HDMI:  return "H";
    }
    return "?";
}

// ── MagicQ-style programmer feeds ────────────────────────────────────────────

static std::string feeds_key_s(const std::vector<int>& ids) {
    std::vector<int> s = ids;
    std::sort(s.begin(), s.end());
    std::string key;
    for (int id : s) {
        if (!key.empty()) key += ',';
        key += std::to_string(id);
    }
    return key;
}

static void feeds_switch_s(const std::vector<int>& old_ids,
                            const std::vector<int>& new_ids,
                            LayoutContext& ctx) {
    // Save each departing stream's programmer state for future reference.
    for (int old_id : old_ids) {
        bool leaving = std::find(new_ids.begin(), new_ids.end(), old_id) == new_ids.end();
        if (leaving) {
            ProgrammerFeedState& s = ctx.programmer_feeds[std::to_string(old_id)];
            s.objects = ctx.frame_editor.objects;
            s.global  = ctx.programmer_global;
            s.fx      = ctx.programmer_fx_layer;
        }
    }
    // Restore saved content when exactly one stream becomes selected AND that stream
    // was not already in the previous selection.  If the stream was continuously selected
    // (i.e., it appears in both old_ids and new_ids), the current programmer state is
    // already the live state for that head — restoring a stale saved snapshot here
    // would corrupt the programmer and desync Frame FX across streams.
    if (new_ids.size() == 1) {
        const int target_id = new_ids[0];
        bool was_already_selected = std::find(old_ids.begin(), old_ids.end(),
                                              target_id) != old_ids.end();
        if (!was_already_selected) {
            const std::string key = std::to_string(target_id);
            auto it = ctx.programmer_feeds.find(key);
            if (it != ctx.programmer_feeds.end() &&
                (!it->second.objects.empty() || !it->second.fx.fx.empty() || !it->second.global.fx.empty())) {
                ctx.frame_editor.objects = it->second.objects;
                ctx.programmer_global   = it->second.global;
                ctx.programmer_fx_layer = it->second.fx;
            }
        }
    }
}

// Head-based recording: capture current programmer state for these outputs now.
static void feeds_capture_s(const std::vector<int>& ids, LayoutContext& ctx) {
    ProgrammerFeedState& slot = ctx.programmer_feeds[feeds_key_s(ids)];
    slot.objects = ctx.frame_editor.objects;
    slot.global  = ctx.programmer_global;
    slot.fx      = ctx.programmer_fx_layer;
    // Also save under each individual stream key so feeds_switch_s can restore
    // per-head state when a stream is re-added to the selection.
    for (int id : ids) {
        ProgrammerFeedState& s = ctx.programmer_feeds[std::to_string(id)];
        s.objects = ctx.frame_editor.objects;
        s.global  = ctx.programmer_global;
        s.fx      = ctx.programmer_fx_layer;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void panel_streams(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs) {
    if (!ImGui::Begin(kWinStreams, &ctx.streams_window_open,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    // ── Global style polish ───────────────────────────────────────────────────
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.f, 4.f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(6.f, 5.f));

    static bool s_show_grp_modal = false;
    static bool s_grp_mirror_per[32] = {};   // per-member mirror flags (indexed by position in active_stream_ids)
    static char s_grp_name[64]   = {};
    static int  s_pending_slot   = -1;  // group slot index (0-based) being recorded to

    // Output grid settings
    static int s_grid_cols = 4;
    static int s_grid_rows = 2;
    static std::vector<int> s_cell_order;  // output_id or -1 (empty)

    // Sync output cell order
    {
        for (auto& cid : s_cell_order) {
            if (cid < 0) continue;
            bool found = false;
            for (const auto& po : state.patched_outputs)
                if (po.id == cid) { found = true; break; }
            if (!found) cid = -1;
        }
        for (const auto& po : state.patched_outputs) {
            bool in_grid = false;
            for (int cid : s_cell_order)
                if (cid == po.id) { in_grid = true; break; }
            if (!in_grid) {
                bool placed = false;
                for (auto& cid : s_cell_order)
                    if (cid == -1) { cid = po.id; placed = true; break; }
                if (!placed) s_cell_order.push_back(po.id);
            }
        }
    }

    const std::vector<int> prev_stream_ids = state.active_stream_ids;
    bool streams_changed = false;

    // ── Header row ────────────────────────────────────────────────────────────
    ImGui::TextDisabled("Programmer targets:");
    ImGui::SetItemTooltip(
        "Select outputs that receive programmer content.\n"
        "None selected = programmer outputs to NONE. Each stream keeps its own programmed content.");
    ImGui::SameLine(0, 12.f);
    if (ImGui::SmallButton("All") && !state.patched_outputs.empty()) {
        std::vector<int> old_ids_all = state.active_stream_ids;
        std::vector<int> new_ids;
        for (const auto& po : state.patched_outputs) new_ids.push_back(po.id);
        feeds_switch_s(old_ids_all, new_ids, ctx);
        state.active_stream_ids = std::move(new_ids);
        state.active_group_id = -1;
        if (cbs.on_active_streams_changed)
            cbs.on_active_streams_changed(old_ids_all, state.active_stream_ids);
    }
    ImGui::SameLine(0, 4.f);
    if (ImGui::SmallButton("None")) {
        std::vector<int> old_ids_none = state.active_stream_ids;
        feeds_switch_s(old_ids_none, {}, ctx);
        state.active_stream_ids.clear();
        state.active_group_id = -1;
        if (cbs.on_active_streams_changed)
            cbs.on_active_streams_changed(old_ids_none, state.active_stream_ids);
    }
    ImGui::SetItemTooltip("Deselect all — programmer sends to NONE. Each stream keeps its own programmed content.");

    // Grid column settings (gear)
    ImGui::SameLine(0, 8.f);
    if (ImGui::SmallButton("[=]##sg_cfg")) ImGui::OpenPopup("##sg_settings");
    ImGui::SetItemTooltip("Grid settings");
    if (ImGui::BeginPopup("##sg_settings")) {
        ImGui::TextUnformatted("Grid Settings");
        ImGui::Separator();
        ImGui::SetNextItemWidth(140.f);
        ImGui::SliderInt("Columns##sgc", &s_grid_cols, 1, 8, "%d",
                         ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetNextItemWidth(140.f);
        ImGui::SliderInt("Min Rows##sgr", &s_grid_rows, 1, 8, "%d",
                         ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndPopup();
    }

    ImGui::Separator();

    // ── OUTPUTS grid ──────────────────────────────────────────────────────────
    const float avail_w  = ImGui::GetContentRegionAvail().x;
    const float cell_gap = 4.f;
    const float cell_w   = (avail_w - cell_gap * (static_cast<float>(s_grid_cols) - 1.f))
                           / static_cast<float>(s_grid_cols);
    const float cell_h   = 38.f;

    const int n_out = static_cast<int>(state.patched_outputs.size());
    int n_cells_auto = (n_out > 0)
        ? ((n_out + s_grid_cols - 1) / s_grid_cols) * s_grid_cols
        : s_grid_cols;
    int n_cells_min  = s_grid_rows * s_grid_cols;
    const int n_cells = std::max(n_cells_auto, n_cells_min);
    while (static_cast<int>(s_cell_order.size()) < n_cells)
        s_cell_order.push_back(-1);

    for (int i = 0; i < n_cells; ++i) {
        const int output_id = (i < static_cast<int>(s_cell_order.size()))
                              ? s_cell_order[i] : -1;

        if (i % s_grid_cols != 0) ImGui::SameLine(0.f, cell_gap);

        UIState::PatchedOutput* po_ptr = nullptr;
        if (output_id >= 0)
            for (auto& po : state.patched_outputs)
                if (po.id == output_id) { po_ptr = &po; break; }

        // Apply consistent FrameRounding for all feed selector buttons
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.f);

        // Empty output cell
        if (!po_ptr) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.08f, 0.08f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ctx.stream_move_mode
                    ? ImVec4(0.22f, 0.22f, 0.30f, 1.f)
                    : ImVec4(0.10f, 0.10f, 0.12f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.10f, 0.10f, 0.12f, 1.f));
            char eid[32]; std::snprintf(eid, sizeof(eid), "##eo_%d", i);
            ImGui::Button(eid, ImVec2(cell_w, cell_h));
            if (ctx.stream_move_mode && ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("OUT_CELL");
                if (pl) {
                    int src = *static_cast<const int*>(pl->Data);
                    if (src >= 0 && src < static_cast<int>(s_cell_order.size()))
                        std::swap(s_cell_order[src], s_cell_order[i]);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar();  // FrameRounding
            continue;
        }

        // Disabled output
        if (!po_ptr->enabled) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f,0.10f,0.12f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.12f,0.12f,0.14f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.08f,0.08f,0.10f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          theme::kTextDim);
            char bid[32]; std::snprintf(bid, sizeof(bid), "##od_%d", po_ptr->id);
            ImGui::Button(bid, ImVec2(cell_w, cell_h));
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();  // FrameRounding
            continue;
        }

        bool active = std::find(state.active_stream_ids.begin(),
                                state.active_stream_ids.end(),
                                po_ptr->id) != state.active_stream_ids.end();
        ImVec4 tc = stream_type_color_s(po_ptr->type);

        if (ctx.stream_move_mode) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f,0.22f,0.28f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f,0.30f,0.38f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f,0.18f,0.24f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
        } else if (state.rem_mode) {
            // REM armed: full red delete tint on every output cell
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.55f, 0.08f, 0.06f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(0.80f, 0.12f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.40f, 0.06f, 0.04f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.75f, 0.75f, 1.f));
        } else if (active) {
            // Active feed: type-color tint with a green overlay at 15% alpha
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(tc.x*0.45f + 0.f*0.15f,
                       tc.y*0.45f + theme::kGreen.y*0.15f,
                       tc.z*0.45f + theme::kGreen.z*0.15f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(tc.x*0.65f, tc.y*0.65f, tc.z*0.65f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(tc.x*0.35f, tc.y*0.35f, tc.z*0.35f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
        } else {
            // Inactive: use kTextDim for the stream name
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f,0.18f,0.22f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f,0.25f,0.30f,1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.14f,0.14f,0.18f,1.f));
            ImGui::PushStyleColor(ImGuiCol_Text,          theme::kTextDim);
        }

        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "[%s] %s###out_%d",
                      stream_type_label_s(po_ptr->type), po_ptr->name.c_str(), po_ptr->id);
        ImGui::Button(lbl, ImVec2(cell_w, cell_h));
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();  // FrameRounding

        if (ctx.stream_move_mode) {
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("OUT_CELL", &i, sizeof(int));
                ImGui::Text("[%s] %s", stream_type_label_s(po_ptr->type), po_ptr->name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("OUT_CELL");
                if (pl) {
                    int src = *static_cast<const int*>(pl->Data);
                    if (src >= 0 && src < static_cast<int>(s_cell_order.size()))
                        std::swap(s_cell_order[src], s_cell_order[i]);
                }
                ImGui::EndDragDropTarget();
            }
        } else if (state.rem_mode) {
            if (ImGui::IsItemClicked()) {
                const int del_id = po_ptr->id;
                std::vector<int> old_ids_rem = state.active_stream_ids;
                // Remove from active selection
                state.active_stream_ids.erase(
                    std::remove(state.active_stream_ids.begin(),
                                state.active_stream_ids.end(), del_id),
                    state.active_stream_ids.end());
                if (state.active_group_id >= 0) {
                    // Clear group if it referenced this output
                    for (const auto& g : state.output_groups)
                        if (g.id == state.active_group_id)
                            for (int mid : g.member_ids)
                                if (mid == del_id) { state.active_group_id = -1; break; }
                }
                // Unpatch: delete from patched_outputs entirely
                state.patched_outputs.erase(
                    std::remove_if(state.patched_outputs.begin(),
                                   state.patched_outputs.end(),
                                   [del_id](const UIState::PatchedOutput& p) {
                                       return p.id == del_id;
                                   }),
                    state.patched_outputs.end());
                if (cbs.on_active_streams_changed)
                    cbs.on_active_streams_changed(old_ids_rem, state.active_stream_ids);
                if (cbs.on_output_patch_changed) {
                    std::vector<OutputStreamConfig> new_patch;
                    for (const auto& po2 : state.patched_outputs)
                        new_patch.push_back(po2.config);
                    cbs.on_output_patch_changed(new_patch);
                }
                state.rem_mode = false;
                // po_ptr invalidated — stop processing the cell loop
                break;
            }
            ImGui::SetItemTooltip("REM: click to UNPATCH and delete this output");
        } else if (ImGui::IsItemClicked()) {
            state.active_group_id = -1;
            if (active)
                state.active_stream_ids.erase(
                    std::remove(state.active_stream_ids.begin(),
                                state.active_stream_ids.end(), po_ptr->id),
                    state.active_stream_ids.end());
            else
                state.active_stream_ids.push_back(po_ptr->id);
            streams_changed = true;
        }
        if (!state.rem_mode) {
            ImGui::SetItemTooltip(
                ctx.stream_move_mode ? "Drag to reorder" :
                active ? "Click to deselect" : "Click to select");
        }

        // Right-click on output cell → per-output settings
        if (!state.rem_mode && !ctx.stream_move_mode) {
            char popid[32];
            std::snprintf(popid, sizeof(popid), "##out_cfg_%d", po_ptr->id);
            if (ImGui::BeginPopupContextItem(popid)) {
                ImGui::TextUnformatted(po_ptr->name.c_str());
                ImGui::Separator();
                bool fx = po_ptr->config.transform.flip_x;
                bool fy = po_ptr->config.transform.flip_y;
                if (ImGui::Checkbox("Flip X (invert horizontal)##fx", &fx)) {
                    po_ptr->config.transform.flip_x = fx;
                    if (cbs.on_output_patch_changed) {
                        std::vector<OutputStreamConfig> cfgs;
                        for (const auto& po2 : state.patched_outputs)
                            cfgs.push_back(po2.config);
                        cbs.on_output_patch_changed(cfgs);
                    }
                }
                if (ImGui::Checkbox("Flip Y (invert vertical)##fy", &fy)) {
                    po_ptr->config.transform.flip_y = fy;
                    if (cbs.on_output_patch_changed) {
                        std::vector<OutputStreamConfig> cfgs;
                        for (const auto& po2 : state.patched_outputs)
                            cfgs.push_back(po2.config);
                        cbs.on_output_patch_changed(cfgs);
                    }
                }
                ImGui::EndPopup();
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── GROUPS grid ───────────────────────────────────────────────────────────
    // Always shows kGroupSlotCount slots. Occupied = named group. Empty = dashed.
    // REC mode: click any slot (empty OR occupied) → record/overwrite group.
    {
        // Groups grid uses the same column count as the output grid
        const int grp_cols = s_grid_cols;
        const float gw = (avail_w - cell_gap * (static_cast<float>(grp_cols) - 1.f))
                         / static_cast<float>(grp_cols);
        const float gh = 34.f;

        // Section label
        if (ctx.rec_armed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.35f, 1.f));
            ImGui::TextUnformatted("GROUPS  [REC: click slot to record]");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("GROUPS");
        }
        ImGui::Spacing();

        for (int slot = 0; slot < kGroupSlotCount; ++slot) {
            if (slot % grp_cols != 0) ImGui::SameLine(0.f, cell_gap);

            // Find group occupying this slot (slot stored in group.id mapped 0-based)
            const UIState::OutputGroup* grp = nullptr;
            for (const auto& g : state.output_groups)
                if (g.id == slot) { grp = &g; break; }

            bool grp_selected = grp && (state.active_group_id == slot);

            char btn_lbl[96];
            if (grp)
                std::snprintf(btn_lbl, sizeof(btn_lbl), "[G%d] %s###gs_%d",
                              slot + 1, grp->name.c_str(), slot);
            else
                std::snprintf(btn_lbl, sizeof(btn_lbl), "G%d###gs_%d", slot + 1, slot);

            if (state.rem_mode && grp) {
                // REM armed + occupied: aggressive red to signal destructive delete
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.70f, 0.10f, 0.08f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.90f, 0.15f, 0.10f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.55f, 0.08f, 0.06f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.f, 0.75f, 0.75f, 1.f));
            } else if (state.rem_mode && !grp) {
                // REM armed + empty: keep dim empty slot appearance
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.08f, 0.08f, 0.11f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.11f, 0.11f, 0.16f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.10f, 0.10f, 0.14f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.28f, 0.28f, 0.35f, 1.f));
            } else if (ctx.rec_armed) {
                // REC highlight
                ImGui::PushStyleColor(ImGuiCol_Button,
                    grp ? ImVec4(0.45f, 0.10f, 0.10f, 1.f)
                        : ImVec4(0.30f, 0.07f, 0.07f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.70f, 0.15f, 0.15f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.55f, 0.12f, 0.12f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(1.f, 0.80f, 0.80f, 1.f));
            } else if (grp_selected) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.18f, 0.30f, 0.72f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.25f, 0.40f, 0.90f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.14f, 0.24f, 0.60f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.85f, 0.92f, 1.f, 1.f));
            } else if (grp) {
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.12f, 0.16f, 0.38f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.18f, 0.24f, 0.52f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.10f, 0.13f, 0.30f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.55f, 0.68f, 1.f, 1.f));
            } else {
                // Empty slot — dashed/dim appearance
                ImGui::PushStyleColor(ImGuiCol_Button,
                    ImVec4(0.08f, 0.08f, 0.11f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    ImVec4(0.11f, 0.11f, 0.16f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    ImVec4(0.10f, 0.10f, 0.14f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImVec4(0.28f, 0.28f, 0.35f, 1.f));
            }

            bool clicked = ImGui::Button(btn_lbl, ImVec2(gw, gh));
            ImGui::PopStyleColor(4);

            if (state.rem_mode) {
                ImGui::SetItemTooltip(
                    grp ? "REM: click to DELETE this group"
                        : "Empty — nothing to remove");
            } else if (ctx.rec_armed) {
                ImGui::SetItemTooltip(
                    grp
                    ? "REC: overwrite this group with current selection."
                    : "REC: record current output selection as a new group here.");
            } else if (grp) {
                ImGui::SetItemTooltip(
                    grp_selected
                    ? "Click to deselect group."
                    : "Click to activate group (selects heads, restores programmer state).");
            } else {
                ImGui::SetItemTooltip(
                    "Empty group slot. Arm REC then click to record a group here.");
            }

            if (clicked) {
                if (state.rem_mode && grp) {
                    // REM delete: remove the group, clear active state if needed
                    if (state.active_group_id == slot) {
                        std::vector<int> old_ids_grem = state.active_stream_ids;
                        feeds_switch_s(old_ids_grem, {}, ctx);
                        state.active_group_id = -1;
                        state.active_stream_ids.clear();
                        if (cbs.on_active_streams_changed)
                            cbs.on_active_streams_changed(old_ids_grem, state.active_stream_ids);
                    }
                    state.output_groups.erase(
                        std::remove_if(state.output_groups.begin(),
                                       state.output_groups.end(),
                                       [slot](const UIState::OutputGroup& g) {
                                           return g.id == slot;
                                       }),
                        state.output_groups.end());
                    state.rem_mode = false;
                    // iterator invalidated — must break out of the slot loop
                    break;
                } else if (ctx.rec_armed) {
                    // Record current selection to this slot
                    if (!state.active_stream_ids.empty()) {
                        std::snprintf(s_grp_name, sizeof(s_grp_name), "Group %d", slot + 1);
                        std::fill(std::begin(s_grp_mirror_per), std::end(s_grp_mirror_per), false);
                        s_pending_slot = slot;
                        s_show_grp_modal = true;
                        ImGui::OpenPopup(kGrpModalId);
                    }
                } else if (grp) {
                    std::vector<int> old_ids_grp = state.active_stream_ids;
                    if (grp_selected) {
                        // Capture combo key before switching away so that
                        // re-selecting this group later restores its programmer content.
                        feeds_capture_s(old_ids_grp, ctx);
                        feeds_switch_s(old_ids_grp, {}, ctx);
                        state.active_group_id = -1;
                        state.active_stream_ids.clear();
                        if (cbs.on_mirrored_streams_changed)
                            cbs.on_mirrored_streams_changed({});
                    } else {
                        feeds_switch_s(old_ids_grp, grp->member_ids, ctx);
                        // Restore group programmer state from combo key if available.
                        // feeds_switch_s only restores single-stream state (new_ids.size()==1),
                        // so multi-member groups need this explicit combo-key lookup.
                        {
                            const std::string combo_key = feeds_key_s(grp->member_ids);
                            auto it = ctx.programmer_feeds.find(combo_key);
                            if (it != ctx.programmer_feeds.end()) {
                                const auto& saved = it->second;
                                if (!saved.objects.empty() || !saved.fx.fx.empty() || !saved.global.fx.empty()) {
                                    ctx.frame_editor.objects = saved.objects;
                                    ctx.programmer_global    = saved.global;
                                    ctx.programmer_fx_layer  = saved.fx;
                                }
                            }
                        }
                        state.active_group_id   = slot;
                        state.active_stream_ids = grp->member_ids;
                        if (cbs.on_mirrored_streams_changed)
                            cbs.on_mirrored_streams_changed(grp->mirrored_ids);
                    }
                    if (cbs.on_active_streams_changed)
                        cbs.on_active_streams_changed(old_ids_grp, state.active_stream_ids);
                }
            }

            // Right-click on occupied slot → delete
            if (grp) {
                char ctx_id[32];
                std::snprintf(ctx_id, sizeof(ctx_id), "##gctx_%d", slot);
                if (ImGui::BeginPopupContextItem(ctx_id)) {
                    char del_lbl[64];
                    std::snprintf(del_lbl, sizeof(del_lbl), "Delete Group %d", slot + 1);
                    if (ImGui::MenuItem(del_lbl)) {
                        if (state.active_group_id == slot) {
                            std::vector<int> old_ids_gctx = state.active_stream_ids;
                            feeds_switch_s(old_ids_gctx, {}, ctx);
                            state.active_group_id = -1;
                            state.active_stream_ids.clear();
                            if (cbs.on_active_streams_changed)
                                cbs.on_active_streams_changed(old_ids_gctx, state.active_stream_ids);
                        }
                        state.output_groups.erase(
                            std::remove_if(state.output_groups.begin(),
                                           state.output_groups.end(),
                                           [slot](const UIState::OutputGroup& g) {
                                               return g.id == slot;
                                           }),
                            state.output_groups.end());
                        ImGui::EndPopup();
                        break;  // iterator invalidated
                    }
                    ImGui::EndPopup();
                }
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Status line ───────────────────────────────────────────────────────────
    if (state.rem_mode) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.35f, 1.f));
        ImGui::TextUnformatted("REM armed — click an output to UNPATCH it, or a group slot to delete it");
        ImGui::PopStyleColor();
    } else if (state.patched_outputs.empty()) {
        ImGui::TextDisabled("No outputs — go to OUTPUTS to add outputs.");
    } else if (ctx.stream_move_mode) {
        ImGui::TextDisabled("MOVE — drag output cells to reorder");
    } else if (ctx.rec_armed) {
        if (state.active_stream_ids.empty())
            ImGui::TextDisabled("REC armed — select outputs then click a group slot.");
        else
            ImGui::TextDisabled("REC armed — %d output(s) selected. Click a group slot.",
                                static_cast<int>(state.active_stream_ids.size()));
    } else if (state.active_group_id >= 0) {
        const UIState::OutputGroup* ag = nullptr;
        for (const auto& g : state.output_groups)
            if (g.id == state.active_group_id) { ag = &g; break; }
        if (ag)
            ImGui::TextDisabled("Group %d: %s  |  %d outputs",
                state.active_group_id + 1,
                ag->name.c_str(),
                static_cast<int>(ag->member_ids.size()));
        else
            state.active_group_id = -1;
    } else if (state.active_stream_ids.empty()) {
        ImGui::TextDisabled("All outputs (broadcast)");
    } else {
        ImGui::TextDisabled("%d output(s) selected",
                            static_cast<int>(state.active_stream_ids.size()));
    }

    // ── Record Group modal ────────────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(320.f, 0.f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal(kGrpModalId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Record group in slot G%d  (%d outputs selected).",
                    s_pending_slot + 1,
                    static_cast<int>(state.active_stream_ids.size()));
        ImGui::Spacing();
        ImGui::SetNextItemWidth(220.f);
        ImGui::InputText("Name##grp_name", s_grp_name, sizeof(s_grp_name));
        if (!state.active_stream_ids.empty()) {
            ImGui::Spacing();
            ImGui::TextUnformatted("Mirror members (X-axis flip):");
            int n_members = static_cast<int>(state.active_stream_ids.size());
            if (n_members > 32) n_members = 32;
            for (int mi = 0; mi < n_members; ++mi) {
                int mid = state.active_stream_ids[static_cast<size_t>(mi)];
                const char* mname = "Output";
                for (const auto& po : state.patched_outputs)
                    if (po.id == mid) { mname = po.name.c_str(); break; }
                char mlbl[96];
                std::snprintf(mlbl, sizeof(mlbl), "[%d] %s##mirchk_%d", mi + 1, mname, mi);
                ImGui::Checkbox(mlbl, &s_grp_mirror_per[mi]);
            }
        }
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool do_ok     = ImGui::Button("Record##grp_ok", ImVec2(100.f, 0.f));
        ImGui::SameLine(0, 8.f);
        bool do_cancel = ImGui::Button("Cancel##grp_cancel", ImVec2(100.f, 0.f));

        if (do_ok && s_pending_slot >= 0) {
            // Remove any existing group in this slot
            state.output_groups.erase(
                std::remove_if(state.output_groups.begin(),
                               state.output_groups.end(),
                               [&](const UIState::OutputGroup& g) {
                                   return g.id == s_pending_slot;
                               }),
                state.output_groups.end());

            UIState::OutputGroup grp;
            grp.id         = s_pending_slot;  // slot IS the id
            grp.name       = (s_grp_name[0] != '\0') ? s_grp_name : "Group";
            grp.member_ids = state.active_stream_ids;
            {
                int n_members = static_cast<int>(state.active_stream_ids.size());
                if (n_members > 32) n_members = 32;
                for (int mi = 0; mi < n_members; ++mi)
                    if (s_grp_mirror_per[mi])
                        grp.mirrored_ids.push_back(state.active_stream_ids[static_cast<size_t>(mi)]);
            }
            state.output_groups.push_back(grp);
            // Don't advance output_group_next_id since we use slot as id

            // Head-based recording: snapshot programmer state for these outputs
            feeds_capture_s(grp.member_ids, ctx);

            s_show_grp_modal = false;
            s_pending_slot   = -1;
            ImGui::CloseCurrentPopup();
        }
        if (do_cancel) {
            s_show_grp_modal = false;
            s_pending_slot   = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    (void)s_show_grp_modal;

    if (streams_changed) {
        feeds_switch_s(prev_stream_ids, state.active_stream_ids, ctx);
        if (cbs.on_active_streams_changed)
            cbs.on_active_streams_changed(prev_stream_ids, state.active_stream_ids);
    }

    // ── Close global style overrides ─────────────────────────────────────────
    ImGui::PopStyleVar(2);  // FramePadding, ItemSpacing

    ImGui::End();
}

} // namespace idhmfis
