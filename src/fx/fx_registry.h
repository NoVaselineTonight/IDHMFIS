#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include "fx_block.h"

namespace idhmfis {

struct FxBlockMeta {
    std::string name;
    std::string category;
    std::string description;
    std::function<std::unique_ptr<IFxBlock>()> factory;
};

class FxRegistry {
public:
    static FxRegistry& instance();

    // Registers all built-in blocks (existing + new).
    void register_all();

    const std::vector<FxBlockMeta>& all() const;
    std::vector<FxBlockMeta> by_category(const std::string& cat) const;
    std::unique_ptr<IFxBlock> create(const std::string& block_name) const;

private:
    FxRegistry() = default;
    FxRegistry(const FxRegistry&) = delete;
    FxRegistry& operator=(const FxRegistry&) = delete;

    std::vector<FxBlockMeta> entries_;
};

} // namespace idhmfis
