#pragma once
#include "expr_context.h"
#include "fx_param.h"
#include "../core/types.h"
#include <string>
#include <vector>

namespace idhmfis {

class IFxBlock {
public:
    virtual ~IFxBlock() = default;

    virtual const char* name()        const = 0;
    virtual const char* category()    const = 0;  // "Geometry", "Color", "Time", "Modulator"
    virtual const char* description() const = 0;

    bool  enabled  = true;
    bool  bypassed = false;
    float wet      = 1.f;  // 0..1 dry/wet mix

    // Process in place; modifies buf
    virtual void process(PointBuffer& buf, float dt, const ExprContext& ctx) = 0;

    // Parameter access (for UI and modulation matrix)
    virtual std::vector<FxParam*>       params()       = 0;
    virtual const std::vector<FxParam*> params() const = 0;

    FxParam*       param(const std::string& param_name);
    const FxParam* param(const std::string& param_name) const;
};

} // namespace idhmfis
