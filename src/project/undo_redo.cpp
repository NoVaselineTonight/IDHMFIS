// undo_redo.cpp — Concrete command implementations and UndoStack.

#include "undo_redo.h"
#include <algorithm>
#include <stdexcept>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  UndoStack
// ─────────────────────────────────────────────────────────────────────────────
UndoStack::UndoStack(int max_depth)
    : max_depth_(max_depth > 0 ? max_depth : 1)
{}

void UndoStack::push(std::unique_ptr<ICommand> cmd, Project& p) {
    // Execute first — if this throws, nothing is pushed
    cmd->execute(p);

    // Clear redo history (linear undo semantics)
    redo_stack_.clear();

    undo_stack_.push_back(std::move(cmd));

    // Evict oldest entry if over the limit
    while (static_cast<int>(undo_stack_.size()) > max_depth_)
        undo_stack_.pop_front();
}

void UndoStack::undo(Project& p) {
    if (undo_stack_.empty())
        throw std::runtime_error("Nothing to undo");

    auto cmd = std::move(undo_stack_.back());
    undo_stack_.pop_back();

    cmd->undo(p);
    redo_stack_.push_back(std::move(cmd));
}

void UndoStack::redo(Project& p) {
    if (redo_stack_.empty())
        throw std::runtime_error("Nothing to redo");

    auto cmd = std::move(redo_stack_.back());
    redo_stack_.pop_back();

    cmd->execute(p);
    undo_stack_.push_back(std::move(cmd));
}

std::string UndoStack::undo_description() const {
    if (undo_stack_.empty()) return "";
    return "Undo " + undo_stack_.back()->description();
}

std::string UndoStack::redo_description() const {
    if (redo_stack_.empty()) return "";
    return "Redo " + redo_stack_.back()->description();
}

void UndoStack::clear() {
    undo_stack_.clear();
    redo_stack_.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
//  AddCueCommand
// ─────────────────────────────────────────────────────────────────────────────
AddCueCommand::AddCueCommand(Cue cue)
    : cue_(std::move(cue))
{}

void AddCueCommand::execute(Project& p) {
    // Ensure the cue has a valid id
    if (cue_.id.empty())
        cue_.id = Project::new_id();
    p.cues.push_back(cue_);
}

void AddCueCommand::undo(Project& p) {
    auto it = std::find_if(p.cues.begin(), p.cues.end(),
        [&](const Cue& c) { return c.id == cue_.id; });
    if (it != p.cues.end())
        p.cues.erase(it);
}

std::string AddCueCommand::description() const {
    return "Add Cue \"" + cue_.name + "\"";
}

// ─────────────────────────────────────────────────────────────────────────────
//  RemoveCueCommand
// ─────────────────────────────────────────────────────────────────────────────
RemoveCueCommand::RemoveCueCommand(std::string cue_id)
    : cue_id_(std::move(cue_id))
{}

void RemoveCueCommand::execute(Project& p) {
    int idx = p.find_cue(cue_id_);
    if (idx < 0)
        throw std::runtime_error("RemoveCueCommand: cue not found: " + cue_id_);

    saved_cue_   = p.cues[static_cast<size_t>(idx)];
    saved_index_ = idx;
    p.cues.erase(p.cues.begin() + idx);

    // Remove and save all cue-list entries that reference this cue
    saved_cue_list_entries_.clear();
    for (int i = static_cast<int>(p.cue_list.size()) - 1; i >= 0; --i) {
        if (p.cue_list[static_cast<size_t>(i)].cue_id == cue_id_) {
            saved_cue_list_entries_.emplace_back(i, p.cue_list[static_cast<size_t>(i)]);
            p.cue_list.erase(p.cue_list.begin() + i);
        }
    }
    // Restore in original order
    std::reverse(saved_cue_list_entries_.begin(), saved_cue_list_entries_.end());
}

void RemoveCueCommand::undo(Project& p) {
    // Restore the cue at its original index
    int ins = std::min(saved_index_, static_cast<int>(p.cues.size()));
    p.cues.insert(p.cues.begin() + ins, saved_cue_);

    // Restore cue-list entries
    for (const auto& [idx, entry] : saved_cue_list_entries_) {
        int ins2 = std::min(idx, static_cast<int>(p.cue_list.size()));
        p.cue_list.insert(p.cue_list.begin() + ins2, entry);
    }
}

std::string RemoveCueCommand::description() const {
    return "Remove Cue";
}

// ─────────────────────────────────────────────────────────────────────────────
//  MoveCueCommand
// ─────────────────────────────────────────────────────────────────────────────
MoveCueCommand::MoveCueCommand(int from_index, int to_index)
    : from_(from_index), to_(to_index)
{}

// Rotate-based move so no temporary storage is needed.
static void move_entry(std::vector<CueListEntry>& v, int from, int to) {
    int n = static_cast<int>(v.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;

    if (from < to)
        std::rotate(v.begin() + from, v.begin() + from + 1, v.begin() + to + 1);
    else
        std::rotate(v.begin() + to, v.begin() + from, v.begin() + from + 1);
}

void MoveCueCommand::execute(Project& p) {
    move_entry(p.cue_list, from_, to_);
}

void MoveCueCommand::undo(Project& p) {
    move_entry(p.cue_list, to_, from_);
}

std::string MoveCueCommand::description() const {
    return "Move Cue";
}

// ─────────────────────────────────────────────────────────────────────────────
//  RenameCommand
// ─────────────────────────────────────────────────────────────────────────────
RenameCommand::RenameCommand(std::string cue_id, std::string new_name,
                             const Project& project)
    : cue_id_(std::move(cue_id))
    , new_name_(std::move(new_name))
{
    int idx = project.find_cue(cue_id_);
    if (idx >= 0)
        old_name_ = project.cues[static_cast<size_t>(idx)].name;
}

void RenameCommand::execute(Project& p) {
    int idx = p.find_cue(cue_id_);
    if (idx < 0)
        throw std::runtime_error("RenameCommand: cue not found: " + cue_id_);
    p.cues[static_cast<size_t>(idx)].name = new_name_;
}

void RenameCommand::undo(Project& p) {
    int idx = p.find_cue(cue_id_);
    if (idx < 0)
        throw std::runtime_error("RenameCommand: cue not found on undo: " + cue_id_);
    p.cues[static_cast<size_t>(idx)].name = old_name_;
}

std::string RenameCommand::description() const {
    return "Rename Cue to \"" + new_name_ + "\"";
}

// ─────────────────────────────────────────────────────────────────────────────
//  BatchCommand
// ─────────────────────────────────────────────────────────────────────────────
BatchCommand::BatchCommand(std::string desc)
    : desc_(std::move(desc))
{}

void BatchCommand::add(std::unique_ptr<ICommand> cmd) {
    cmds_.push_back(std::move(cmd));
}

void BatchCommand::execute(Project& p) {
    int executed = 0;
    try {
        for (auto& cmd : cmds_) {
            cmd->execute(p);
            ++executed;
        }
    } catch (...) {
        for (int i = executed - 1; i >= 0; --i)
            cmds_[static_cast<size_t>(i)]->undo(p);
        throw;
    }
}

void BatchCommand::undo(Project& p) {
    // Undo in reverse order
    for (auto it = cmds_.rbegin(); it != cmds_.rend(); ++it)
        (*it)->undo(p);
}

std::string BatchCommand::description() const {
    return desc_;
}

} // namespace idhmfis
