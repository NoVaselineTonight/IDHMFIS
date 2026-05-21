#pragma once
// undo_redo.h — Command pattern undo/redo system.
//
// All UI actions that modify the Project must go through the UndoStack.
// Calling push() executes the command immediately and places it on the
// undo stack. Undo/redo are O(1) deque operations.
//
// Design notes:
//   - ICommand is a pure-virtual interface; concrete commands own all state
//     needed to perform and reverse the action.
//   - BatchCommand wraps multiple commands into a single undo step.
//   - ModifyParamCommand is a template that handles any copyable field.
//   - The redo stack is cleared whenever a new command is pushed (standard
//     linear-history behaviour).
//   - max_depth limits memory; when the undo stack is full, the oldest entry
//     is dropped (similar to vim's undolevels).

#include <deque>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>
#include <functional>

#include "../core/types.h"
#include "project.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Command interface
// ─────────────────────────────────────────────────────────────────────────────
struct ICommand {
    virtual ~ICommand() = default;

    // Execute the command, modifying the project.
    virtual void execute(Project& p) = 0;

    // Reverse the effect of execute().
    virtual void undo(Project& p) = 0;

    // Short human-readable description shown in Edit menu.
    virtual std::string description() const = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
//  UndoStack
// ─────────────────────────────────────────────────────────────────────────────
class UndoStack {
public:
    explicit UndoStack(int max_depth = kUndoBufferDepth);

    // Execute cmd and push it onto the undo stack.
    // The redo stack is cleared.
    void push(std::unique_ptr<ICommand> cmd, Project& p);

    bool can_undo() const { return !undo_stack_.empty(); }
    bool can_redo() const { return !redo_stack_.empty(); }

    void undo(Project& p);
    void redo(Project& p);

    // Description of the top action for menu labelling ("Undo Add Cue", etc.)
    std::string undo_description() const;
    std::string redo_description() const;

    // Discard all history (e.g. after a "Save As" to a new project).
    void clear();

    // Number of operations currently on each stack.
    int undo_depth() const { return static_cast<int>(undo_stack_.size()); }
    int redo_depth() const { return static_cast<int>(redo_stack_.size()); }

private:
    std::deque<std::unique_ptr<ICommand>> undo_stack_;
    std::deque<std::unique_ptr<ICommand>> redo_stack_;
    int max_depth_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  AddCueCommand — insert a new cue at the end of Project::cues
// ─────────────────────────────────────────────────────────────────────────────
class AddCueCommand : public ICommand {
public:
    explicit AddCueCommand(Cue cue);

    void execute(Project& p) override;
    void undo(Project& p) override;
    std::string description() const override;

private:
    Cue cue_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  RemoveCueCommand — remove the cue with the given id
// ─────────────────────────────────────────────────────────────────────────────
class RemoveCueCommand : public ICommand {
public:
    explicit RemoveCueCommand(std::string cue_id);

    void execute(Project& p) override;
    void undo(Project& p) override;
    std::string description() const override;

private:
    std::string cue_id_;

    // Saved state for undo
    Cue    saved_cue_;
    int    saved_index_ = -1;

    // Saved cue-list entries that referenced this cue (restored on undo)
    std::vector<std::pair<int, CueListEntry>> saved_cue_list_entries_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  MoveCueCommand — reorder a cue in the cue list by index
// ─────────────────────────────────────────────────────────────────────────────
class MoveCueCommand : public ICommand {
public:
    // Moves the cue_list entry at from_index to to_index.
    MoveCueCommand(int from_index, int to_index);

    void execute(Project& p) override;
    void undo(Project& p) override;
    std::string description() const override;

private:
    int from_;
    int to_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  ModifyParamCommand<T> — generic parameter change
//  T must be copyable.  A setter function is used so this works for any
//  field reachable from Project without needing compile-time knowledge of
//  the full type hierarchy.
// ─────────────────────────────────────────────────────────────────────────────
template <typename T>
class ModifyParamCommand : public ICommand {
public:
    using Setter = std::function<void(Project&, const T&)>;
    using Getter = std::function<T(const Project&)>;

    ModifyParamCommand(std::string desc,
                       Getter getter,
                       Setter setter,
                       T new_value,
                       const Project& project)
        : desc_(std::move(desc))
        , getter_(std::move(getter))
        , setter_(std::move(setter))
        , old_value_(getter_(project))
        , new_value_(std::move(new_value))
    {}

    void execute(Project& p) override { setter_(p, new_value_); }
    void undo(Project& p)    override { setter_(p, old_value_); }
    std::string description() const override { return desc_; }

private:
    std::string desc_;
    Getter      getter_;
    Setter      setter_;
    T           old_value_;
    T           new_value_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  RenameCommand — rename a cue by id
// ─────────────────────────────────────────────────────────────────────────────
class RenameCommand : public ICommand {
public:
    RenameCommand(std::string cue_id, std::string new_name,
                  const Project& project);

    void execute(Project& p) override;
    void undo(Project& p) override;
    std::string description() const override;

private:
    std::string cue_id_;
    std::string old_name_;
    std::string new_name_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  BatchCommand — group multiple commands as one undo step
// ─────────────────────────────────────────────────────────────────────────────
class BatchCommand : public ICommand {
public:
    explicit BatchCommand(std::string desc);

    // Add a sub-command. Must be called before push()ing this batch.
    void add(std::unique_ptr<ICommand> cmd);

    void execute(Project& p) override;
    void undo(Project& p) override;
    std::string description() const override;

private:
    std::string                          desc_;
    std::vector<std::unique_ptr<ICommand>> cmds_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Factory helpers — convenience functions for the most common operations
// ─────────────────────────────────────────────────────────────────────────────

// Modify a float field inside the GeneratorParams of a cue.
// Example:
//   auto cmd = make_cue_param_cmd(
//       project, cue_id, "speed",
//       [](const GeneratorParams& p) { return p.speed; },
//       [](GeneratorParams& p, float v) { p.speed = v; },
//       new_speed);
template <typename FieldType, typename GetF, typename SetF>
std::unique_ptr<ICommand> make_cue_param_cmd(
        const Project& project,
        const std::string& cue_id,
        const std::string& field_desc,
        GetF get_field,
        SetF set_field,
        FieldType new_val)
{
    auto getter = [cue_id, get_field](const Project& p) -> FieldType {
        int idx = p.find_cue(cue_id);
        if (idx < 0) throw std::runtime_error("Cue not found: " + cue_id);
        return get_field(p.cues[idx].params);
    };
    auto setter = [cue_id, set_field](Project& p, const FieldType& v) {
        int idx = p.find_cue(cue_id);
        if (idx < 0) throw std::runtime_error("Cue not found: " + cue_id);
        set_field(p.cues[idx].params, v);
    };

    return std::make_unique<ModifyParamCommand<FieldType>>(
        "Change " + field_desc,
        std::move(getter),
        std::move(setter),
        std::move(new_val),
        project
    );
}

} // namespace idhmfis
