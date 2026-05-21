// §B1 — Vector Primitive Rasterizers
// Converts geometric primitives to PointBuffer for DAC output.

#include "primitives.h"

#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cctype>

namespace idhmfis::prim {

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
static LaserPoint make_lit(float nx, float ny, const Color4& col, float intensity)
{
    float eff = std::clamp(intensity, 0.f, 1.f);
    return LaserPoint::from_norm(
        nx, ny,
        static_cast<uint8_t>(col.r8() * eff),
        static_cast<uint8_t>(col.g8() * eff),
        static_cast<uint8_t>(col.b8() * eff),
        false
    );
}

static LaserPoint make_blank(float nx, float ny)
{
    return LaserPoint::from_norm(nx, ny, 0, 0, 0, true);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Polyline rasterizer
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize_polyline(const Polyline& p)
{
    PointBuffer out;
    if (p.pts.empty()) return out;

    // Reserve: 1 blank + N lit points (+ 1 extra if closed)
    out.reserve(p.pts.size() + 2);

    // Blanking move to first point
    out.push_back(make_blank(p.pts[0].x, p.pts[0].y));

    for (const auto& pt : p.pts)
        out.push_back(make_lit(pt.x, pt.y, p.color, p.intensity));

    if (p.closed && p.pts.size() > 1)
        out.push_back(make_lit(p.pts[0].x, p.pts[0].y, p.color, p.intensity));

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Bezier rasterizer — de Casteljau cubic
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize_bezier(const Bezier& b)
{
    PointBuffer out;
    int n = std::max(2, b.subdivisions);
    out.reserve(static_cast<size_t>(n) + 1);

    // Blanking move to p0
    out.push_back(make_blank(b.p0.x, b.p0.y));

    for (int i = 0; i <= n; ++i)
    {
        float t  = static_cast<float>(i) / static_cast<float>(n);
        float u  = 1.f - t;
        float u2 = u * u;
        float u3 = u2 * u;
        float t2 = t * t;
        float t3 = t2 * t;

        float x = u3*b.p0.x + 3.f*u2*t*b.p1.x + 3.f*u*t2*b.p2.x + t3*b.p3.x;
        float y = u3*b.p0.y + 3.f*u2*t*b.p1.y + 3.f*u*t2*b.p2.y + t3*b.p3.y;

        out.push_back(make_lit(x, y, b.color, b.intensity));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Arc rasterizer — parametric cos/sin
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize_arc(const Arc& a)
{
    PointBuffer out;
    int n = std::max(2, a.subdivisions);
    out.reserve(static_cast<size_t>(n) + 2);

    float x0 = a.center.x + a.radius * std::cos(a.start_rad);
    float y0 = a.center.y + a.radius * std::sin(a.start_rad);
    out.push_back(make_blank(x0, y0));

    for (int i = 0; i <= n; ++i)
    {
        float t     = static_cast<float>(i) / static_cast<float>(n);
        float angle = a.start_rad + t * (a.end_rad - a.start_rad);
        float x     = a.center.x + a.radius * std::cos(angle);
        float y     = a.center.y + a.radius * std::sin(angle);
        out.push_back(make_lit(x, y, a.color, a.intensity));
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  ClosedPolygon — polyline with pts[0] appended at end
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize_polygon(const ClosedPolygon& p)
{
    Polyline pl;
    pl.pts       = p.pts;
    pl.color     = p.color;
    pl.intensity = p.intensity;
    pl.closed    = true;
    return rasterize_polyline(pl);
}

// ─────────────────────────────────────────────────────────────────────────────
//  BeamPrimitive — two-point segment (origin → origin + dir*len)
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize_beam(const BeamPrimitive& b)
{
    PointBuffer out;
    out.reserve(3);

    float ex = b.origin.x + b.direction.x * b.length;
    float ey = b.origin.y + b.direction.y * b.length;

    out.push_back(make_blank(b.origin.x, b.origin.y));
    out.push_back(make_lit(b.origin.x, b.origin.y, b.color, b.intensity));
    out.push_back(make_lit(ex, ey, b.color, b.intensity));
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Hershey-style minimal stroke font
//
//  Coordinate system: [-0.5 .. 0.5] per character cell, scaled by TextRun::size.
//  Each glyph is stored as a flat array of (x,y) pairs terminated by (99,99).
//  A pen-up move is encoded as (88,88) followed by the next position.
//  Characters are spaced by 'size' in x.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr float kPU = 88.f;  // pen-up sentinel x value
static constexpr float kEG = 99.f;  // end-glyph sentinel

// Each glyph: flat array of floats, pairs of (x,y). 88,88 = penup; 99,99 = end.
// Coordinates in [-0.5, 0.5].

// Helper macro to make glyph definitions more readable.
#define PU kPU, kPU
#define EG kEG, kEG

static const float glyph_A[] = {
    -0.3f,-0.5f,  0.f,0.5f,  0.3f,-0.5f,  PU,
    -0.15f,0.f,  0.15f,0.f,  EG
};
static const float glyph_B[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,0.15f,  0.15f,0.f,  -0.3f,0.f,  PU,
    0.15f,0.f,  0.3f,-0.15f,  0.3f,-0.35f,  0.15f,-0.5f,  -0.3f,-0.5f,  EG
};
static const float glyph_C[] = {
    0.3f,0.35f,  0.15f,0.5f,  -0.15f,0.5f,  -0.3f,0.35f,
    -0.3f,-0.35f,  -0.15f,-0.5f,  0.15f,-0.5f,  0.3f,-0.35f,  EG
};
static const float glyph_D[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.1f,0.5f,  0.3f,0.3f,
    0.3f,-0.3f,  0.1f,-0.5f,  -0.3f,-0.5f,  EG
};
static const float glyph_E[] = {
    0.3f,0.5f,  -0.3f,0.5f,  -0.3f,-0.5f,  0.3f,-0.5f,  PU,
    -0.3f,0.f,  0.15f,0.f,  EG
};
static const float glyph_F[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.3f,0.5f,  PU,
    -0.3f,0.f,  0.15f,0.f,  EG
};
static const float glyph_G[] = {
    0.3f,0.35f,  0.15f,0.5f,  -0.15f,0.5f,  -0.3f,0.35f,
    -0.3f,-0.35f,  -0.15f,-0.5f,  0.15f,-0.5f,  0.3f,-0.35f,
    0.3f,0.f,  0.05f,0.f,  EG
};
static const float glyph_H[] = {
    -0.3f,0.5f,  -0.3f,-0.5f,  PU,
    0.3f,0.5f,  0.3f,-0.5f,  PU,
    -0.3f,0.f,  0.3f,0.f,  EG
};
static const float glyph_I[] = {
    0.f,0.5f,  0.f,-0.5f,  PU,
    -0.2f,0.5f,  0.2f,0.5f,  PU,
    -0.2f,-0.5f,  0.2f,-0.5f,  EG
};
static const float glyph_J[] = {
    0.2f,0.5f,  0.2f,-0.3f,  0.1f,-0.5f,  -0.1f,-0.5f,  -0.2f,-0.3f,  EG
};
static const float glyph_K[] = {
    -0.3f,0.5f,  -0.3f,-0.5f,  PU,
    0.3f,0.5f,  -0.3f,0.f,  PU,
    -0.05f,0.2f,  0.3f,-0.5f,  EG
};
static const float glyph_L[] = {
    -0.3f,0.5f,  -0.3f,-0.5f,  0.3f,-0.5f,  EG
};
static const float glyph_M[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.f,0.1f,  0.3f,0.5f,  0.3f,-0.5f,  EG
};
static const float glyph_N[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.3f,-0.5f,  0.3f,0.5f,  EG
};
static const float glyph_O[] = {
    -0.15f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,-0.35f,  0.15f,-0.5f,
    -0.15f,-0.5f,  -0.3f,-0.35f,  -0.3f,0.35f,  -0.15f,0.5f,  EG
};
static const float glyph_P[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,0.15f,  0.15f,0.f,  -0.3f,0.f,  EG
};
static const float glyph_Q[] = {
    -0.15f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,-0.35f,  0.15f,-0.5f,
    -0.15f,-0.5f,  -0.3f,-0.35f,  -0.3f,0.35f,  -0.15f,0.5f,  PU,
    0.05f,-0.2f,  0.3f,-0.5f,  EG
};
static const float glyph_R[] = {
    -0.3f,-0.5f,  -0.3f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,0.15f,  0.15f,0.f,  -0.3f,0.f,  PU,
    0.f,0.f,  0.3f,-0.5f,  EG
};
static const float glyph_S[] = {
    0.3f,0.35f,  0.15f,0.5f,  -0.15f,0.5f,  -0.3f,0.35f,  -0.3f,0.1f,
    0.3f,-0.1f,  0.3f,-0.35f,  0.15f,-0.5f,  -0.15f,-0.5f,  -0.3f,-0.35f,  EG
};
static const float glyph_T[] = {
    -0.3f,0.5f,  0.3f,0.5f,  PU,
    0.f,0.5f,  0.f,-0.5f,  EG
};
static const float glyph_U[] = {
    -0.3f,0.5f,  -0.3f,-0.3f,  -0.15f,-0.5f,  0.15f,-0.5f,  0.3f,-0.3f,  0.3f,0.5f,  EG
};
static const float glyph_V[] = {
    -0.3f,0.5f,  0.f,-0.5f,  0.3f,0.5f,  EG
};
static const float glyph_W[] = {
    -0.3f,0.5f,  -0.15f,-0.5f,  0.f,0.f,  0.15f,-0.5f,  0.3f,0.5f,  EG
};
static const float glyph_X[] = {
    -0.3f,0.5f,  0.3f,-0.5f,  PU,
    0.3f,0.5f,  -0.3f,-0.5f,  EG
};
static const float glyph_Y[] = {
    -0.3f,0.5f,  0.f,0.f,  0.3f,0.5f,  PU,
    0.f,0.f,  0.f,-0.5f,  EG
};
static const float glyph_Z[] = {
    -0.3f,0.5f,  0.3f,0.5f,  -0.3f,-0.5f,  0.3f,-0.5f,  EG
};

static const float glyph_0[] = {
    -0.15f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,-0.35f,  0.15f,-0.5f,
    -0.15f,-0.5f,  -0.3f,-0.35f,  -0.3f,0.35f,  -0.15f,0.5f,  PU,
    -0.15f,0.35f,  0.15f,-0.35f,  EG
};
static const float glyph_1[] = {
    -0.1f,0.3f,  0.f,0.5f,  0.f,-0.5f,  PU,
    -0.2f,-0.5f,  0.2f,-0.5f,  EG
};
static const float glyph_2[] = {
    -0.3f,0.3f,  -0.15f,0.5f,  0.15f,0.5f,  0.3f,0.3f,
    0.3f,0.1f,  -0.3f,-0.4f,  -0.3f,-0.5f,  0.3f,-0.5f,  EG
};
static const float glyph_3[] = {
    -0.3f,0.5f,  0.3f,0.5f,  0.f,0.1f,  0.25f,0.f,
    0.3f,-0.15f,  0.3f,-0.35f,  0.15f,-0.5f,  -0.15f,-0.5f,  -0.3f,-0.35f,  EG
};
static const float glyph_4[] = {
    0.1f,-0.5f,  0.1f,0.5f,  -0.35f,0.f,  0.35f,0.f,  EG
};
static const float glyph_5[] = {
    0.3f,0.5f,  -0.3f,0.5f,  -0.3f,0.1f,  0.15f,0.1f,
    0.3f,-0.05f,  0.3f,-0.35f,  0.15f,-0.5f,  -0.15f,-0.5f,  -0.3f,-0.35f,  EG
};
static const float glyph_6[] = {
    0.25f,0.5f,  -0.15f,0.5f,  -0.3f,0.3f,  -0.3f,-0.35f,
    -0.15f,-0.5f,  0.15f,-0.5f,  0.3f,-0.35f,  0.3f,-0.05f,
    0.15f,0.1f,  -0.3f,0.1f,  EG
};
static const float glyph_7[] = {
    -0.3f,0.5f,  0.3f,0.5f,  -0.1f,-0.5f,  EG
};
static const float glyph_8[] = {
    -0.15f,0.f,  -0.3f,0.15f,  -0.3f,0.35f,  -0.15f,0.5f,
    0.15f,0.5f,  0.3f,0.35f,  0.3f,0.15f,  -0.15f,0.f,
    -0.3f,-0.15f,  -0.3f,-0.35f,  -0.15f,-0.5f,
    0.15f,-0.5f,  0.3f,-0.35f,  0.3f,-0.15f,  -0.15f,0.f,  EG
};
static const float glyph_9[] = {
    -0.25f,-0.5f,  0.15f,-0.5f,  0.3f,-0.3f,  0.3f,0.35f,
    0.15f,0.5f,  -0.15f,0.5f,  -0.3f,0.35f,  -0.3f,0.05f,
    -0.15f,-0.1f,  0.3f,-0.1f,  EG
};

// Punctuation / special
static const float glyph_space[] = { EG };  // nothing to draw
static const float glyph_dot[]   = { -0.05f,-0.45f,  0.05f,-0.45f,  0.05f,-0.55f,  -0.05f,-0.55f,  -0.05f,-0.45f, EG };
static const float glyph_comma[] = { 0.f,-0.4f,  -0.1f,-0.6f,  EG };
static const float glyph_exclam[]= { 0.f,0.5f,  0.f,0.f,  PU,  0.f,-0.35f,  0.f,-0.5f, EG };
static const float glyph_minus[] = { -0.25f,0.f,  0.25f,0.f, EG };
static const float glyph_plus[]  = { 0.f,0.35f,  0.f,-0.35f,  PU,  -0.25f,0.f,  0.25f,0.f, EG };
static const float glyph_slash[] = { -0.2f,-0.5f,  0.2f,0.5f, EG };
static const float glyph_colon[] = {
    -0.05f,0.15f,  0.05f,0.15f,  0.05f,0.05f,  -0.05f,0.05f,  -0.05f,0.15f,  PU,
    -0.05f,-0.35f,  0.05f,-0.35f,  0.05f,-0.45f,  -0.05f,-0.45f,  -0.05f,-0.35f,  EG
};
static const float glyph_question[]= {
    -0.2f,0.35f,  -0.15f,0.5f,  0.15f,0.5f,  0.2f,0.35f,
    0.2f,0.1f,  0.f,-0.1f,  0.f,-0.3f,  PU,
    0.f,-0.45f,  0.f,-0.5f,  EG
};
static const float glyph_at[]     = { // '@' — simplified circle with hook
    0.15f,0.f,  0.05f,-0.1f,  -0.1f,-0.1f,  -0.2f,0.f,
    -0.2f,0.15f,  -0.1f,0.25f,  0.1f,0.25f,  0.2f,0.15f,
    0.2f,-0.3f,  0.1f,-0.4f,  -0.15f,-0.4f,  -0.3f,-0.2f,  EG
};
static const float glyph_lpar[]   = { 0.1f,0.5f,  -0.1f,0.25f,  -0.1f,-0.25f,  0.1f,-0.5f,  EG };
static const float glyph_rpar[]   = { -0.1f,0.5f,  0.1f,0.25f,  0.1f,-0.25f,  -0.1f,-0.5f,  EG };
static const float glyph_quote[]  = { -0.1f,0.5f,  -0.15f,0.3f,  PU,  0.1f,0.5f,  0.05f,0.3f,  EG };
static const float glyph_amp[]    = { // '&'
    0.3f,-0.5f,  -0.15f,0.f,  -0.3f,0.15f,  -0.3f,0.35f,
    -0.15f,0.5f,  0.15f,0.5f,  0.3f,0.35f,  0.3f,0.2f,  -0.3f,-0.5f,  EG
};
static const float glyph_star[]   = {
    0.f,0.4f,  0.f,-0.4f,  PU,
    -0.35f,0.2f,  0.35f,-0.2f,  PU,
    0.35f,0.2f,  -0.35f,-0.2f,  EG
};
static const float glyph_eq[]     = { -0.25f,0.1f,  0.25f,0.1f,  PU,  -0.25f,-0.1f,  0.25f,-0.1f,  EG };
static const float glyph_lt[]     = { 0.2f,0.4f,  -0.2f,0.f,  0.2f,-0.4f,  EG };
static const float glyph_gt[]     = { -0.2f,0.4f,  0.2f,0.f,  -0.2f,-0.4f,  EG };
static const float glyph_hash[]   = {
    -0.1f,0.5f,  -0.2f,-0.5f,  PU,
    0.1f,0.5f,   0.f,-0.5f,  PU,
    -0.25f,0.15f,  0.25f,0.15f,  PU,
    -0.25f,-0.15f, 0.25f,-0.15f,  EG
};
static const float glyph_dollar[] = {
    0.f,0.6f,  0.f,-0.6f,  PU,
    0.25f,0.4f,  -0.25f,0.4f,  -0.3f,0.25f,  -0.3f,0.1f,
    0.3f,-0.1f,  0.3f,-0.25f,  0.25f,-0.4f,  -0.25f,-0.4f,  EG
};
static const float glyph_percent[]= {
    0.3f,0.5f,  -0.3f,-0.5f,  PU,
    -0.2f,0.35f,  -0.1f,0.35f,  -0.1f,0.5f,  -0.2f,0.5f,  -0.2f,0.35f,  PU,
    0.1f,-0.35f,  0.2f,-0.35f,  0.2f,-0.5f,  0.1f,-0.5f,  0.1f,-0.35f,  EG
};
static const float glyph_caret[]  = { -0.2f,0.f,  0.f,0.4f,  0.2f,0.f,  EG };
static const float glyph_under[]  = { -0.3f,-0.5f,  0.3f,-0.5f,  EG };
static const float glyph_bar[]    = { 0.f,0.5f,  0.f,-0.5f,  EG };
static const float glyph_tilde[]  = {
    -0.3f,-0.05f,  -0.15f,0.1f,  0.15f,-0.1f,  0.3f,0.05f,  EG
};
static const float glyph_lbrace[] = {
    0.1f,0.5f,  0.f,0.4f,  0.f,0.1f,  -0.1f,0.f,  0.f,-0.1f,  0.f,-0.4f,  0.1f,-0.5f,  EG
};
static const float glyph_rbrace[] = {
    -0.1f,0.5f,  0.f,0.4f,  0.f,0.1f,  0.1f,0.f,  0.f,-0.1f,  0.f,-0.4f,  -0.1f,-0.5f,  EG
};
static const float glyph_lbracket[] = { 0.1f,0.5f,  -0.1f,0.5f,  -0.1f,-0.5f,  0.1f,-0.5f,  EG };
static const float glyph_rbracket[] = { -0.1f,0.5f,  0.1f,0.5f,  0.1f,-0.5f,  -0.1f,-0.5f,  EG };
static const float glyph_semicol[] = {
    -0.05f,0.15f,  0.05f,0.15f,  0.05f,0.05f,  -0.05f,0.05f,  -0.05f,0.15f,  PU,
    0.05f,-0.35f,  -0.1f,-0.55f,  EG
};
static const float glyph_backsl[] = { 0.2f,0.5f,  -0.2f,-0.5f,  EG };
static const float glyph_apos[]   = { 0.f,0.5f,  -0.05f,0.3f,  EG };
static const float glyph_grave[]  = { -0.05f,0.5f,  0.f,0.3f,  EG };

#undef PU
#undef EG

struct GlyphEntry { char ch; const float* data; };

static const GlyphEntry kGlyphTable[] = {
    {' ', glyph_space},
    {'!', glyph_exclam},
    {'"', glyph_quote},
    {'#', glyph_hash},
    {'$', glyph_dollar},
    {'%', glyph_percent},
    {'&', glyph_amp},
    {'\'',glyph_apos},
    {'(', glyph_lpar},
    {')', glyph_rpar},
    {'*', glyph_star},
    {'+', glyph_plus},
    {',', glyph_comma},
    {'-', glyph_minus},
    {'.', glyph_dot},
    {'/', glyph_slash},
    {'0', glyph_0},
    {'1', glyph_1},
    {'2', glyph_2},
    {'3', glyph_3},
    {'4', glyph_4},
    {'5', glyph_5},
    {'6', glyph_6},
    {'7', glyph_7},
    {'8', glyph_8},
    {'9', glyph_9},
    {':', glyph_colon},
    {';', glyph_semicol},
    {'<', glyph_lt},
    {'=', glyph_eq},
    {'>', glyph_gt},
    {'?', glyph_question},
    {'@', glyph_at},
    {'A', glyph_A},
    {'B', glyph_B},
    {'C', glyph_C},
    {'D', glyph_D},
    {'E', glyph_E},
    {'F', glyph_F},
    {'G', glyph_G},
    {'H', glyph_H},
    {'I', glyph_I},
    {'J', glyph_J},
    {'K', glyph_K},
    {'L', glyph_L},
    {'M', glyph_M},
    {'N', glyph_N},
    {'O', glyph_O},
    {'P', glyph_P},
    {'Q', glyph_Q},
    {'R', glyph_R},
    {'S', glyph_S},
    {'T', glyph_T},
    {'U', glyph_U},
    {'V', glyph_V},
    {'W', glyph_W},
    {'X', glyph_X},
    {'Y', glyph_Y},
    {'Z', glyph_Z},
    {'[', glyph_lbracket},
    {'\\',glyph_backsl},
    {']', glyph_rbracket},
    {'^', glyph_caret},
    {'_', glyph_under},
    {'`', glyph_grave},
    // lowercase: map to uppercase
    {'a', glyph_A},
    {'b', glyph_B},
    {'c', glyph_C},
    {'d', glyph_D},
    {'e', glyph_E},
    {'f', glyph_F},
    {'g', glyph_G},
    {'h', glyph_H},
    {'i', glyph_I},
    {'j', glyph_J},
    {'k', glyph_K},
    {'l', glyph_L},
    {'m', glyph_M},
    {'n', glyph_N},
    {'o', glyph_O},
    {'p', glyph_P},
    {'q', glyph_Q},
    {'r', glyph_R},
    {'s', glyph_S},
    {'t', glyph_T},
    {'u', glyph_U},
    {'v', glyph_V},
    {'w', glyph_W},
    {'x', glyph_X},
    {'y', glyph_Y},
    {'z', glyph_Z},
    {'|', glyph_bar},
    {'{', glyph_lbrace},
    {'}', glyph_rbrace},
    {'~', glyph_tilde},
};

static const float* find_glyph(char ch)
{
    for (const auto& e : kGlyphTable)
        if (e.ch == ch) return e.data;
    return glyph_question;   // unknown → '?'
}

static void rasterize_glyph(const float* data,
                              float ox, float oy, float sz,
                              const Color4& col, float intensity,
                              PointBuffer& out)
{
    bool pen_down = false;
    for (int i = 0; ; i += 2)
    {
        float gx = data[i];
        float gy = data[i + 1];

        if (gx == kEG) break;

        if (gx == kPU) {
            pen_down = false;
            continue;
        }

        float wx = ox + gx * sz;
        float wy = oy + gy * sz;

        if (!pen_down) {
            out.push_back(make_blank(wx, wy));
            pen_down = true;
        }
        out.push_back(make_lit(wx, wy, col, intensity));
    }
}

PointBuffer rasterize_textrun(const TextRun& t)
{
    PointBuffer out;
    if (t.text.empty()) return out;

    float cursor_x = t.pos.x;
    float sz = t.size;

    for (char ch : t.text)
    {
        const float* glyph = find_glyph(ch);
        rasterize_glyph(glyph, cursor_x, t.pos.y, sz, t.color, t.intensity, out);
        cursor_x += sz * 1.2f;  // character advance = 120% of size
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  SVGPath parser — supports M, L, C, Q, Z only
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Skip whitespace and optional comma
static void skip_ws_comma(const char*& p, const char* end)
{
    while (p < end && (std::isspace(static_cast<unsigned char>(*p)) || *p == ','))
        ++p;
}

// Parse one float from the current position; returns false if none found.
static bool parse_float(const char*& p, const char* end, float& val)
{
    skip_ws_comma(p, end);
    if (p >= end) return false;

    char* np = nullptr;
    // strtof needs a non-const pointer; we cast conservatively
    val = std::strtof(p, &np);
    if (np == p) return false;
    p = np;
    return true;
}

// Parse exactly N floats; returns false if any fails.
static bool parse_n(const char*& p, const char* end, float* vals, int n)
{
    for (int i = 0; i < n; ++i)
        if (!parse_float(p, end, vals[i])) return false;
    return true;
}

// Evaluate a quadratic bezier (P0,P1,P2) at t → (x,y)
static void quad_bezier_pt(float p0x, float p0y,
                            float p1x, float p1y,
                            float p2x, float p2y,
                            float t, float& ox, float& oy)
{
    float u = 1.f - t;
    ox = u*u*p0x + 2.f*u*t*p1x + t*t*p2x;
    oy = u*u*p0y + 2.f*u*t*p1y + t*t*p2y;
}

} // anonymous namespace

PointBuffer rasterize_svgpath(const SVGPath& sv)
{
    PointBuffer out;
    const std::string& d = sv.path_data;
    const char* p   = d.c_str();
    const char* end = p + d.size();

    float cx = sv.origin.x;  // current position
    float cy = sv.origin.y;
    bool  pen_down = false;
    float subpath_start_x = cx;
    float subpath_start_y = cy;

    auto emit = [&](float wx, float wy, bool blank)
    {
        float sx = sv.origin.x + wx * sv.scale;
        float sy = sv.origin.y + wy * sv.scale;
        if (blank)
            out.push_back(make_blank(sx, sy));
        else
            out.push_back(make_lit(sx, sy, sv.color, sv.intensity));
    };

    // We track raw SVG coords; scale/offset applied in emit().
    // For M/L we use raw coords so subtract origin first, then emit adds it back.
    // Actually simpler: transform once in emit:
    //   world = origin + raw * scale
    // So we track raw SVG coords in (cx, cy).

    // Re-define emit to treat cx,cy as raw SVG space:
    auto emit_raw = [&](float rx, float ry, bool blank)
    {
        float wx = sv.origin.x + rx * sv.scale;
        float wy = sv.origin.y + ry * sv.scale;
        if (blank)
            out.push_back(make_blank(wx, wy));
        else
            out.push_back(make_lit(wx, wy, sv.color, sv.intensity));
    };
    (void)emit; // suppress unused warning

    constexpr int kBezierSteps = 16;

    while (p < end)
    {
        skip_ws_comma(p, end);
        if (p >= end) break;

        char cmd = *p++;
        if (!std::isalpha(static_cast<unsigned char>(cmd))) {
            // Not a command letter — skip
            continue;
        }

        bool relative = std::islower(static_cast<unsigned char>(cmd));
        char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));

        if (upper == 'M')
        {
            float v[2] = {};
            if (!parse_n(p, end, v, 2)) continue;
            if (relative) { cx += v[0]; cy += v[1]; }
            else          { cx  = v[0]; cy  = v[1]; }
            subpath_start_x = cx;
            subpath_start_y = cy;
            emit_raw(cx, cy, true);
            pen_down = false;

            // Subsequent pairs after M are implicit L
            while (true) {
                skip_ws_comma(p, end);
                if (p >= end || std::isalpha(static_cast<unsigned char>(*p))) break;
                float lv[2] = {};
                if (!parse_n(p, end, lv, 2)) break;
                if (relative) { cx += lv[0]; cy += lv[1]; }
                else          { cx  = lv[0]; cy  = lv[1]; }
                if (!pen_down) {
                    emit_raw(cx, cy, true);
                    pen_down = true;
                }
                emit_raw(cx, cy, false);
            }
        }
        else if (upper == 'L')
        {
            while (true) {
                skip_ws_comma(p, end);
                if (p >= end || std::isalpha(static_cast<unsigned char>(*p))) break;
                float v[2] = {};
                if (!parse_n(p, end, v, 2)) break;
                if (relative) { cx += v[0]; cy += v[1]; }
                else          { cx  = v[0]; cy  = v[1]; }
                if (!pen_down) {
                    emit_raw(cx, cy, true);
                    pen_down = true;
                }
                emit_raw(cx, cy, false);
                pen_down = true;
            }
        }
        else if (upper == 'Z')
        {
            // Close subpath
            if (pen_down) {
                emit_raw(subpath_start_x, subpath_start_y, false);
            }
            cx = subpath_start_x;
            cy = subpath_start_y;
            pen_down = false;
        }
        else if (upper == 'C')
        {
            // Cubic bezier: 6 params per curve
            while (true) {
                skip_ws_comma(p, end);
                if (p >= end || std::isalpha(static_cast<unsigned char>(*p))) break;
                float v[6] = {};
                if (!parse_n(p, end, v, 6)) break;

                float x1, y1, x2, y2, x3, y3;
                if (relative) {
                    x1 = cx+v[0]; y1 = cy+v[1];
                    x2 = cx+v[2]; y2 = cy+v[3];
                    x3 = cx+v[4]; y3 = cy+v[5];
                } else {
                    x1=v[0]; y1=v[1]; x2=v[2]; y2=v[3]; x3=v[4]; y3=v[5];
                }

                if (!pen_down) {
                    emit_raw(cx, cy, true);
                    pen_down = true;
                }

                for (int i = 1; i <= kBezierSteps; ++i) {
                    float t  = static_cast<float>(i) / static_cast<float>(kBezierSteps);
                    float u  = 1.f - t;
                    float u2 = u*u, u3 = u2*u;
                    float t2 = t*t, t3 = t2*t;
                    float bx = u3*cx + 3.f*u2*t*x1 + 3.f*u*t2*x2 + t3*x3;
                    float by = u3*cy + 3.f*u2*t*y1 + 3.f*u*t2*y2 + t3*y3;
                    emit_raw(bx, by, false);
                }
                cx = x3; cy = y3;
            }
        }
        else if (upper == 'Q')
        {
            // Quadratic bezier: 4 params per curve
            while (true) {
                skip_ws_comma(p, end);
                if (p >= end || std::isalpha(static_cast<unsigned char>(*p))) break;
                float v[4] = {};
                if (!parse_n(p, end, v, 4)) break;

                float x1, y1, x2, y2;
                if (relative) {
                    x1 = cx+v[0]; y1 = cy+v[1];
                    x2 = cx+v[2]; y2 = cy+v[3];
                } else {
                    x1=v[0]; y1=v[1]; x2=v[2]; y2=v[3];
                }

                if (!pen_down) {
                    emit_raw(cx, cy, true);
                    pen_down = true;
                }

                for (int i = 1; i <= kBezierSteps; ++i) {
                    float t = static_cast<float>(i) / static_cast<float>(kBezierSteps);
                    float bx, by;
                    quad_bezier_pt(cx, cy, x1, y1, x2, y2, t, bx, by);
                    emit_raw(bx, by, false);
                }
                cx = x2; cy = y2;
            }
        }
        else
        {
            // Unsupported command — skip arguments until next command letter
            while (p < end && !std::isalpha(static_cast<unsigned char>(*p)))
                ++p;
        }
    }
    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Generic rasterize dispatch
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize(const Primitive& prim)
{
    return std::visit([](const auto& p) -> PointBuffer {
        using T = std::decay_t<decltype(p)>;
        if constexpr (std::is_same_v<T, Point>) {
            PointBuffer out;
            out.push_back(make_blank(p.pos.x, p.pos.y));
            out.push_back(make_lit(p.pos.x, p.pos.y, p.color, p.intensity));
            return out;
        } else if constexpr (std::is_same_v<T, Polyline>) {
            return rasterize_polyline(p);
        } else if constexpr (std::is_same_v<T, Bezier>) {
            return rasterize_bezier(p);
        } else if constexpr (std::is_same_v<T, Arc>) {
            return rasterize_arc(p);
        } else if constexpr (std::is_same_v<T, ClosedPolygon>) {
            return rasterize_polygon(p);
        } else if constexpr (std::is_same_v<T, BeamPrimitive>) {
            return rasterize_beam(p);
        } else if constexpr (std::is_same_v<T, TextRun>) {
            return rasterize_textrun(p);
        } else if constexpr (std::is_same_v<T, SVGPath>) {
            return rasterize_svgpath(p);
        } else {
            return {};
        }
    }, prim);
}

PointBuffer rasterize(const std::vector<Primitive>& prims)
{
    PointBuffer out;
    for (const auto& prim : prims) {
        PointBuffer seg = rasterize(prim);
        out.insert(out.end(), seg.begin(), seg.end());
    }
    return out;
}

} // namespace idhmfis::prim
