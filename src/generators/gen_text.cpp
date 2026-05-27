// gen_text.cpp — Text scroller using embedded Hershey Simplex vector font.
//
// The Hershey simplex font (Roman simplex, occidental) is encoded as a table
// of strokes per glyph. Each glyph entry is a compact sequence of (x, y) pairs
// where (127, 127) marks a pen-up move.
//
// param_a: scroll speed (0..1 → slow..fast)
// text is read from a global string; in production this would come from cue metadata.

#include "igenerator.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Hershey Simplex font data (ASCII 32..126)
//  Format per glyph: { width, { (x,y), ... } }
//  x=127,y=127 → pen-up. Coordinates in units of 1/21 of character height.
//  These are the standard Hershey font stroke data (public domain).
// ─────────────────────────────────────────────────────────────────────────────

struct HersheyStroke { int8_t x, y; }; // 127=penup sentinel

struct HersheyGlyph {
    int8_t              width;   // character advance width in Hershey units
    int                 nstrokes;
    const HersheyStroke* strokes;
};

// We embed a subset of Hershey Roman Simplex here.
// Full 95-character set (space through tilde).
// Coordinate origin at left baseline; y positive = up.

// Stroke tables — one entry per ASCII character 32..126
// Format: width, then stroke pairs terminated by (127,127)

static const HersheyStroke s_space[]  = {{127,127}};
static const HersheyStroke s_excl[]   = {{0,21},{0,7},{127,127},{0,2},{0,0},{127,127}};
static const HersheyStroke s_quote[]  = {{-1,21},{1,14},{127,127},{4,21},{6,14},{127,127}};
static const HersheyStroke s_hash[]   = {{-3,12},{17,12},{127,127},{-3,6},{17,6},{127,127},{5,20},{3,-2},{127,127},{9,20},{7,-2},{127,127}};
static const HersheyStroke s_dollar[] = {{8,21},{8,-2},{127,127},{4,18},{6,20},{10,20},{12,18},{12,16},{10,14},{4,12},{2,10},{2,8},{4,6},{8,4},{10,4},{12,6},{127,127}};
static const HersheyStroke s_pct[]    = {{3,20},{19,2},{127,127},{6,20},{8,18},{8,16},{6,14},{4,16},{4,18},{6,20},{127,127},{14,8},{16,6},{16,4},{14,2},{12,4},{12,6},{14,8},{127,127}};
static const HersheyStroke s_amp[]    = {{18,12},{10,20},{8,16},{8,12},{10,8},{16,4},{18,4},{20,8},{20,12},{18,16},{16,20},{12,20},{10,18},{10,16},{12,14},{18,10},{127,127}};
static const HersheyStroke s_apos[]   = {{0,21},{-2,13},{127,127}};
static const HersheyStroke s_lparen[] = {{5,20},{3,18},{1,14},{1,8},{3,4},{5,2},{127,127}};
static const HersheyStroke s_rparen[] = {{0,20},{2,18},{4,14},{4,8},{2,4},{0,2},{127,127}};
static const HersheyStroke s_star[]   = {{0,14},{12,0},{127,127},{12,14},{0,0},{127,127},{6,17},{6,-3},{127,127}};
static const HersheyStroke s_plus[]   = {{1,18},{1,0},{127,127},{-4,9},{6,9},{127,127}};
static const HersheyStroke s_comma[]  = {{1,2},{-1,-2},{127,127}};
static const HersheyStroke s_minus[]  = {{-4,9},{6,9},{127,127}};
static const HersheyStroke s_period[] = {{0,2},{0,0},{127,127}};
static const HersheyStroke s_slash[]  = {{14,20},{-2,0},{127,127}};

static const HersheyStroke s_0[] = {{9,21},{6,20},{4,17},{3,14},{3,9},{4,6},{6,3},{9,2},{11,2},{14,3},{16,6},{17,9},{17,14},{16,17},{14,20},{11,21},{9,21},{127,127}};
static const HersheyStroke s_1[] = {{6,17},{8,18},{11,21},{11,0},{127,127}};
static const HersheyStroke s_2[] = {{4,18},{6,20},{10,21},{12,21},{16,20},{18,18},{18,15},{16,12},{10,7},{3,0},{19,0},{127,127}};
static const HersheyStroke s_3[] = {{5,21},{16,21},{10,13},{13,13},{16,12},{18,10},{18,7},{16,4},{13,2},{9,1},{6,1},{3,2},{1,4},{127,127}};
static const HersheyStroke s_4[] = {{13,21},{3,7},{18,7},{127,127},{13,21},{13,0},{127,127}};
static const HersheyStroke s_5[] = {{15,21},{5,21},{4,12},{6,14},{9,15},{12,15},{15,14},{17,12},{18,9},{18,7},{17,4},{15,2},{12,1},{8,1},{5,2},{3,4},{127,127}};
static const HersheyStroke s_6[] = {{16,18},{13,20},{9,21},{7,21},{4,20},{2,17},{1,14},{1,10},{2,6},{4,3},{7,1},{10,1},{13,2},{15,4},{16,7},{16,8},{15,10},{13,11},{10,11},{7,9},{5,6},{4,3},{127,127}};
static const HersheyStroke s_7[] = {{17,21},{7,0},{127,127},{3,21},{17,21},{127,127}};
static const HersheyStroke s_8[] = {{8,21},{5,20},{3,17},{3,14},{5,11},{8,10},{11,10},{14,11},{16,14},{16,17},{14,20},{11,21},{8,21},{127,127},{8,10},{5,9},{3,6},{3,3},{5,1},{8,0},{11,0},{14,1},{16,3},{16,6},{14,9},{11,10},{127,127}};
static const HersheyStroke s_9[] = {{16,14},{15,11},{13,9},{10,8},{9,8},{6,9},{4,11},{3,14},{3,15},{4,18},{6,20},{9,21},{11,21},{14,20},{16,18},{17,15},{17,11},{16,7},{14,4},{12,2},{9,1},{7,1},{4,2},{3,4},{127,127}};

static const HersheyStroke s_colon[]  = {{0,13},{0,11},{127,127},{0,2},{0,0},{127,127}};
static const HersheyStroke s_semi[]   = {{0,13},{0,11},{127,127},{0,2},{-1,-2},{127,127}};
static const HersheyStroke s_lt[]     = {{14,18},{2,9},{14,0},{127,127}};
static const HersheyStroke s_eq[]     = {{-4,12},{16,12},{127,127},{-4,6},{16,6},{127,127}};
static const HersheyStroke s_gt[]     = {{2,18},{14,9},{2,0},{127,127}};
static const HersheyStroke s_quest[]  = {{3,16},{3,18},{6,21},{9,21},{12,20},{13,18},{13,16},{12,14},{9,12},{9,9},{127,127},{9,2},{9,0},{127,127}};
static const HersheyStroke s_at[]     = {{13,13},{12,15},{10,17},{7,18},{5,16},{4,13},{4,9},{5,6},{7,4},{10,4},{13,5},{14,8},{13,13},{127,127},{18,18},{10,18},{7,16},{5,14},{4,11},{4,8},{5,5},{7,3},{10,1},{13,0},{17,0},{127,127}};

// Uppercase letters
static const HersheyStroke sA[] = {{9,21},{1,0},{127,127},{9,21},{17,0},{127,127},{4,7},{14,7},{127,127}};
static const HersheyStroke sB[] = {{4,21},{4,0},{127,127},{4,21},{13,21},{16,20},{17,19},{18,17},{18,15},{17,13},{16,12},{13,11},{127,127},{4,11},{13,11},{16,10},{17,9},{18,7},{18,4},{17,2},{16,1},{13,0},{4,0},{127,127}};
static const HersheyStroke sC[] = {{18,16},{17,18},{15,20},{13,21},{9,21},{7,20},{5,18},{4,16},{3,13},{3,8},{4,5},{6,3},{9,1},{11,0},{13,1},{15,3},{16,5},{127,127}};
static const HersheyStroke sD[] = {{4,21},{4,0},{127,127},{4,21},{11,21},{14,20},{16,18},{17,16},{18,13},{18,8},{17,5},{16,3},{14,1},{11,0},{4,0},{127,127}};
static const HersheyStroke sE[] = {{4,21},{4,0},{127,127},{4,21},{17,21},{127,127},{4,11},{12,11},{127,127},{4,0},{17,0},{127,127}};
static const HersheyStroke sF[] = {{4,21},{4,0},{127,127},{4,21},{17,21},{127,127},{4,11},{12,11},{127,127}};
static const HersheyStroke sG[] = {{18,16},{17,18},{15,20},{13,21},{9,21},{7,20},{5,18},{4,16},{3,13},{3,8},{4,5},{6,3},{9,1},{11,0},{13,0},{15,1},{16,3},{16,8},{127,127},{11,8},{16,8},{127,127}};
static const HersheyStroke sH[] = {{4,21},{4,0},{127,127},{18,21},{18,0},{127,127},{4,11},{18,11},{127,127}};
static const HersheyStroke sI[] = {{4,21},{11,21},{127,127},{4,0},{11,0},{127,127},{7,21},{7,0},{127,127}};
static const HersheyStroke sJ[] = {{12,21},{12,5},{10,2},{8,1},{5,1},{3,2},{1,5},{1,7},{127,127}};
static const HersheyStroke sK[] = {{4,21},{4,0},{127,127},{18,21},{4,7},{127,127},{9,12},{18,0},{127,127}};
static const HersheyStroke sL[] = {{4,21},{4,0},{127,127},{4,0},{16,0},{127,127}};
static const HersheyStroke sM[] = {{4,21},{4,0},{127,127},{4,21},{12,0},{127,127},{20,21},{12,0},{127,127},{20,21},{20,0},{127,127}};
static const HersheyStroke sN[] = {{4,21},{4,0},{127,127},{4,21},{18,0},{127,127},{18,21},{18,0},{127,127}};
static const HersheyStroke sO[] = {{9,21},{7,20},{5,18},{4,16},{3,13},{3,8},{4,5},{6,3},{9,1},{11,0},{13,0},{16,1},{18,3},{19,5},{20,8},{20,13},{19,16},{18,18},{16,20},{13,21},{9,21},{127,127}};
static const HersheyStroke sP[] = {{4,21},{4,0},{127,127},{4,21},{13,21},{16,20},{17,19},{18,17},{18,14},{17,12},{16,11},{13,10},{4,10},{127,127}};
static const HersheyStroke sQ[] = {{9,21},{7,20},{5,18},{4,16},{3,13},{3,8},{4,5},{6,3},{9,1},{11,0},{13,0},{16,1},{18,3},{19,5},{20,8},{20,13},{19,16},{18,18},{16,20},{13,21},{9,21},{127,127},{13,5},{19,-1},{127,127}};
static const HersheyStroke sR[] = {{4,21},{4,0},{127,127},{4,21},{13,21},{16,20},{17,19},{18,17},{18,15},{17,13},{16,12},{13,11},{4,11},{127,127},{11,11},{18,0},{127,127}};
static const HersheyStroke sS[] = {{17,18},{15,20},{12,21},{8,21},{5,20},{3,18},{3,16},{4,14},{5,13},{7,12},{13,10},{15,9},{16,8},{17,6},{17,3},{15,1},{12,0},{8,0},{5,1},{3,3},{127,127}};
static const HersheyStroke sT[] = {{8,21},{8,0},{127,127},{1,21},{15,21},{127,127}};
static const HersheyStroke sU[] = {{4,21},{4,6},{5,3},{7,1},{10,0},{12,0},{15,1},{17,3},{18,6},{18,21},{127,127}};
static const HersheyStroke sV[] = {{1,21},{9,0},{127,127},{17,21},{9,0},{127,127}};
static const HersheyStroke sW[] = {{2,21},{7,0},{127,127},{12,21},{7,0},{127,127},{12,21},{17,0},{127,127},{22,21},{17,0},{127,127}};
static const HersheyStroke sX[] = {{3,21},{17,0},{127,127},{17,21},{3,0},{127,127}};
static const HersheyStroke sY[] = {{1,21},{9,11},{9,0},{127,127},{17,21},{9,11},{127,127}};
static const HersheyStroke sZ[] = {{17,21},{3,0},{127,127},{3,21},{17,21},{127,127},{3,0},{17,0},{127,127}};

static const HersheyStroke s_lbrac[] = {{4,25},{4,-7},{127,127},{5,25},{5,-7},{127,127},{4,25},{11,25},{127,127},{4,-7},{11,-7},{127,127}};
static const HersheyStroke s_bslash[]= {{0,20},{14,0},{127,127}};
static const HersheyStroke s_rbrac[] = {{9,25},{9,-7},{127,127},{10,25},{10,-7},{127,127},{3,25},{10,25},{127,127},{3,-7},{10,-7},{127,127}};
static const HersheyStroke s_caret[] = {{3,10},{8,18},{13,10},{127,127}};
static const HersheyStroke s_under[] = {{-2,-7},{16,-7},{127,127}};
static const HersheyStroke s_grave[] = {{6,21},{4,14},{127,127}};

// Lowercase letters
static const HersheyStroke sa[] = {{15,14},{15,0},{127,127},{15,11},{13,13},{11,14},{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{127,127}};
static const HersheyStroke sb[] = {{4,21},{4,0},{127,127},{4,11},{6,13},{8,14},{11,14},{13,13},{15,11},{16,8},{16,6},{15,3},{13,1},{11,0},{8,0},{6,1},{4,3},{127,127}};
static const HersheyStroke sc[] = {{15,11},{13,13},{11,14},{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{127,127}};
static const HersheyStroke sd[] = {{15,21},{15,0},{127,127},{15,11},{13,13},{11,14},{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{127,127}};
static const HersheyStroke se[] = {{3,8},{15,8},{15,10},{14,12},{13,13},{11,14},{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{127,127}};
static const HersheyStroke sf[] = {{10,21},{8,21},{6,20},{5,17},{5,0},{127,127},{2,14},{9,14},{127,127}};
static const HersheyStroke sg[] = {{15,14},{15,-2},{14,-5},{13,-6},{11,-7},{8,-7},{6,-6},{127,127},{15,11},{13,13},{11,14},{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{127,127}};
static const HersheyStroke sh[] = {{4,21},{4,0},{127,127},{4,10},{7,13},{9,14},{12,14},{14,13},{15,10},{15,0},{127,127}};
static const HersheyStroke si[] = {{3,21},{4,20},{5,21},{4,22},{3,21},{127,127},{4,14},{4,0},{127,127}};
static const HersheyStroke sj[] = {{5,21},{6,20},{7,21},{6,22},{5,21},{127,127},{6,14},{6,-3},{5,-6},{3,-7},{1,-7},{127,127}};
static const HersheyStroke sk[] = {{4,21},{4,0},{127,127},{14,14},{4,4},{127,127},{8,8},{15,0},{127,127}};
static const HersheyStroke sl[] = {{4,21},{4,0},{127,127}};
static const HersheyStroke sm[] = {{4,14},{4,0},{127,127},{4,10},{7,13},{9,14},{12,14},{14,13},{15,10},{15,0},{127,127},{15,10},{18,13},{20,14},{23,14},{25,13},{26,10},{26,0},{127,127}};
static const HersheyStroke sn[] = {{4,14},{4,0},{127,127},{4,10},{7,13},{9,14},{12,14},{14,13},{15,10},{15,0},{127,127}};
static const HersheyStroke so[] = {{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{16,6},{16,8},{15,11},{13,13},{11,14},{8,14},{127,127}};
static const HersheyStroke sp[] = {{4,14},{4,-7},{127,127},{4,11},{6,13},{8,14},{11,14},{13,13},{15,11},{16,8},{16,6},{15,3},{13,1},{11,0},{8,0},{6,1},{4,3},{127,127}};
static const HersheyStroke sq[] = {{15,14},{15,-7},{127,127},{15,11},{13,13},{11,14},{8,14},{6,13},{4,11},{3,8},{3,6},{4,3},{6,1},{8,0},{11,0},{13,1},{15,3},{127,127}};
static const HersheyStroke sr[] = {{4,14},{4,0},{127,127},{4,8},{5,11},{7,13},{9,14},{12,14},{127,127}};
static const HersheyStroke ss[] = {{14,11},{13,13},{10,14},{7,14},{4,13},{3,11},{4,9},{6,8},{11,7},{13,6},{14,4},{14,3},{13,1},{10,0},{7,0},{4,1},{3,3},{127,127}};
static const HersheyStroke st[] = {{5,21},{5,4},{6,1},{8,0},{10,0},{127,127},{2,14},{9,14},{127,127}};
static const HersheyStroke su[] = {{4,14},{4,4},{5,1},{7,0},{10,0},{12,1},{15,4},{127,127},{15,14},{15,0},{127,127}};
static const HersheyStroke sv[] = {{2,14},{8,0},{127,127},{14,14},{8,0},{127,127}};
static const HersheyStroke sw[] = {{3,14},{7,0},{127,127},{11,14},{7,0},{127,127},{11,14},{15,0},{127,127},{19,14},{15,0},{127,127}};
static const HersheyStroke sx[] = {{3,14},{14,0},{127,127},{14,14},{3,0},{127,127}};
static const HersheyStroke sy[] = {{2,14},{8,0},{127,127},{14,14},{8,0},{6,-4},{4,-6},{2,-7},{1,-7},{127,127}};
static const HersheyStroke sz[] = {{14,14},{3,0},{127,127},{3,14},{14,14},{127,127},{3,0},{14,0},{127,127}};

static const HersheyStroke s_lcurl[]  = {{9,25},{7,24},{6,23},{5,21},{5,19},{6,17},{7,16},{8,14},{8,12},{6,10},{127,127},{7,24},{6,22},{6,20},{7,18},{8,17},{9,15},{9,13},{8,11},{4,9},{8,7},{9,5},{9,3},{8,1},{7,0},{6,-2},{6,-4},{7,-6},{127,127},{6,8},{8,6},{8,4},{7,2},{6,1},{5,-1},{5,-3},{6,-5},{7,-6},{9,-7},{127,127}};
static const HersheyStroke s_bar[]    = {{4,25},{4,-7},{127,127}};
static const HersheyStroke s_rcurl[]  = {{5,25},{7,24},{8,23},{9,21},{9,19},{8,17},{7,16},{6,14},{6,12},{8,10},{127,127},{7,24},{8,22},{8,20},{7,18},{6,17},{5,15},{5,13},{6,11},{10,9},{6,7},{5,5},{5,3},{6,1},{7,0},{8,-2},{8,-4},{7,-6},{127,127},{8,8},{6,6},{6,4},{7,2},{8,1},{9,-1},{9,-3},{8,-5},{7,-6},{5,-7},{127,127}};
static const HersheyStroke s_tilde[]  = {{0,6},{1,8},{4,9},{7,8},{10,6},{13,5},{16,6},{17,8},{127,127}};

// The glyph table indexed by ASCII code - 32
// Using a simple struct of pointer + count
#define HS(arr) { (int8_t)(sizeof(arr)/sizeof(arr[0])-1), (int)(sizeof(arr)/sizeof(arr[0])-1), arr }
// width is set per-glyph manually below:

struct GlyphEntry {
    int8_t width;
    int    n;
    const HersheyStroke* data;
};

static const GlyphEntry kGlyphs[95] = {
    {8,  1, s_space},  // space
    {5,  6, s_excl},   // !
    {8,  6, s_quote},  // "
    {14, 12, s_hash},  // #
    {16, 17, s_dollar},// $
    {22, 19, s_pct},   // %
    {20, 17, s_amp},   // &
    {5,  3, s_apos},   // '
    {7,  7, s_lparen}, // (
    {7,  7, s_rparen}, // )
    {8,  9, s_star},   // *
    {11, 6, s_plus},   // +
    {5,  3, s_comma},  // ,
    {11, 3, s_minus},  // -
    {5,  3, s_period}, // .
    {12, 3, s_slash},  // /
    {20, 18, s_0},     // 0
    {14, 5, s_1},      // 1
    {20, 12, s_2},     // 2
    {20, 14, s_3},     // 3
    {20, 7, s_4},      // 4
    {20, 17, s_5},     // 5
    {20, 23, s_6},     // 6
    {20, 6, s_7},      // 7
    {20, 27, s_8},     // 8
    {20, 25, s_9},     // 9
    {5,  6, s_colon},  // :
    {5,  6, s_semi},   // ;
    {14, 4, s_lt},     // <
    {14, 6, s_eq},     // =
    {14, 4, s_gt},     // >
    {18, 14, s_quest}, // ?
    {27, 26, s_at},    // @
    {22, 9, sA},       // A
    {21, 24, sB},      // B
    {21, 18, sC},      // C
    {21, 16, sD},      // D
    {19, 12, sE},      // E
    {18, 9, sF},       // F
    {21, 22, sG},      // G
    {22, 9, sH},       // H
    {8,  9, sI},       // I
    {16, 9, sJ},       // J
    {21, 9, sK},       // K
    {17, 6, sL},       // L
    {24, 12, sM},      // M
    {22, 9, sN},       // N
    {22, 22, sO},      // O
    {21, 14, sP},      // P
    {22, 25, sQ},      // Q
    {21, 17, sR},      // R
    {20, 21, sS},      // S
    {16, 6, sT},       // T
    {22, 11, sU},      // U
    {18, 6, sV},       // V
    {24, 12, sW},      // W
    {20, 6, sX},       // X
    {18, 7, sY},       // Y
    {20, 9, sZ},       // Z
    {7,  12, s_lbrac}, // [
    {14, 3, s_bslash}, // '\'
    {7,  12, s_rbrac}, // ]
    {10, 4, s_caret},  // ^
    {16, 3, s_under},  // _
    {6,  3, s_grave},  // `
    {19, 18, sa},      // a
    {19, 18, sb},      // b
    {18, 15, sc},      // c
    {19, 18, sd},      // d
    {18, 18, se},      // e
    {12, 9, sf},       // f
    {19, 23, sg},      // g
    {19, 11, sh},      // h
    {8,  9, si},       // i
    {10, 12, sj},      // j
    {17, 9, sk},       // k
    {8,  3, sl},       // l
    {30, 19, sm},      // m
    {19, 11, sn},      // n
    {19, 18, so},      // o
    {19, 18, sp},      // p
    {19, 18, sq},      // q
    {13, 9, sr},       // r
    {17, 18, ss},      // s
    {12, 9, st},       // t
    {19, 11, su},      // u
    {16, 6, sv},       // v
    {22, 12, sw},      // w
    {17, 6, sx},       // x
    {19, 10, sy},      // y
    {17, 9, sz},       // z
    {14, 40, s_lcurl}, // {
    {8,  3, s_bar},    // |
    {14, 40, s_rcurl}, // }
    {10, 9, s_tilde},  // ~
};

// ─────────────────────────────────────────────────────────────────────────────
//  Text global (set by cue loader; read by generator)
// ─────────────────────────────────────────────────────────────────────────────
static std::string g_text = "IDHMFIS";

void set_text_string(const std::string& s) { g_text = s; }

// ─────────────────────────────────────────────────────────────────────────────
//  GenText
// ─────────────────────────────────────────────────────────────────────────────
class GenText final : public IGenerator {
public:
    const char* name()         const override { return "text"; }
    const char* display_name() const override { return "Text"; }
    const char* param_a_label()const override { return "Scroll Speed"; }
    const char* param_b_label()const override { return "Char Scale"; }
    const char* param_c_label()const override { return "Vertical Pos"; }

    PointBuffer generate(const GeneratorParams& p, double t) const override
    {
        PointBuffer pts;
        if (g_text.empty()) return pts;

        // Character scale: Hershey units map to normalised [-1,1] space.
        // Hershey y range is typically 0..21; we normalise to char_height.
        float char_scale = (0.04f + p.param_b * 0.12f) * p.scale;
        float scroll     = static_cast<float>(t) * p.speed * p.param_a * 0.3f;
        float vert       = (p.param_c * 2.f - 1.f) * p.scale;

        // Hershey baseline reference: y=0 at baseline, y=21 at cap height.
        // We centre vertically by offsetting by half cap height.
        float baseline_offset = -10.5f * char_scale + vert;

        // Compute total text width in Hershey units for centering
        float total_w = 0.f;
        for (char c : g_text)
        {
            int idx = static_cast<int>(c) - 32;
            if (idx < 0 || idx >= 95) idx = 0;
            total_w += static_cast<float>(kGlyphs[idx].width) * char_scale;
        }

        // Start X so text scrolls left from right edge
        float cursor_x = 1.2f - scroll;
        // Wrap: when cursor_x + total_w < -1.2, add total_w+2.4 to cursor_x
        while (cursor_x + total_w < -1.2f) cursor_x += total_w + 2.4f;

        Color4 ca = p.color_a;
        Color4 cb = p.color_b;
        int char_n = static_cast<int>(g_text.size());

        for (int ci = 0; ci < char_n; ++ci)
        {
            char c = g_text[ci];
            int idx = static_cast<int>(c) - 32;
            if (idx < 0 || idx >= 95) idx = 0;
            const GlyphEntry& glyph = kGlyphs[idx];

            float frac = char_n > 1 ?
                static_cast<float>(ci) / (char_n - 1) : 0.5f;
            Color4 col = ca.lerp(cb, frac);

            bool pen_up = true;
            for (int s = 0; s < glyph.n; ++s)
            {
                const HersheyStroke& stroke = glyph.data[s];
                if (stroke.x == 127 && stroke.y == 127)
                {
                    pen_up = true;
                    continue;
                }
                float wx = cursor_x + stroke.x * char_scale;
                float wy = baseline_offset + stroke.y * char_scale;

                LaserPoint lp = LaserPoint::from_norm(wx, wy,
                    col.r8(), col.g8(), col.b8(), pen_up);
                pts.push_back(lp);
                pen_up = false;
            }

            cursor_x += static_cast<float>(glyph.width) * char_scale;
        }

        return pts;
    }
};

std::unique_ptr<IGenerator> make_gen_text()
{
    return std::make_unique<GenText>();
}

} // namespace idhmfis
