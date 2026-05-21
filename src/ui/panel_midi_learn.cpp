// panel_midi_learn.cpp — MIDI Learn panel.
// Shows the binding table and lets the user arm/delete bindings.

#include "layout.h"
#include "imgui.h"
#include "../input/midi_learn.h"

#include <cstdio>
#include <cmath>
#include <string>

namespace idhmfis {

static constexpr const char* kWinMidiLearn = "MIDI Learn##w";

static const char* midi_type_label(MidiMsgType t)
{
    switch (t) {
    case MidiMsgType::CC:          return "CC";
    case MidiMsgType::Note:        return "Note";
    case MidiMsgType::PitchBend:   return "PitchBend";
    case MidiMsgType::Aftertouch:  return "Aftertouch";
    }
    return "?";
}

void panel_midi_learn(UIState& state, LayoutCallbacks& cbs)
{
    // Enforce minimum size so the table is always readable.
    ImGui::SetNextWindowSizeConstraints(ImVec2(380.f, 200.f), ImVec2(FLT_MAX, FLT_MAX));

    if (!ImGui::Begin(kWinMidiLearn)) {
        ImGui::End();
        return;
    }

    // ── Armed status banner ──────────────────────────────────────────────────
    if (state.midi_learn_armed) {
        // Pulsing red/orange background
        float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 4.f);
        ImVec4 banner_col = { 0.7f + 0.3f * pulse, 0.2f, 0.05f, 1.f };
        ImGui::PushStyleColor(ImGuiCol_ChildBg, banner_col);
        ImGui::BeginChild("##armed_banner", ImVec2(0.f, 36.f), ImGuiChildFlags_Borders);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.f);
        ImGui::Text("ARMED — send a MIDI message to bind to: %s",
                    state.midi_learn_target.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Cancel")) {
            state.midi_learn_armed = false;
            state.midi_learn_target.clear();
        }
        ImGui::SetItemTooltip("Disarm MIDI learn without creating a binding.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // ── Toolbar ──────────────────────────────────────────────────────────────
    // Clear All is destructive — require Ctrl to confirm.
    bool ctrl_held = ImGui::GetIO().KeyCtrl;
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.55f, 0.12f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.72f, 0.18f, 0.14f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.08f, 0.07f, 1.f));
    ImGui::BeginDisabled(state.midi_bindings.empty() || !ctrl_held);
    if (ImGui::Button("Clear All")) {
        if (cbs.on_midi_clear) cbs.on_midi_clear();
        state.midi_bindings.clear();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(
        "Remove all MIDI bindings. Hold Ctrl then click to confirm.\n"
        "This cannot be undone.");

    ImGui::SameLine();
    ImGui::TextDisabled("(%d binding%s)",
                        (int)state.midi_bindings.size(),
                        state.midi_bindings.size() == 1 ? "" : "s");

    ImGui::Separator();

    // ── Binding table ────────────────────────────────────────────────────────
    ImGui::SeparatorText("Bindings");

    if (state.midi_bindings.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No MIDI bindings configured.");
        ImGui::TextDisabled("Use the LEARN buttons below or the LEARN buttons in the Inspector.");
        ImGui::Spacing();
    } else {
        constexpr ImGuiTableFlags kTableFlags =
            ImGuiTableFlags_Borders          |
            ImGuiTableFlags_RowBg            |
            ImGuiTableFlags_SizingStretchProp|
            ImGuiTableFlags_ScrollY;

        // Reserve height leaving room for the Quick LEARN section.
        float table_h = ImGui::GetContentRegionAvail().y
                      - ImGui::GetTextLineHeightWithSpacing() * 12.f;
        if (table_h < 60.f) table_h = 60.f;

        if (ImGui::BeginTable("##midi_bindings", 7, kTableFlags, ImVec2(0.f, table_h))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Type",    ImGuiTableColumnFlags_WidthFixed,   72.f);
            ImGui::TableSetupColumn("Ch",      ImGuiTableColumnFlags_WidthFixed,   32.f);
            ImGui::TableSetupColumn("Num",     ImGuiTableColumnFlags_WidthFixed,   42.f);
            ImGui::TableSetupColumn("Target",  ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Min",     ImGuiTableColumnFlags_WidthFixed,   50.f);
            ImGui::TableSetupColumn("Max",     ImGuiTableColumnFlags_WidthFixed,   50.f);
            ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthFixed,   56.f);
            ImGui::TableHeadersRow();

            int del_idx = -1;
            for (int i = 0; i < (int)state.midi_bindings.size(); ++i) {
                const MidiBinding& b = state.midi_bindings[static_cast<size_t>(i)];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(midi_type_label(b.type));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MIDI message type");

                ImGui::TableSetColumnIndex(1);
                if (b.channel == 0)
                    ImGui::TextDisabled("Any");
                else
                    ImGui::Text("%d", b.channel);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MIDI channel (1-16, or Any for all channels)");

                ImGui::TableSetColumnIndex(2);
                if (b.type == MidiMsgType::PitchBend || b.type == MidiMsgType::Aftertouch)
                    ImGui::TextDisabled("—");
                else
                    ImGui::Text("%d", b.number);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("CC number or Note number");

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(b.target.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Engine parameter controlled by this binding:\n%s",
                                      b.target.c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%.2f", b.min_val);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Minimum output value when MIDI is at 0");

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%.2f", b.max_val);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Maximum output value when MIDI is at 127");

                ImGui::TableSetColumnIndex(6);
                ImGui::PushID(i);
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.50f, 0.10f, 0.10f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.18f, 0.14f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.06f, 0.06f, 1.f));
                if (ImGui::SmallButton("Delete"))
                    del_idx = i;
                ImGui::PopStyleColor(3);
                ImGui::SetItemTooltip("Remove this MIDI binding.");
                ImGui::PopID();
            }

            if (del_idx >= 0) {
                const std::string& target = state.midi_bindings[static_cast<size_t>(del_idx)].target;
                if (cbs.on_midi_unbind) cbs.on_midi_unbind(target);
                state.midi_bindings.erase(state.midi_bindings.begin() + del_idx);
            }

            ImGui::EndTable();
        }
    }

    // ── Quick-arm section ────────────────────────────────────────────────────
    ImGui::Spacing();
    ImGui::SeparatorText("Quick LEARN");
    ImGui::TextDisabled("Arm a common target then send a MIDI message to bind it.");
    ImGui::Spacing();

    struct QuickTarget { const char* label; const char* target; const char* tip; };
    static constexpr QuickTarget kQuickTargets[] = {
        { "Master Intensity", "master_intensity",
          "Controls the master output intensity (0-100%)." },
        { "BPM",              "bpm",
          "Controls the beat tempo in BPM." },
        { "Speed",            "speed",
          "Controls cue playback speed multiplier." },
        { "Scale",            "scale",
          "Controls the overall scale / zoom of the laser output." },
        { "Density",          "density",
          "Controls the point density along beam paths." },
        { "Pan",              "pan",
          "Controls horizontal position offset." },
        { "Tilt",             "tilt",
          "Controls vertical position offset." },
    };

    for (const auto& qt : kQuickTargets) {
        bool already_armed = state.midi_learn_armed && state.midi_learn_target == qt.target;

        // Show armed button in orange; unarmed in default style.
        if (already_armed) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.80f, 0.30f, 0.05f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.40f, 0.08f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.60f, 0.22f, 0.03f, 1.f));
        }

        char btn_label[64];
        std::snprintf(btn_label, sizeof(btn_label),
                      already_armed ? "ARMED##%s" : "LEARN##%s", qt.target);

        if (ImGui::SmallButton(btn_label)) {
            if (already_armed) {
                state.midi_learn_armed = false;
                state.midi_learn_target.clear();
            } else {
                state.midi_learn_armed  = true;
                state.midi_learn_target = qt.target;
            }
        }
        if (already_armed) ImGui::PopStyleColor(3);

        ImGui::SetItemTooltip(
            "%s\nTarget: %s\n%s\n\nClick to %s MIDI learn for this target.",
            qt.label, qt.target, qt.tip,
            already_armed ? "cancel" : "arm");

        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(qt.label);
    }

    ImGui::End();
}

} // namespace idhmfis
