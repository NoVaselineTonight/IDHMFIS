#pragma once
// SVG import -> vectorized for laser (path ordering, blanking, point density optimization)
#include "../core/types.h"
#include <string>
#include <vector>

namespace idhmfis {

class SvgImporter {
public:
    struct Options {
        float point_spacing   = 0.005f;
        int   anchor_repeats  = 3;
        float blank_lead      = 0.002f;
        bool  optimize_order  = true;
        float flatness        = 0.01f;
    };

    PointBuffer import(const std::string& svg_path, const Options& opt = {});
    PointBuffer import_from_string(const std::string& svg_text, const Options& opt = {});
    std::string error() const { return error_; }

private:
    std::string error_;
    using Polyline = std::vector<std::pair<float,float>>;

    std::vector<Polyline> parse_paths(const std::string& svg_text);
    void   parse_path_d(const std::string& d, std::vector<Polyline>& out, float flatness);
    void   flatten_bezier_cubic(float x0,float y0,float x1,float y1,
                                float x2,float y2,float x3,float y3,
                                float tol, Polyline& out);
    void   flatten_bezier_quad(float x0,float y0,float x1,float y1,
                               float x2,float y2, float tol, Polyline& out);
    void   flatten_arc(float x0,float y0,float rx,float ry,
                       float x_rot,float large_arc,float sweep_flag,
                       float ex,float ey, float flatness, Polyline& out);
    void   optimize_path_order(std::vector<Polyline>& paths);
    void   normalize_paths(std::vector<Polyline>& paths);
    PointBuffer paths_to_points(const std::vector<Polyline>& paths, const Options& opt);
};

} // namespace idhmfis
