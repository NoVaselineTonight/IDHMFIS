// panel_zones.cpp — Zone routing panel for multi-projector output configuration.
// Docked alongside Cue Library in the left panel.

#include "layout.h"
#include "imgui.h"

#include <cstdio>
#include <algorithm>
#include <string>

namespace idhmfis {

static constexpr const char* kWinZones = "Zones##w";

static const char* kPatternNames[] = {
    "Circle",
    "Crosshair",
    "Box",
    "Star",
    "Scanlines",
};
static constexpr int kPatternCount = 5;

// ─────────────────────────────────────────────────────────────────────────────
//  panel_zones
// ─────────────────────────────────────────────────────────────────────────────
void panel_zones(UIState& state, LayoutContext& ctx, LayoutCallbacks& cbs)
{
    // Enforce a minimum window size so controls are never clipped.
    ImGui::SetNextWindowSizeConstraints(ImVec2(260.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin(kWinZones)) {
        ImGui::End();
        return;
    }

    // ── Header: Add Zone button ───────────────────────────────────────────────
    if (ImGui::Button("+ Add Zone"))
    {
        if (cbs.on_zone_set)
        {
            Zone z;
            z.id   = 0;   // 0 = "please create a new zone"
            z.name = "Zone " + std::to_string(static_cast<int>(state.zones.size()) + 1);
            cbs.on_zone_set(z);
        }
    }
    ImGui::SetItemTooltip(
        "Create a new output zone. Each zone maps a portion of the cue output "
        "to a specific DAC/projector with its own transform and color correction.");

    ImGui::SameLine();
    ImGui::TextDisabled("(%d zone%s)", (int)state.zones.size(),
                        state.zones.size() == 1 ? "" : "s");

    ImGui::Separator();

    // ── Empty state placeholder ───────────────────────────────────────────────
    if (state.zones.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No zones configured.");
        ImGui::TextDisabled("Click '+ Add Zone' to create the first output zone.");
        ImGui::Spacing();
        ImGui::End();
        return;
    }

    // ── Zone list ─────────────────────────────────────────────────────────────
    // Track whether a removal was requested (defer until after iteration).
    int remove_id = -1;

    for (Zone& zone : state.zones)
    {
        // Status dot: green = enabled, grey = disabled
        ImVec2 cursor = ImGui::GetCursorScreenPos();
        float  dot_r  = ImGui::GetTextLineHeight() * 0.38f;
        ImU32  dot_col = zone.enabled
            ? IM_COL32(50, 210, 80, 255)
            : IM_COL32(110, 110, 110, 200);
        ImGui::GetWindowDrawList()->AddCircleFilled(
            ImVec2(cursor.x + dot_r, cursor.y + dot_r + 2.f), dot_r, dot_col);
        ImGui::Dummy(ImVec2(dot_r * 2.f + 4.f, ImGui::GetTextLineHeight()));
        ImGui::SameLine(0, 4);

        // Tree node label: "Zone N — DAC name (or 'No DAC')"
        std::string dac_label = zone.dac_id.empty() ? "No DAC" : zone.dac_id;
        char node_label[128];
        std::snprintf(node_label, sizeof(node_label),
                      "%s  [%s]###zone_%d",
                      zone.name.c_str(), dac_label.c_str(), zone.id);

        bool node_open = ImGui::TreeNodeEx(
            node_label,
            (ctx.zones_selected == zone.id ? ImGuiTreeNodeFlags_Selected : 0) |
            ImGuiTreeNodeFlags_SpanAvailWidth);

        if (ImGui::IsItemClicked())
            ctx.zones_selected = zone.id;

        if (node_open)
        {
            bool dirty = false;

            // ── Enabled / Solo / Blind toggles ──────────────────────────────
            ImGui::SeparatorText("Routing");

            dirty |= ImGui::Checkbox("Enabled##z", &zone.enabled);
            ImGui::SetItemTooltip("Route cue output to this zone. Uncheck to mute this zone.");
            ImGui::SameLine();
            dirty |= ImGui::Checkbox("Solo##z",    &zone.solo);
            ImGui::SetItemTooltip(
                "Solo: mute all other zones and output only this zone. "
                "Multiple zones can be soloed simultaneously.");
            ImGui::SameLine();
            dirty |= ImGui::Checkbox("Blind##z",   &zone.blind);
            ImGui::SetItemTooltip(
                "Blind: edits to this zone are not sent to the physical output. "
                "Useful for pre-programming while the show is live.");

            ImGui::Spacing();

            // ── DAC selection ────────────────────────────────────────────────
            ImGui::SeparatorText("Device");

            {
                // Build combo items from available_dacs.
                int current_item = 0; // default: None
                for (int di = 0; di < static_cast<int>(state.available_dacs.size()); ++di)
                {
                    if (state.available_dacs[di].id == zone.dac_id)
                    {
                        current_item = di + 1;
                        break;
                    }
                }

                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::BeginCombo("##dac", current_item == 0
                        ? "No DAC"
                        : state.available_dacs[current_item - 1].name.c_str()))
                {
                    bool sel_none = (current_item == 0);
                    if (ImGui::Selectable("No DAC", sel_none))
                    {
                        zone.dac_id = "";
                        dirty = true;
                    }
                    if (sel_none) ImGui::SetItemDefaultFocus();

                    for (int di = 0; di < static_cast<int>(state.available_dacs.size()); ++di)
                    {
                        const DacDescriptor& d = state.available_dacs[di];
                        bool selected = (current_item == di + 1);
                        if (ImGui::Selectable(d.name.c_str(), selected))
                        {
                            zone.dac_id = d.id;
                            dirty = true;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip(
                    "Physical DAC/ILDA device that receives this zone's output.");

                // Show a warning when no DAC is assigned and zone is enabled.
                if (zone.enabled && zone.dac_id.empty()) {
                    ImGui::SameLine(0, 8);
                    ImGui::TextColored(ImVec4(1.f, 0.7f, 0.1f, 1.f), "(!) No device");
                }
            }

            // ── Transform controls ───────────────────────────────────────────
            ImGui::Spacing();
            ImGui::SeparatorText("Transform");

            float offset[2] = { zone.transform.offset_x, zone.transform.offset_y };
            if (ImGui::DragFloat2("Offset XY##z", offset, 0.005f, -1.f, 1.f, "%.3f"))
            {
                zone.transform.offset_x = offset[0];
                zone.transform.offset_y = offset[1];
                dirty = true;
            }
            ImGui::SetItemTooltip(
                "Translate the zone output in normalized coordinates (-1..1). "
                "Drag to nudge position.");

            float scale[2] = { zone.transform.scale_x, zone.transform.scale_y };
            if (ImGui::DragFloat2("Scale XY##z", scale, 0.01f, 0.1f, 4.f, "%.3f"))
            {
                zone.transform.scale_x = scale[0];
                zone.transform.scale_y = scale[1];
                dirty = true;
            }
            ImGui::SetItemTooltip(
                "Scale the zone output independently on each axis. "
                "1.0 = no scaling, 2.0 = double size.");

            dirty |= ImGui::DragFloat("Rotation##z", &zone.transform.rotation,
                                      0.5f, -360.f, 360.f, "%.1f deg");
            ImGui::SetItemTooltip("Rotate the zone output clockwise in degrees.");

            float shear[2] = { zone.transform.shear_x, zone.transform.shear_y };
            if (ImGui::DragFloat2("Shear XY##z", shear, 0.005f, -1.f, 1.f, "%.3f"))
            {
                zone.transform.shear_x = shear[0];
                zone.transform.shear_y = shear[1];
                dirty = true;
            }
            ImGui::SetItemTooltip(
                "Apply horizontal/vertical shear (keystone correction). "
                "Use to correct trapezoidal distortion from an angled projector.");

            // ── Intensity ────────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::SeparatorText("Levels");

            dirty |= ImGui::SliderFloat("Intensity##z", &zone.intensity, 0.f, 1.f, "%.0f%%");
            ImGui::SetItemTooltip(
                "Master intensity for this zone (0%% = off, 100%% = full). "
                "Applied on top of the cue master intensity.");

            // ── Colour correction ────────────────────────────────────────────
            float col[3] = { zone.color_r, zone.color_g, zone.color_b };
            if (ImGui::SliderFloat3("RGB Scale##z", col, 0.f, 1.f, "%.2f"))
            {
                zone.color_r = col[0];
                zone.color_g = col[1];
                zone.color_b = col[2];
                dirty = true;
            }
            ImGui::SetItemTooltip(
                "Per-channel color scaling for white balance / color matching "
                "between projectors (1.0 = no correction).");

            // ── Test pattern ─────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::SeparatorText("Test Pattern");

            dirty |= ImGui::Checkbox("Show Test Pattern##z", &zone.show_test);
            ImGui::SetItemTooltip(
                "Output a geometric test pattern to this zone instead of cue content. "
                "Useful for alignment and calibration.");
            if (zone.show_test)
            {
                ImGui::SameLine(0, 8);
                ImGui::SetNextItemWidth(130.f);
                if (ImGui::BeginCombo("##tp",
                    (zone.test_pattern >= 0 && zone.test_pattern < kPatternCount)
                        ? kPatternNames[zone.test_pattern]
                        : "Circle"))
                {
                    for (int pi = 0; pi < kPatternCount; ++pi)
                    {
                        bool sel = (zone.test_pattern == pi);
                        if (ImGui::Selectable(kPatternNames[pi], sel))
                        {
                            zone.test_pattern = pi;
                            dirty = true;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetItemTooltip("Select the geometric test pattern to display.");
            }

            // ── Remove button ────────────────────────────────────────────────
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.6f, 0.1f, 0.1f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.5f, 0.05f, 0.05f, 1.f));
            char rm_label[48];
            std::snprintf(rm_label, sizeof(rm_label), "Remove Zone '%s'##zrm%d",
                          zone.name.c_str(), zone.id);

            // Require Ctrl to confirm zone removal (destructive operation).
            bool ctrl_held = ImGui::GetIO().KeyCtrl;
            ImGui::BeginDisabled(!ctrl_held);
            if (ImGui::Button(rm_label))
            {
                remove_id = zone.id;
            }
            ImGui::EndDisabled();
            ImGui::PopStyleColor(3);
            ImGui::SetItemTooltip(
                "Permanently remove this zone. Hold Ctrl then click to confirm.\n"
                "All settings for this zone will be lost.");

            ImGui::TreePop();

            // Push updated zone back to engine when any value changed.
            if (dirty && cbs.on_zone_set)
                cbs.on_zone_set(zone);
        }
    }

    // Deferred removal — safe to do after the iteration completes.
    if (remove_id >= 0) {
        if (cbs.on_zone_remove)
            cbs.on_zone_remove(remove_id);
        if (ctx.zones_selected == remove_id)
            ctx.zones_selected = -1;
    }

    ImGui::End();
}

} // namespace idhmfis
