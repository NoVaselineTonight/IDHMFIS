#include "fx_block.h"

namespace idhmfis {

FxParam* IFxBlock::param(const std::string& param_name) {
    for (FxParam* p : params()) {
        if (p && p->name == param_name) return p;
    }
    return nullptr;
}

const FxParam* IFxBlock::param(const std::string& param_name) const {
    for (const FxParam* p : params()) {
        if (p && p->name == param_name) return p;
    }
    return nullptr;
}

} // namespace idhmfis
