// IDHMFIS — Command Palette implementation

#include "command_palette.h"
#include "ui_state.h"
#include "keyboard_shortcuts.h"
#include "theme.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cstdio>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────
CommandPalette::CommandPalette() {
    std::memset(search_buf_, 0, sizeof(search_buf_));
}

void CommandPalette::open() {
    open_         = true;
    just_opened_  = true;
    selected_idx_ = 0;
    std::memset(search_buf_, 0, sizeof(search_buf_));
    filter_entries();
}

void CommandPalette::close() {
    open_ = false;
}

void CommandPalette::add_action(PaletteEntry entry) {
    all_entries_.push_back(std::move(entry));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build entries from state + shortcut registry
// ─────────────────────────────────────────────────────────────────────────────
void CommandPalette::build_entries(const UIState& state,
                                   const ShortcutRegistry& shortcuts)
{
    all_entries_.clear();

    // --- Standard app actions -------------------------------------------------
    auto add_act = [&](const char* icon, const char* name, std::function<void()> fn) {
        PaletteEntry e;
        e.category = CommandCategory::Action;
        e.icon     = icon;
        e.name     = name;
        e.shortcut = shortcuts.chord_for(name);
        e.action   = std::move(fn);
        all_entries_.push_back(std::move(e));
    };

    // These lambdas capture nothing meaningful at build time; the app wires
    // real callbacks via add_action() after construction.
    add_act("N", "New Project",       [] {});
    add_act("O", "Open Project",      [] {});
    add_act("S", "Save Project",      [] {});
    add_act("S", "Save As",           [] {});
    add_act("Z", "Undo",              [] {});
    add_act("Z", "Redo",              [] {});
    add_act("▶", "Play/Pause",        [] {});
    add_act("■", "Stop",              [] {});
    add_act("D", "Duplicate Cue",     [] {});
    add_act("X", "Delete Selected",   [] {});
    add_act("A", "Select All",        [] {});
    add_act("F", "Fullscreen Preview",[] {});
    add_act("?", "Help",              [] {});

    // --- Cues from state ------------------------------------------------------
    for (const auto& cue : state.cues) {
        PaletteEntry e;
        e.category = CommandCategory::Cue;
        e.icon     = "Q";
        e.name     = cue.name;
        e.detail   = "Cue #" + std::to_string(cue.index + 1)
                   + " — " + cue.generator;
        int idx    = cue.index;
        e.action   = [idx, &state]() mutable {
            // In a real impl this would post a SelectCue command.
            // Here we capture for display purposes only.
            (void)idx; (void)state;
        };
        all_entries_.push_back(std::move(e));
    }

    // --- Generator parameters (fixed set matching GeneratorParams) -----------
    static const char* param_names[] = {
        "Speed", "Scale", "Density",
        "Param A", "Param B", "Param C",
        "Color A", "Color B",
        "Rotation", "Pan", "Tilt", "Zoom",
        "Intensity", "Point Count"
    };
    for (auto* pn : param_names) {
        PaletteEntry e;
        e.category = CommandCategory::Parameter;
        e.icon     = "P";
        e.name     = pn;
        e.detail   = "Generator parameter";
        e.action   = [] {};
        all_entries_.push_back(std::move(e));
    }

    // --- Recent files ---------------------------------------------------------
    for (const auto& path : state.recent_files) {
        PaletteEntry e;
        e.category = CommandCategory::RecentFile;
        e.icon     = "F";
        // Use filename portion as name
        size_t slash = path.find_last_of("/\\");
        e.name   = (slash != std::string::npos) ? path.substr(slash + 1) : path;
        e.detail = path;
        std::string p = path;
        e.action = [p] {};
        all_entries_.push_back(std::move(e));
    }

    filter_entries();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Fuzzy scoring — simple substring match with bonus for prefix
// ─────────────────────────────────────────────────────────────────────────────
float CommandPalette::fuzzy_score(const std::string& text,
                                  const std::string& query) const
{
    if (query.empty()) return 1.f;

    // Lowercase compare
    std::string t = text, q = query;
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    for (char& c : q) c = (char)std::tolower((unsigned char)c);

    size_t pos = t.find(q);
    if (pos == std::string::npos) {
        // Try character-by-character subsequence match
        size_t qi = 0;
        for (size_t ti = 0; ti < t.size() && qi < q.size(); ++ti) {
            if (t[ti] == q[qi]) ++qi;
        }
        if (qi == q.size())
            return 0.3f;
        return 0.f;
    }
    // Prefix match scores higher
    return pos == 0 ? 1.f : 0.7f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Filter
// ─────────────────────────────────────────────────────────────────────────────
void CommandPalette::filter_entries() {
    filtered_indices_.clear();

    std::string query(search_buf_);

    std::vector<std::pair<float, int>> scored;
    scored.reserve(all_entries_.size());

    for (int i = 0; i < (int)all_entries_.size(); ++i) {
        const auto& e = all_entries_[i];
        float score = fuzzy_score(e.name, query);
        if (score > 0.f)
            scored.push_back({ score, i });
    }

    // Sort by score descending, then by name ascending
    std::stable_sort(scored.begin(), scored.end(),
        [&](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            return all_entries_[a.second].name < all_entries_[b.second].name;
        });

    for (auto& [sc, idx] : scored)
        filtered_indices_.push_back(idx);

    selected_idx_ = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Category helpers
// ─────────────────────────────────────────────────────────────────────────────
const char* CommandPalette::category_label(CommandCategory c) const {
    switch (c) {
        case CommandCategory::Action:     return "ACTION";
        case CommandCategory::Cue:        return "CUE";
        case CommandCategory::Parameter:  return "PARAM";
        case CommandCategory::RecentFile: return "FILE";
    }
    return "";
}

ImVec4 CommandPalette::category_color(CommandCategory c) const {
    switch (c) {
        case CommandCategory::Action:     return theme::accent();
        case CommandCategory::Cue:        return theme::accent_warm();
        case CommandCategory::Parameter:  return theme::success();
        case CommandCategory::RecentFile: return theme::text_secondary();
    }
    return theme::text_disabled();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render
// ─────────────────────────────────────────────────────────────────────────────
bool CommandPalette::render(UIState& /*state*/) {
    if (!open_) return false;

    // Full-screen dimmed overlay
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(0.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.f, 0.f, 0.f, 0.f));

    ImGuiWindowFlags overlay_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking;

    ImGui::Begin("##palette_overlay", nullptr, overlay_flags);

    // Draw dim background via draw list
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(0.f, 0.f), io.DisplaySize,
        IM_COL32(0, 0, 0, 160));

    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    // Palette window
    float pal_w   = std::min(620.f, io.DisplaySize.x - 40.f);
    float pal_x   = (io.DisplaySize.x - pal_w) * 0.5f;
    float pal_y   = io.DisplaySize.y * 0.18f;
    float max_h   = io.DisplaySize.y * 0.6f;

    ImGui::SetNextWindowPos(ImVec2(pal_x, pal_y), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(pal_w, 0.f), ImGuiCond_Always);
    ImGui::SetNextWindowSizeConstraints(ImVec2(pal_w, 0.f), ImVec2(pal_w, max_h));

    ImGui::PushStyleColor(ImGuiCol_WindowBg,   ImGui::ColorConvertFloat4ToU32(theme::surface()));
    ImGui::PushStyleColor(ImGuiCol_Border,     ImGui::ColorConvertFloat4ToU32(theme::accent()));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
    ImGui::PushStyleVar (ImGuiStyleVar_WindowRounding, theme::kLargeRounding);
    ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 1.5f);

    ImGuiWindowFlags pal_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;

    bool activated = false;

    if (ImGui::Begin("##command_palette", nullptr, pal_flags)) {
        // --- Search box -------------------------------------------------------
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertFloat4ToU32(theme::background()));
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2(12.f, 10.f));
        ImGui::PushItemWidth(pal_w);

        if (just_opened_) {
            ImGui::SetKeyboardFocusHere();
            just_opened_ = false;
        }

        bool search_changed = ImGui::InputText("##palette_search",
                                               search_buf_, sizeof(search_buf_),
                                               ImGuiInputTextFlags_None);
        if (search_changed) {
            filter_entries();
            selected_idx_ = 0;
        }

        ImGui::PopItemWidth();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // --- Separator --------------------------------------------------------
        ImGui::GetWindowDrawList()->AddLine(
            ImGui::GetCursorScreenPos(),
            ImVec2(ImGui::GetCursorScreenPos().x + pal_w, ImGui::GetCursorScreenPos().y),
            ImGui::ColorConvertFloat4ToU32(theme::border_color()));
        ImGui::Dummy(ImVec2(pal_w, 1.f));

        // --- Keyboard navigation ----------------------------------------------
        int n = (int)filtered_indices_.size();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) {
            selected_idx_ = (selected_idx_ + 1) % std::max(n, 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) {
            selected_idx_ = (selected_idx_ - 1 + std::max(n, 1)) % std::max(n, 1);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            close();
        }

        // --- Results list -----------------------------------------------------
        ImGui::BeginChild("##palette_results",
                          ImVec2(pal_w, std::min((float)n * 40.f + 8.f, max_h - 56.f)),
                          ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);

        CommandCategory last_cat = CommandCategory::RecentFile;
        bool first = true;

        for (int ri = 0; ri < n; ++ri) {
            const auto& entry = all_entries_[filtered_indices_[ri]];

            // Category header
            if (first || entry.category != last_cat) {
                if (!first) ImGui::Dummy(ImVec2(0.f, 2.f));
                ImGui::SetCursorPosX(12.f);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(theme::text_secondary()));
                ImGui::TextUnformatted(category_label(entry.category));
                ImGui::PopStyleColor();
                last_cat = entry.category;
                first = false;
            }

            bool is_selected = (ri == selected_idx_);
            ImGui::PushID(ri);

            ImVec2 item_pos = ImGui::GetCursorScreenPos();
            float  item_h   = 36.f;

            if (is_selected) {
                ImGui::GetWindowDrawList()->AddRectFilled(
                    item_pos,
                    ImVec2(item_pos.x + pal_w, item_pos.y + item_h),
                    ImGui::ColorConvertFloat4ToU32(theme::surface2()));
            }

            ImGui::SetCursorPosX(12.f);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (item_h - ImGui::GetTextLineHeight()) * 0.5f);

            // Icon badge
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertFloat4ToU32(category_color(entry.category)));
            ImGui::Text("[%s]", entry.icon.c_str());
            ImGui::PopStyleColor();

            ImGui::SameLine(0.f, 8.f);

            // Name
            ImGui::PushStyleColor(ImGuiCol_Text,
                is_selected
                    ? ImGui::ColorConvertFloat4ToU32(theme::text_primary())
                    : ImGui::ColorConvertFloat4ToU32(theme::text_secondary()));
            ImGui::TextUnformatted(entry.name.c_str());
            ImGui::PopStyleColor();

            // Shortcut hint (right-aligned)
            if (!entry.shortcut.empty()) {
                float sc_w = ImGui::CalcTextSize(entry.shortcut.c_str()).x + 16.f;
                ImGui::SameLine(pal_w - sc_w - 12.f);
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
                ImGui::TextUnformatted(entry.shortcut.c_str());
                ImGui::PopStyleColor();
            }

            // Detail subtitle
            if (!entry.detail.empty()) {
                float prev_y = ImGui::GetCursorPosY();
                ImGui::SetCursorPos(ImVec2(50.f, item_pos.y - ImGui::GetWindowPos().y
                                           + ImGui::GetTextLineHeight() + 2.f));
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
                ImGui::TextUnformatted(entry.detail.c_str());
                ImGui::PopStyleColor();
                ImGui::SetCursorPosY(prev_y);
            }

            // Advance cursor by item height
            ImGui::SetCursorScreenPos(ImVec2(item_pos.x, item_pos.y + item_h));

            // Click
            ImGui::InvisibleButton("##entry", ImVec2(pal_w, item_h));
            if (ImGui::IsItemHovered()) selected_idx_ = ri;
            if (ImGui::IsItemClicked()) {
                if (entry.action) entry.action();
                close();
                activated = true;
            }

            ImGui::PopID();
        }

        if (n == 0) {
            ImGui::SetCursorPosX(12.f);
            ImGui::PushStyleColor(ImGuiCol_Text,
                ImGui::ColorConvertFloat4ToU32(theme::text_disabled()));
            ImGui::TextUnformatted("No results");
            ImGui::PopStyleColor();
        }

        // Enter to activate selected
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && n > 0) {
            const auto& entry = all_entries_[filtered_indices_[selected_idx_]];
            if (entry.action) entry.action();
            close();
            activated = true;
        }

        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    return activated;
}

} // namespace idhmfis
