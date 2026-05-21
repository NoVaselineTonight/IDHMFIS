#pragma once
// IDHMFIS — Command Palette (Ctrl+K overlay)
// Fuzzy-searches app actions, cues by name, parameters, and recent files.
// Keyboard navigation: arrows, Enter to activate, Escape to close.

#include <string>
#include <vector>
#include <functional>
#include "imgui.h"

namespace idhmfis {
    struct UIState;
    class  ShortcutRegistry;
}

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Command palette entry
// ─────────────────────────────────────────────────────────────────────────────
enum class CommandCategory {
    Action,
    Cue,
    Parameter,
    RecentFile
};

struct PaletteEntry {
    CommandCategory category;
    std::string     icon;      // Short text icon, e.g., "▶", "Q", "P", "F"
    std::string     name;
    std::string     shortcut;  // Display chord, e.g., "Ctrl+N"
    std::string     detail;    // Subtitle / path info
    std::function<void()> action;
};

// ─────────────────────────────────────────────────────────────────────────────
//  CommandPalette
// ─────────────────────────────────────────────────────────────────────────────
class CommandPalette {
public:
    CommandPalette();

    // Populate the action list once at startup. Re-call when cues change.
    void build_entries(const UIState& state,
                       const ShortcutRegistry& shortcuts);

    // Add a custom action entry (called by app for dynamic entries)
    void add_action(PaletteEntry entry);

    // Open the palette. Call in response to Ctrl+K.
    void open();

    // Close the palette.
    void close();

    bool is_open() const { return open_; }

    // Render the palette. Must be called every frame (renders nothing when closed).
    // Returns the activated entry's action (nullptr if no activation this frame).
    bool render(UIState& state);

private:
    bool open_     = false;
    bool just_opened_ = false;

    char search_buf_[256] = {};
    int  selected_idx_    = 0;

    std::vector<PaletteEntry> all_entries_;
    std::vector<int>          filtered_indices_;

    void filter_entries();
    float fuzzy_score(const std::string& text, const std::string& query) const;

    // Category labels
    const char* category_label(CommandCategory c) const;
    ImVec4      category_color(CommandCategory c) const;
};

} // namespace idhmfis
