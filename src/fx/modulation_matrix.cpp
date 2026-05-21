#include "modulation_matrix.h"
#include <algorithm>
#include <unordered_map>

namespace idhmfis {

void ModulationMatrix::add_route(ModRoute r) {
    routes_.push_back(std::move(r));
}

void ModulationMatrix::remove_route(int index) {
    if (index < 0 || index >= static_cast<int>(routes_.size())) return;
    routes_.erase(routes_.begin() + index);
}

void ModulationMatrix::update_route(int index, ModRoute r) {
    if (index < 0 || index >= static_cast<int>(routes_.size())) return;
    routes_[static_cast<size_t>(index)] = std::move(r);
}

int ModulationMatrix::route_count() const {
    return static_cast<int>(routes_.size());
}

const ModRoute& ModulationMatrix::route_at(int i) const {
    return routes_.at(static_cast<size_t>(i));
}

ModRoute& ModulationMatrix::route_at(int i) {
    return routes_.at(static_cast<size_t>(i));
}

void ModulationMatrix::apply(
    FxEngine& engine,
    const std::unordered_map<std::string, float>& modulator_outputs)
{
    std::unordered_map<std::string, IFxBlock*> block_map;
    block_map.reserve(static_cast<size_t>(engine.block_count()));
    for (int bi = 0; bi < engine.block_count(); ++bi) {
        IFxBlock* blk = engine.block_at(bi);
        if (blk) block_map[blk->name()] = blk;
    }

    // Step 1: zero all mod_values in every block param
    for (int bi = 0; bi < engine.block_count(); ++bi) {
        IFxBlock* blk = engine.block_at(bi);
        if (!blk) continue;
        for (FxParam* p : blk->params()) {
            if (p) p->mod_value = 0.f;
        }
    }

    // Step 2: apply each enabled route
    for (const ModRoute& route : routes_) {
        if (!route.enabled) continue;

        // Resolve source value
        float src_val = 0.f;
        bool src_found = false;

        if (route.src_block.empty()) {
            // Built-in modulator lookup
            auto it = modulator_outputs.find(route.src_param);
            if (it != modulator_outputs.end()) {
                src_val   = it->second;
                src_found = true;
            }
        } else {
            // Source is a param on another block
            {
                auto sit = block_map.find(route.src_block);
                if (sit != block_map.end()) {
                    const FxParam* sp = sit->second->param(route.src_param);
                    if (sp) { src_val = sp->effective(); src_found = true; }
                }
            }
        }

        if (!src_found) continue;

        // Apply to destination param
        {
            auto dit = block_map.find(route.dst_block);
            if (dit != block_map.end()) {
                FxParam* dp = dit->second->param(route.dst_param);
                if (dp) dp->mod_value += route.depth * src_val;
            }
        }
    }
}

} // namespace idhmfis
