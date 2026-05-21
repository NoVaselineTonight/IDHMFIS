#include "fx_engine.h"
#include <algorithm>
#include <stdexcept>

namespace idhmfis {

void FxEngine::add_block(std::unique_ptr<IFxBlock> block, int at_index) {
    if (!block) return;
    if (at_index < 0 || at_index >= static_cast<int>(blocks_.size())) {
        blocks_.push_back(std::move(block));
    } else {
        blocks_.insert(blocks_.begin() + at_index, std::move(block));
    }
}

void FxEngine::remove_block(int index) {
    if (index < 0 || index >= static_cast<int>(blocks_.size())) return;
    blocks_.erase(blocks_.begin() + index);
}

void FxEngine::move_block(int from_index, int to_index) {
    int n = static_cast<int>(blocks_.size());
    if (from_index < 0 || from_index >= n) return;
    if (to_index   < 0 || to_index   >= n) return;
    if (from_index == to_index) return;

    if (from_index < to_index) {
        std::rotate(blocks_.begin() + from_index,
                     blocks_.begin() + from_index + 1,
                     blocks_.begin() + to_index   + 1);
    } else {
        std::rotate(blocks_.begin() + to_index,
                     blocks_.begin() + from_index,
                     blocks_.begin() + from_index + 1);
    }
}

IFxBlock* FxEngine::block_at(int index) {
    if (index < 0 || index >= static_cast<int>(blocks_.size())) return nullptr;
    return blocks_[static_cast<size_t>(index)].get();
}

int FxEngine::block_count() const {
    return static_cast<int>(blocks_.size());
}

void FxEngine::clear() {
    blocks_.clear();
}

void FxEngine::process(PointBuffer& buf, float dt, const ExprContext& ctx) {
    for (auto& blk : blocks_) {
        if (!blk || !blk->enabled || blk->bypassed) continue;

        const float w = blk->wet;
        if (w <= 0.f) continue;

        if (w >= 1.f) {
            // Full wet — process directly
            blk->process(buf, dt, ctx);
        } else {
            // Wet/dry mix: process a copy, then lerp back
            PointBuffer dry = buf;
            blk->process(buf, dt, ctx);
            size_t sz = std::min(buf.size(), dry.size());
            for (size_t i = 0; i < sz; ++i) {
                auto& wp = buf[i];
                const auto& dp = dry[i];
                float inv = 1.f - w;
                // Restore blanked from dry: a partial wet must not open or close the beam
                wp.blanked = dp.blanked;
                wp.r = static_cast<uint8_t>(w * static_cast<float>(wp.r) + inv * static_cast<float>(dp.r));
                wp.g = static_cast<uint8_t>(w * static_cast<float>(wp.g) + inv * static_cast<float>(dp.g));
                wp.b = static_cast<uint8_t>(w * static_cast<float>(wp.b) + inv * static_cast<float>(dp.b));
                float wx = w * static_cast<float>(wp.x) + inv * static_cast<float>(dp.x);
                float wy = w * static_cast<float>(wp.y) + inv * static_cast<float>(dp.y);
                wp.x = static_cast<int16_t>(std::clamp(wx, -32767.f, 32767.f));
                wp.y = static_cast<int16_t>(std::clamp(wy, -32767.f, 32767.f));
            }
            // Clamp to dry size: discard extra points from expanding blocks (e.g. symmetry)
            // to prevent hallucinated geometry at partial wet levels.
            buf.resize(dry.size());
            for (size_t i = sz; i < dry.size(); ++i)
                buf[i] = dry[i];
        }
    }
}

} // namespace idhmfis
