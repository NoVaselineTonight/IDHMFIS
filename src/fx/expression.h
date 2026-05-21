#pragma once
#include "expr_context.h"
#include <string>
#include <vector>
#include <cstdint>

namespace idhmfis {

// Simple register-based bytecode VM for math expressions.
// No heap allocation in the eval path.
//
// Supported variables: t, x, i, beat, bar, bpm, rms, sub, mid, hi, rand
// Supported functions: sin, cos, tan, asin, acos, atan, atan2, exp, log, sqrt,
//   abs, sign, floor, ceil, round, fract, min, max, clamp, mix, smoothstep,
//   step, mod, pow, hypot, pi, tau, e
class ExpressionEngine {
public:
    // Compile expression string to bytecode.
    // Returns false and sets error_msg on failure.
    bool compile(const std::string& expr, std::string& error_msg);

    // Evaluate with given context + per-point x value.
    float eval(const ExprContext& ctx, float x_val = 0.f) const;

    const std::string& source()     const { return source_; }
    const std::string& last_error() const { return last_error_; }
    bool               valid()      const { return valid_; }

    // Populate out with n samples over x = [0, 1)
    void sample_preview(const ExprContext& ctx, int n, std::vector<float>& out) const;

private:
    // ─── Opcode set ───────────────────────────────────────────────────────────
    enum class Op : uint8_t {
        PUSH,       // push imm onto stack
        LOAD_VAR,   // push variable[var_idx] onto stack
        CALL1,      // pop 1 arg, call fn_idx, push result
        CALL2,      // pop 2 args (right first), call fn_idx, push result
        CALL3,      // pop 3 args, call fn_idx, push result
        ADD,
        SUB,
        MUL,
        DIV,
        NEG,
        POW_OP,     // binary power via stack (distinct from CALL2 for pow())
    };

    struct Instr {
        Op      op      = Op::PUSH;
        float   imm     = 0.f;
        uint8_t var_idx = 0;
        uint8_t fn_idx  = 0;
    };

    std::vector<Instr> bytecode_;
    std::string        source_;
    std::string        last_error_;
    bool               valid_ = false;

    // ─── Parser state (used only during compile) ──────────────────────────────
    struct ParseState {
        const char* cur  = nullptr;
        const char* end  = nullptr;
        std::string error;
        bool        ok   = true;
    };

    // Parser helpers
    static void skip_ws(ParseState& ps);
    static float parse_number(ParseState& ps);

    // Recursive descent: expr > term > unary > power > atom
    void parse_expr   (ParseState& ps);
    void parse_term   (ParseState& ps);
    void parse_unary  (ParseState& ps);
    void parse_power  (ParseState& ps);
    void parse_atom   (ParseState& ps);

    // Try to parse a function call starting at ps->cur
    // Returns true if a known function was found and consumed
    bool parse_function_call(ParseState& ps, const char* ident, int ident_len);

    // Emit helpers
    void emit(Instr instr) { bytecode_.push_back(instr); }
    void emit_push(float v)           { emit({ Op::PUSH,    v,  0, 0 }); }
    void emit_load(uint8_t var_idx)   { emit({ Op::LOAD_VAR, 0.f, var_idx, 0 }); }
    void emit_call1(uint8_t fn_idx)   { emit({ Op::CALL1,   0.f, 0, fn_idx }); }
    void emit_call2(uint8_t fn_idx)   { emit({ Op::CALL2,   0.f, 0, fn_idx }); }
    void emit_call3(uint8_t fn_idx)   { emit({ Op::CALL3,   0.f, 0, fn_idx }); }
    void emit_op(Op op)               { emit({ op, 0.f, 0, 0 }); }

    // Variable indices
    static constexpr uint8_t VAR_T    = 0;
    static constexpr uint8_t VAR_X    = 1;
    static constexpr uint8_t VAR_I    = 2;
    static constexpr uint8_t VAR_BEAT = 3;
    static constexpr uint8_t VAR_BAR  = 4;
    static constexpr uint8_t VAR_BPM  = 5;
    static constexpr uint8_t VAR_RMS  = 6;
    static constexpr uint8_t VAR_SUB  = 7;
    static constexpr uint8_t VAR_MID  = 8;
    static constexpr uint8_t VAR_HI   = 9;
    static constexpr uint8_t VAR_RAND = 10;

    // Function indices for CALL1
    static constexpr uint8_t FN1_SIN        = 0;
    static constexpr uint8_t FN1_COS        = 1;
    static constexpr uint8_t FN1_TAN        = 2;
    static constexpr uint8_t FN1_ASIN       = 3;
    static constexpr uint8_t FN1_ACOS       = 4;
    static constexpr uint8_t FN1_ATAN       = 5;
    static constexpr uint8_t FN1_EXP        = 6;
    static constexpr uint8_t FN1_LOG        = 7;
    static constexpr uint8_t FN1_SQRT       = 8;
    static constexpr uint8_t FN1_ABS        = 9;
    static constexpr uint8_t FN1_SIGN       = 10;
    static constexpr uint8_t FN1_FLOOR      = 11;
    static constexpr uint8_t FN1_CEIL       = 12;
    static constexpr uint8_t FN1_ROUND      = 13;
    static constexpr uint8_t FN1_FRACT      = 14;

    // Function indices for CALL2
    static constexpr uint8_t FN2_ATAN2      = 0;
    static constexpr uint8_t FN2_MIN        = 1;
    static constexpr uint8_t FN2_MAX        = 2;
    static constexpr uint8_t FN2_MOD        = 3;
    static constexpr uint8_t FN2_POW        = 4;
    static constexpr uint8_t FN2_HYPOT      = 5;
    static constexpr uint8_t FN2_STEP       = 6;

    // Function indices for CALL3
    static constexpr uint8_t FN3_CLAMP      = 0;
    static constexpr uint8_t FN3_MIX        = 1;
    static constexpr uint8_t FN3_SMOOTHSTEP = 2;
};

} // namespace idhmfis
