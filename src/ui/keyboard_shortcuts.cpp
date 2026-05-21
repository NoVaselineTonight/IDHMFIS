// IDHMFIS — Keyboard shortcut registry implementation

#include "keyboard_shortcuts.h"
#include "ui_state.h"
#include "imgui.h"
#include <algorithm>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
bool ShortcutRegistry::is_pressed(const Shortcut& s) const {
    ImGuiIO& io = ImGui::GetIO();

    bool mod_ok = (io.KeyCtrl  == s.ctrl)  &&
                  (io.KeyShift == s.shift) &&
                  (io.KeyAlt   == s.alt);
    if (!mod_ok) return false;

    if (s.repeat)
        return ImGui::IsKeyDown(s.key);
    else
        return ImGui::IsKeyPressed(s.key, /*repeat=*/false);
}

void ShortcutRegistry::push(Shortcut s) {
    shortcuts_.push_back(std::move(s));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Registration
// ─────────────────────────────────────────────────────────────────────────────
void ShortcutRegistry::register_all(
    UIState& state,
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
    std::function<void()> cb_help)
{
    shortcuts_.clear();

    // --- File -----------------------------------------------------------------
    push({ "New Project",    "Ctrl+N",         ImGuiKey_N,      true,  false, false, std::move(cb_new) });
    push({ "Open Project",   "Ctrl+O",         ImGuiKey_O,      true,  false, false, std::move(cb_open) });
    push({ "Save Project",   "Ctrl+S",         ImGuiKey_S,      true,  false, false, std::move(cb_save) });
    push({ "Save As",        "Ctrl+Shift+S",   ImGuiKey_S,      true,  true,  false, std::move(cb_save_as) });

    // --- Edit -----------------------------------------------------------------
    push({ "Undo",           "Ctrl+Z",         ImGuiKey_Z,      true,  false, false, std::move(cb_undo) });
    push({ "Redo",           "Ctrl+Shift+Z",   ImGuiKey_Z,      true,  true,  false, std::move(cb_redo) });
    // Also handle Ctrl+Y for redo
    push({ "Redo (Alt)",     "Ctrl+Y",         ImGuiKey_Y,      true,  false, false, cb_redo }); // cb_redo copy OK

    // --- Transport ------------------------------------------------------------
    push({ "Play/Pause",     "Space",          ImGuiKey_Space,  false, false, false,
        [&state]() {
            state.playing = !state.playing;
            state.paused  = !state.playing;
        }
    });
    push({ "Stop",           "Escape",         ImGuiKey_Escape, false, false, false,
        [&state]() {
            state.playing = false;
            state.paused  = false;
            state.playhead_s = 0.0;
        }
    });

    // --- View -----------------------------------------------------------------
    push({ "Command Palette", "Ctrl+K",        ImGuiKey_K,      true,  false, false, std::move(cb_command_palette) });
    push({ "Fullscreen Preview","F11",          ImGuiKey_F11,    false, false, false, std::move(cb_fullscreen) });
    push({ "Help",           "F1",             ImGuiKey_F1,     false, false, false, std::move(cb_help) });

    // --- Cue ------------------------------------------------------------------
    push({ "Duplicate Cue",  "Ctrl+D",         ImGuiKey_D,      true,  false, false, std::move(cb_duplicate_cue) });
    push({ "Delete Selected","Delete",         ImGuiKey_Delete, false, false, false, std::move(cb_delete_selected) });
    push({ "Select All",     "Ctrl+A",         ImGuiKey_A,      true,  false, false, std::move(cb_select_all) });

    // --- Cue slots 1–9 (select) -----------------------------------------------
    static const ImGuiKey num_keys[9] = {
        ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4, ImGuiKey_5,
        ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9
    };
    for (int i = 0; i < 9; ++i) {
        int slot = i; // capture by value
        // Without modifier: select cue
        push({ "Select Cue " + std::to_string(i+1),
               std::to_string(i+1),
               num_keys[i], false, false, false,
               [&state, slot]() {
                   if (slot < (int)state.cues.size())
                       state.active_cue_idx = slot;
               }
        });
        // With Ctrl: activate cue
        push({ "Activate Cue " + std::to_string(i+1),
               "Ctrl+" + std::to_string(i+1),
               num_keys[i], true, false, false,
               [&state, slot]() {
                   if (slot < (int)state.cues.size()) {
                       state.active_cue_idx = slot;
                       state.playing        = true;
                   }
               }
        });
    }

    // --- Panel cycle ----------------------------------------------------------
    // Tab is handled specially inside layout to cycle focus
    push({ "Cycle Panels",   "Tab",            ImGuiKey_Tab,    false, false, false,
        []() { /* handled in layout via ImGui SetNextWindowFocus */ }
    });
}

// ─────────────────────────────────────────────────────────────────────────────
//  Per-frame processing
// ─────────────────────────────────────────────────────────────────────────────
void ShortcutRegistry::process() {
    // Don't fire shortcuts when typing in a text widget
    if (ImGui::GetIO().WantTextInput) return;

    for (const auto& s : shortcuts_) {
        if (s.action && is_pressed(s)) {
            s.action();
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Lookup
// ─────────────────────────────────────────────────────────────────────────────
std::string ShortcutRegistry::chord_for(const std::string& name) const {
    for (const auto& s : shortcuts_) {
        if (s.name == name) return s.chord;
    }
    return "";
}

} // namespace idhmfis
