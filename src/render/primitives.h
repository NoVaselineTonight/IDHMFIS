#pragma once
// §B1 — Vector Primitives
// Geometric primitive types and rasterizers that produce PointBuffer output.

#include "../core/types.h"
#include <string>
#include <variant>
#include <vector>

namespace idhmfis::prim {

// ─────────────────────────────────────────────────────────────────────────────
//  2-D position helper (distinct from Color4 which lives in idhmfis namespace)
// ─────────────────────────────────────────────────────────────────────────────
struct Point2D { float x = 0.f, y = 0.f; };

// ─────────────────────────────────────────────────────────────────────────────
//  Geometric primitive types
// ─────────────────────────────────────────────────────────────────────────────
struct Point {
    Point2D pos;
    Color4  color;
    float   intensity = 1.f;
};

struct Polyline {
    std::vector<Point2D> pts;
    Color4               color;
    float                intensity = 1.f;
    bool                 closed    = false;
};

struct Bezier {
    Point2D p0, p1, p2, p3;
    Color4  color;
    float   intensity    = 1.f;
    int     subdivisions = 32;
};

struct Arc {
    Point2D center;
    float   radius;
    float   start_rad;
    float   end_rad;
    Color4  color;
    float   intensity    = 1.f;
    int     subdivisions = 32;
};

struct ClosedPolygon {
    std::vector<Point2D> pts;
    Color4               color;
    float                intensity = 1.f;
};

struct BeamPrimitive {
    Point2D origin;
    Point2D direction;
    float   length;
    Color4  color;
    float   intensity = 1.f;
};

struct TextRun {
    Point2D     pos;
    std::string text;
    float       size      = 0.1f;
    Color4      color;
    float       intensity = 1.f;
};

struct SVGPath {
    std::string path_data;
    Color4      color;
    float       intensity = 1.f;
    float       scale     = 1.f;
    Point2D     origin;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Variant holding any single primitive
// ─────────────────────────────────────────────────────────────────────────────
using Primitive = std::variant<
    Point, Polyline, Bezier, Arc,
    ClosedPolygon, BeamPrimitive, TextRun, SVGPath
>;

// ─────────────────────────────────────────────────────────────────────────────
//  Rasterize API
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer rasterize(const Primitive& prim);
PointBuffer rasterize(const std::vector<Primitive>& prims);

PointBuffer rasterize_polyline  (const Polyline&       p);
PointBuffer rasterize_bezier    (const Bezier&         b);
PointBuffer rasterize_arc       (const Arc&            a);
PointBuffer rasterize_polygon   (const ClosedPolygon&  p);
PointBuffer rasterize_beam      (const BeamPrimitive&  b);
PointBuffer rasterize_textrun   (const TextRun&        t);
PointBuffer rasterize_svgpath   (const SVGPath&        s);

} // namespace idhmfis::prim
