#pragma once
#include "fx_block.h"
#include <memory>
#include <vector>

namespace idhmfis {

// FxEngine — ordered stack of IFxBlock instances.
// Ownership: FxEngine owns blocks via unique_ptr.
class FxEngine {
public:
    // Append a block (at_index == -1) or insert at a specific position.
    void add_block(std::unique_ptr<IFxBlock> block, int at_index = -1);

    // Remove block at index.
    void remove_block(int index);

    // Swap two blocks.
    void move_block(int from_index, int to_index);

    IFxBlock* block_at(int index);
    int       block_count() const;
    void      clear();

    // Run all enabled, non-bypassed blocks in order with wet/dry mix.
    void process(PointBuffer& buf, float dt, const ExprContext& ctx);

    std::vector<std::unique_ptr<IFxBlock>>& blocks() { return blocks_; }
    const std::vector<std::unique_ptr<IFxBlock>>& blocks() const { return blocks_; }

private:
    std::vector<std::unique_ptr<IFxBlock>> blocks_;
};

} // namespace idhmfis
