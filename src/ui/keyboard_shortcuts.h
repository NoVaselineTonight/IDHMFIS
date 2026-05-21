#pragma once
// IDHMFIS — Central keyboard shortcut registry.
// Call process() once per frame BEFORE rendering panels.
// Shortcut callbacks are registered at startup; the system checks
// ImGui::GetIO().KeysDown and modifier state, then fires the callback.

#include <functional>
#include <string>
#include <vector>
#include "imgui.h"

namespace idhmfis {

// Forward declaration
struct UIState;

// ─────────────────────────────────────────────────────────────────────────────
//  Shortcut descriptor
// ─────────────────────────────────────────────────────────────────────────────
struct Shortcut {
    std::string          name;        // Human-readable ("New Project")
    std::string          chord;       // Display string ("Ctrl+N")
    ImGuiKey             key;
    bool                 ctrl  = false;
    bool                 shift = false;
    bool                 alt   = false;
    std::function<void()> action;
    bool                 repeat = false; // fire every frame while held?
};

// ─────────────────────────────────────────────────────────────────────────────
//  ShortcutRegistry
// ─────────────────────────────────────────────────────────────────────────────
class ShortcutRegistry {
public:
    ShortcutRegistry() = default;

    // Register all app-wide shortcuts. Call once at startup passing
    // lambdas that mutate UIState / post commands.
    void register_all(UIState& state,
                      std::function<void()> cb_new,
                      std::function<void()> cb_open,
                      std::function<void()> cb_save,
                      std::function<void()> cb_save_as,
                      std::function<void()> cb_undo,
                      std::function<void()> cb_redo,
                      std::function<void()> cb_command_palette,
                      std::function<void()> cb_fullscreen,
                      std::function<void()> cb_duplicate_cue,
                      std::function<void()> cb_delete_selected,
                      std::function<void()> cb_select_all,
                      std::function<void()> cb_help);

    // Process all shortcuts for this frame. Skip when any ImGui text input
    // widget has keyboard focus (avoids eating typed characters).
    void process();

    // Accessor for the command palette (to display shortcut hints)
    const std::vector<Shortcut>& all() const { return shortcuts_; }

    // Find a shortcut chord string by name
    std::string chord_for(const std::string& name) const;

private:
    std::vector<Shortcut> shortcuts_;

    bool is_pressed(const Shortcut& s) const;
    void push(Shortcut s);
};

} // namespace idhmfis
