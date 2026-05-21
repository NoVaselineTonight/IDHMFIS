#pragma once
#include "fx_engine.h"
#include <string>
#include <vector>
#include <unordered_map>

namespace idhmfis {

// One modulator-to-destination connection.
struct ModRoute {
    std::string src_block;   // "" = built-in modulator name
    std::string src_param;   // output param name on source block (or modulator name)
    std::string dst_block;   // target block name
    std::string dst_param;   // target param name
    float       depth   = 1.f;   // -1..+1 polarity and depth
    bool        enabled = true;
};

class ModulationMatrix {
public:
    void add_route(ModRoute r);
    void remove_route(int index);
    void update_route(int index, ModRoute r);
    int  route_count() const;

    const ModRoute& route_at(int i) const;
    ModRoute&       route_at(int i);

    // Apply modulation:
    //  1. Zero all mod_values on all params in engine.
    //  2. For each enabled route, look up source value and add depth * src to dst mod_value.
    // modulator_outputs: map from modulator name string to current float value.
    void apply(FxEngine& engine,
               const std::unordered_map<std::string, float>& modulator_outputs);

private:
    std::vector<ModRoute> routes_;
};

} // namespace idhmfis
