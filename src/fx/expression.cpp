#include "expression.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Compile
// ─────────────────────────────────────────────────────────────────────────────

bool ExpressionEngine::compile(const std::string& expr, std::string& error_msg) {
    valid_      = false;
    last_error_ = "";
    bytecode_.clear();
    source_ = expr;

    if (expr.empty()) {
        last_error_ = error_msg = "Empty expression";
        return false;
    }

    ParseState ps;
    ps.cur = expr.c_str();
    ps.end = expr.c_str() + expr.size();
    ps.ok  = true;

    parse_expr(ps);

    if (!ps.ok) {
        last_error_ = error_msg = ps.error;
        return false;
    }

    // Ensure we consumed everything (ignoring trailing whitespace)
    skip_ws(ps);
    if (ps.cur < ps.end) {
        last_error_ = error_msg = std::string("Unexpected token: ") + std::string(ps.cur, ps.end);
        bytecode_.clear();
        return false;
    }

    valid_ = true;
    error_msg = "";
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parser helpers
// ─────────────────────────────────────────────────────────────────────────────

void ExpressionEngine::skip_ws(ParseState& ps) {
    while (ps.cur < ps.end && std::isspace(static_cast<unsigned char>(*ps.cur)))
        ++ps.cur;
}

float ExpressionEngine::parse_number(ParseState& ps) {
    skip_ws(ps);
    char* end_ptr = nullptr;
    float val = std::strtof(ps.cur, &end_ptr);
    if (end_ptr == ps.cur) {
        ps.ok = false;
        ps.error = "Expected number";
        return 0.f;
    }
    ps.cur = end_ptr;
    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Recursive descent:
//  expr   -> term  { ('+' | '-') term }
//  term   -> unary { ('*' | '/') unary }
//  unary  -> '-' power | power
//  power  -> atom [ '^' unary ]
//  atom   -> '(' expr ')' | number | ident [ '(' args ')' ]
// ─────────────────────────────────────────────────────────────────────────────

void ExpressionEngine::parse_expr(ParseState& ps) {
    if (!ps.ok) return;
    parse_term(ps);
    if (!ps.ok) return;

    for (;;) {
        skip_ws(ps);
        if (ps.cur >= ps.end) break;
        char ch = *ps.cur;
        if (ch != '+' && ch != '-') break;
        ++ps.cur;
        parse_term(ps);
        if (!ps.ok) return;
        emit_op(ch == '+' ? Op::ADD : Op::SUB);
    }
}

void ExpressionEngine::parse_term(ParseState& ps) {
    if (!ps.ok) return;
    parse_unary(ps);
    if (!ps.ok) return;

    for (;;) {
        skip_ws(ps);
        if (ps.cur >= ps.end) break;
        char ch = *ps.cur;
        if (ch != '*' && ch != '/') break;
        ++ps.cur;
        parse_unary(ps);
        if (!ps.ok) return;
        emit_op(ch == '*' ? Op::MUL : Op::DIV);
    }
}

void ExpressionEngine::parse_unary(ParseState& ps) {
    if (!ps.ok) return;
    skip_ws(ps);
    if (ps.cur < ps.end && *ps.cur == '-') {
        ++ps.cur;
        parse_power(ps);
        if (!ps.ok) return;
        emit_op(Op::NEG);
    } else {
        parse_power(ps);
    }
}

void ExpressionEngine::parse_power(ParseState& ps) {
    if (!ps.ok) return;
    parse_atom(ps);
    if (!ps.ok) return;

    skip_ws(ps);
    if (ps.cur < ps.end && *ps.cur == '^') {
        ++ps.cur;
        parse_unary(ps);  // right-associative
        if (!ps.ok) return;
        emit_op(Op::POW_OP);
    }
}

void ExpressionEngine::parse_atom(ParseState& ps) {
    if (!ps.ok) return;
    skip_ws(ps);

    if (ps.cur >= ps.end) {
        ps.ok = false;
        ps.error = "Unexpected end of expression";
        return;
    }

    char ch = *ps.cur;

    // Parenthesised sub-expression
    if (ch == '(') {
        ++ps.cur;
        parse_expr(ps);
        if (!ps.ok) return;
        skip_ws(ps);
        if (ps.cur >= ps.end || *ps.cur != ')') {
            ps.ok = false;
            ps.error = "Expected ')'";
            return;
        }
        ++ps.cur;
        return;
    }

    // Numeric literal
    if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
        emit_push(parse_number(ps));
        return;
    }

    // Identifier: variable or function
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
        // Read identifier
        const char* id_start = ps.cur;
        while (ps.cur < ps.end &&
               (std::isalnum(static_cast<unsigned char>(*ps.cur)) || *ps.cur == '_'))
            ++ps.cur;
        int id_len = static_cast<int>(ps.cur - id_start);

        // Check if it's a function call
        if (parse_function_call(ps, id_start, id_len)) return;

        // Constants — compare by exact id_len + content
        auto id_eq_const = [&](const char* kw) -> bool {
            int klen = static_cast<int>(std::strlen(kw));
            return id_len == klen && std::strncmp(id_start, kw, static_cast<size_t>(klen)) == 0;
        };
        if (id_eq_const("pi")) {
            emit_push(static_cast<float>(M_PI));
            return;
        }
        if (id_eq_const("tau")) {
            emit_push(static_cast<float>(M_PI * 2.0));
            return;
        }
        if (id_eq_const("e")) {
            emit_push(static_cast<float>(2.718281828459045));
            return;
        }

        // Variables (check by exact match against id_start, length id_len)
        struct VarEntry { const char* name; uint8_t idx; };
        static constexpr VarEntry kVars[] = {
            { "t",    VAR_T    },
            { "x",    VAR_X    },
            { "i",    VAR_I    },
            { "beat", VAR_BEAT },
            { "bar",  VAR_BAR  },
            { "bpm",  VAR_BPM  },
            { "rms",  VAR_RMS  },
            { "sub",  VAR_SUB  },
            { "mid",  VAR_MID  },
            { "hi",   VAR_HI   },
            { "rand", VAR_RAND },
        };
        for (const auto& v : kVars) {
            int vlen = static_cast<int>(std::strlen(v.name));
            if (id_len == vlen && std::strncmp(id_start, v.name, static_cast<size_t>(vlen)) == 0) {
                emit_load(v.idx);
                return;
            }
        }

        ps.ok = false;
        ps.error = std::string("Unknown identifier: ") + std::string(id_start, static_cast<size_t>(id_len));
        return;
    }

    ps.ok = false;
    ps.error = std::string("Unexpected character: ") + ch;
}

bool ExpressionEngine::parse_function_call(ParseState& ps, const char* ident, int ident_len) {
    // Only treat as function call if the next non-space char is '('
    const char* saved_cur = ps.cur;
    skip_ws(ps);
    if (ps.cur >= ps.end || *ps.cur != '(') {
        ps.cur = saved_cur;
        return false;
    }

    // Map function name to (arity, fn_idx)
    struct FnEntry { const char* name; int arity; uint8_t idx; };
    static const FnEntry kFns[] = {
        // 1-arg
        { "sin",        1, FN1_SIN        },
        { "cos",        1, FN1_COS        },
        { "tan",        1, FN1_TAN        },
        { "asin",       1, FN1_ASIN       },
        { "acos",       1, FN1_ACOS       },
        { "atan",       1, FN1_ATAN       },
        { "exp",        1, FN1_EXP        },
        { "log",        1, FN1_LOG        },
        { "sqrt",       1, FN1_SQRT       },
        { "abs",        1, FN1_ABS        },
        { "sign",       1, FN1_SIGN       },
        { "floor",      1, FN1_FLOOR      },
        { "ceil",       1, FN1_CEIL       },
        { "round",      1, FN1_ROUND      },
        { "fract",      1, FN1_FRACT      },
        // 2-arg
        { "atan2",      2, FN2_ATAN2      },
        { "min",        2, FN2_MIN        },
        { "max",        2, FN2_MAX        },
        { "mod",        2, FN2_MOD        },
        { "pow",        2, FN2_POW        },
        { "hypot",      2, FN2_HYPOT      },
        { "step",       2, FN2_STEP       },
        // 3-arg
        { "clamp",      3, FN3_CLAMP      },
        { "mix",        3, FN3_MIX        },
        { "smoothstep", 3, FN3_SMOOTHSTEP },
    };

    const FnEntry* found = nullptr;
    for (const auto& fn : kFns) {
        int klen = static_cast<int>(std::strlen(fn.name));
        if (ident_len == klen && std::strncmp(ident, fn.name, static_cast<size_t>(klen)) == 0) {
            found = &fn;
            break;
        }
    }

    if (!found) {
        ps.cur = saved_cur;
        return false;
    }

    // Consume '('
    ++ps.cur;

    // Parse arguments
    for (int a = 0; a < found->arity; ++a) {
        if (a > 0) {
            skip_ws(ps);
            if (ps.cur >= ps.end || *ps.cur != ',') {
                ps.ok = false;
                ps.error = std::string("Expected ',' in call to ") + found->name;
                return true;
            }
            ++ps.cur;
        }
        parse_expr(ps);
        if (!ps.ok) return true;
    }

    skip_ws(ps);
    if (ps.cur >= ps.end || *ps.cur != ')') {
        ps.ok = false;
        ps.error = std::string("Expected ')' after call to ") + found->name;
        return true;
    }
    ++ps.cur;

    if (found->arity == 1) emit_call1(found->idx);
    else if (found->arity == 2) emit_call2(found->idx);
    else emit_call3(found->idx);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Eval
// ─────────────────────────────────────────────────────────────────────────────

float ExpressionEngine::eval(const ExprContext& ctx, float x_val) const {
    if (!valid_ || bytecode_.empty()) return 0.f;

    // Fixed-size eval stack (max expression depth ~64)
    float stack[64];
    int   top = 0;

    auto push = [&](float v) {
        if (top < 64) stack[top++] = v;
    };
    auto pop = [&]() -> float {
        if (top > 0) return stack[--top];
        return 0.f;
    };

    for (const Instr& instr : bytecode_) {
        switch (instr.op) {
        case Op::PUSH:
            push(instr.imm);
            break;

        case Op::LOAD_VAR: {
            float val = 0.f;
            switch (instr.var_idx) {
            case VAR_T:    val = static_cast<float>(ctx.t);    break;
            case VAR_X:    val = x_val;                         break;
            case VAR_I:    val = static_cast<float>(ctx.sample_i); break;
            case VAR_BEAT: val = static_cast<float>(ctx.beat); break;
            case VAR_BAR:  val = static_cast<float>(ctx.bar);  break;
            case VAR_BPM:  val = ctx.bpm;                       break;
            case VAR_RMS:  val = ctx.rms;                       break;
            case VAR_SUB:  val = ctx.sub;                       break;
            case VAR_MID:  val = ctx.mid;                       break;
            case VAR_HI:   val = ctx.hi;                        break;
            case VAR_RAND: val = static_cast<float>(std::rand()) / static_cast<float>(RAND_MAX); break;
            default:       val = 0.f;                           break;
            }
            push(val);
            break;
        }

        case Op::CALL1: {
            float a = pop();
            float r = 0.f;
            switch (instr.fn_idx) {
            case FN1_SIN:   r = std::sin(a);   break;
            case FN1_COS:   r = std::cos(a);   break;
            case FN1_TAN:   r = std::tan(a);   break;
            case FN1_ASIN:  r = std::asin(std::clamp(a, -1.f, 1.f)); break;
            case FN1_ACOS:  r = std::acos(std::clamp(a, -1.f, 1.f)); break;
            case FN1_ATAN:  r = std::atan(a);  break;
            case FN1_EXP:   r = std::exp(a);   break;
            case FN1_LOG:   r = std::log(std::fabs(a) < 1e-30f ? 1e-30f : std::fabs(a)); break;
            case FN1_SQRT:  r = std::sqrt(a < 0.f ? 0.f : a); break;
            case FN1_ABS:   r = std::fabs(a);  break;
            case FN1_SIGN:  r = (a > 0.f) ? 1.f : (a < 0.f) ? -1.f : 0.f; break;
            case FN1_FLOOR: r = std::floor(a); break;
            case FN1_CEIL:  r = std::ceil(a);  break;
            case FN1_ROUND: r = std::round(a); break;
            case FN1_FRACT: r = a - std::floor(a); break;
            default:        r = a; break;
            }
            push(r);
            break;
        }

        case Op::CALL2: {
            // Second arg pushed last, so pop b then a
            float b = pop();
            float a = pop();
            float r = 0.f;
            switch (instr.fn_idx) {
            case FN2_ATAN2: r = std::atan2(a, b); break;
            case FN2_MIN:   r = (a < b) ? a : b;  break;
            case FN2_MAX:   r = (a > b) ? a : b;  break;
            case FN2_MOD:   r = (b != 0.f) ? std::fmod(a, b) : 0.f; break;
            case FN2_POW:   r = std::pow(std::fabs(a), b); break;
            case FN2_HYPOT: r = std::hypot(a, b); break;
            case FN2_STEP:  r = (b < a) ? 0.f : 1.f; break;  // step(edge, x)
            default:        r = a; break;
            }
            push(r);
            break;
        }

        case Op::CALL3: {
            float c = pop();
            float b = pop();
            float a = pop();
            float r = 0.f;
            switch (instr.fn_idx) {
            case FN3_CLAMP:
                r = std::clamp(a, b, c);
                break;
            case FN3_MIX:
                r = a + (b - a) * c;  // mix(a, b, t)
                break;
            case FN3_SMOOTHSTEP: {
                // smoothstep(edge0, edge1, x)
                float t = std::clamp((c - a) / (b - a + 1e-30f), 0.f, 1.f);
                r = t * t * (3.f - 2.f * t);
                break;
            }
            default: r = a; break;
            }
            push(r);
            break;
        }

        case Op::ADD: { float b = pop(); float a = pop(); push(a + b); break; }
        case Op::SUB: { float b = pop(); float a = pop(); push(a - b); break; }
        case Op::MUL: { float b = pop(); float a = pop(); push(a * b); break; }
        case Op::DIV: {
            float b = pop(); float a = pop();
            push((b != 0.f) ? a / b : 0.f);
            break;
        }
        case Op::NEG:    { float a = pop(); push(-a); break; }
        case Op::POW_OP: { float b = pop(); float a = pop(); push(std::pow(a, b)); break; }
        default: break;
        }
    }

    return (top > 0) ? stack[top - 1] : 0.f;
}

void ExpressionEngine::sample_preview(const ExprContext& ctx, int n,
                                       std::vector<float>& out) const {
    out.resize(static_cast<size_t>(n));
    if (n <= 0) return;
    ExprContext local_ctx = ctx;
    for (int i = 0; i < n; ++i) {
        float x = static_cast<float>(i) / static_cast<float>(n);
        local_ctx.sample_i = i;
        local_ctx.sample_n = n;
        out[static_cast<size_t>(i)] = eval(local_ctx, x);
    }
}

} // namespace idhmfis
