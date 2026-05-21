// SVG to laser vector importer.
// Supports: M, L, H, V, C, S, Q, T, A, Z (absolute and relative).
#include "svg_importer.h"
#include <fstream>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

namespace idhmfis {

static constexpr float kPi = 3.14159265358979323846f;

PointBuffer SvgImporter::import(const std::string& path, const Options& opt) {
    std::ifstream f(path);
    if (!f) { error_ = "Cannot open SVG: " + path; return {}; }
    std::string text((std::istreambuf_iterator<char>(f)), {});
    return import_from_string(text, opt);
}

PointBuffer SvgImporter::import_from_string(const std::string& svg, const Options& opt) {
    error_.clear();
    auto paths = parse_paths(svg);
    if (paths.empty()) return {};
    normalize_paths(paths);
    if (opt.optimize_order) optimize_path_order(paths);
    return paths_to_points(paths, opt);
}

static std::vector<std::string> extract_path_ds(const std::string& svg) {
    std::vector<std::string> result;
    size_t pos = 0;
    while ((pos = svg.find("<path", pos)) != std::string::npos) {
        size_t end = svg.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = svg.substr(pos, end - pos);
        for (const char* attr : {" d=", "\td="}) {
            size_t d = tag.find(attr);
            if (d != std::string::npos) {
                size_t q1 = tag.find('"', d + 3);
                if (q1 != std::string::npos) {
                    size_t q2 = tag.find('"', q1 + 1);
                    if (q2 != std::string::npos)
                        result.push_back(tag.substr(q1+1, q2-q1-1));
                }
                break;
            }
        }
        pos = end;
    }
    return result;
}

std::vector<SvgImporter::Polyline> SvgImporter::parse_paths(const std::string& svg) {
    std::vector<Polyline> all;
    for (auto& d : extract_path_ds(svg))
        parse_path_d(d, all, 0.01f);
    return all;
}

static float next_float(const std::string& s, size_t& i) {
    while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == ',')) ++i;
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
    if (start == i) return 0.f;
    return std::stof(s.substr(start, i - start));
}

void SvgImporter::parse_path_d(const std::string& d, std::vector<Polyline>& out, float flatness) {
    Polyline current;
    float cx = 0.f, cy = 0.f;
    float sx = 0.f, sy = 0.f;
    char cmd = 'M';
    size_t i = 0;

    auto commit = [&]() {
        if (!current.empty()) {
            out.push_back(std::move(current));
            current.clear();
        }
    };

    while (i < d.size()) {
        while (i < d.size() && std::isspace((unsigned char)d[i])) ++i;
        if (i >= d.size()) break;

        char c = d[i];
        if (std::isalpha((unsigned char)c)) { cmd = c; ++i; continue; }

        bool rel = std::islower((unsigned char)cmd);
        char CMD = (char)std::toupper((unsigned char)cmd);

        if (CMD == 'M') {
            float x = next_float(d,i) + (rel ? cx : 0.f);
            float y = next_float(d,i) + (rel ? cy : 0.f);
            commit();
            cx = sx = x; cy = sy = y;
            current.push_back({cx,cy});
            cmd = rel ? 'l' : 'L';
        } else if (CMD == 'L') {
            float x = next_float(d,i) + (rel ? cx : 0.f);
            float y = next_float(d,i) + (rel ? cy : 0.f);
            current.push_back({cx=x, cy=y});
        } else if (CMD == 'H') {
            float x = next_float(d,i) + (rel ? cx : 0.f);
            current.push_back({cx=x, cy});
        } else if (CMD == 'V') {
            float y = next_float(d,i) + (rel ? cy : 0.f);
            current.push_back({cx, cy=y});
        } else if (CMD == 'C') {
            float x1=next_float(d,i)+(rel?cx:0), y1=next_float(d,i)+(rel?cy:0);
            float x2=next_float(d,i)+(rel?cx:0), y2=next_float(d,i)+(rel?cy:0);
            float  x=next_float(d,i)+(rel?cx:0),  y=next_float(d,i)+(rel?cy:0);
            flatten_bezier_cubic(cx,cy,x1,y1,x2,y2,x,y,flatness,current);
            cx=x; cy=y;
        } else if (CMD == 'S') {
            // Smooth cubic: implicit first control point = reflection of last C's 2nd cp
            float x2=next_float(d,i)+(rel?cx:0), y2=next_float(d,i)+(rel?cy:0);
            float  x=next_float(d,i)+(rel?cx:0),  y=next_float(d,i)+(rel?cy:0);
            // We use (cx,cy) as the reflected control point (simplified: no stored cp)
            flatten_bezier_cubic(cx,cy,cx,cy,x2,y2,x,y,flatness,current);
            cx=x; cy=y;
        } else if (CMD == 'Q') {
            float x1=next_float(d,i)+(rel?cx:0), y1=next_float(d,i)+(rel?cy:0);
            float  x=next_float(d,i)+(rel?cx:0),  y=next_float(d,i)+(rel?cy:0);
            flatten_bezier_quad(cx,cy,x1,y1,x,y,flatness,current);
            cx=x; cy=y;
        } else if (CMD == 'T') {
            // Smooth quadratic
            float x=next_float(d,i)+(rel?cx:0), y=next_float(d,i)+(rel?cy:0);
            flatten_bezier_quad(cx,cy,cx,cy,x,y,flatness,current);
            cx=x; cy=y;
        } else if (CMD == 'A') {
            float rx=next_float(d,i), ry=next_float(d,i);
            float x_rot=next_float(d,i);
            float large=next_float(d,i);
            float sweep=next_float(d,i);
            float ex=next_float(d,i)+(rel?cx:0), ey=next_float(d,i)+(rel?cy:0);
            flatten_arc(cx,cy,rx,ry,x_rot,large,sweep,ex,ey,flatness,current);
            cx=ex; cy=ey;
        } else if (CMD == 'Z') {
            if (!current.empty() && (current.back().first!=sx || current.back().second!=sy))
                current.push_back({sx,sy});
            commit(); cx=sx; cy=sy;
        } else {
            ++i;
        }
    }
    commit();
}

void SvgImporter::flatten_bezier_cubic(float x0,float y0,float x1,float y1,
                                        float x2,float y2,float x3,float y3,
                                        float tol, Polyline& out) {
    float dx = x0-x1*2+x2, dy = y0-y1*2+y2;
    float err = std::sqrt(dx*dx+dy*dy);
    if (err < tol) { out.push_back({x3,y3}); return; }
    float x01=(x0+x1)/2, y01=(y0+y1)/2, x12=(x1+x2)/2, y12=(y1+y2)/2;
    float x23=(x2+x3)/2, y23=(y2+y3)/2;
    float x012=(x01+x12)/2, y012=(y01+y12)/2;
    float x123=(x12+x23)/2, y123=(y12+y23)/2;
    float xm=(x012+x123)/2, ym=(y012+y123)/2;
    flatten_bezier_cubic(x0,y0,x01,y01,x012,y012,xm,ym,tol,out);
    flatten_bezier_cubic(xm,ym,x123,y123,x23,y23,x3,y3,tol,out);
}

void SvgImporter::flatten_bezier_quad(float x0,float y0,float x1,float y1,
                                       float x2,float y2, float tol, Polyline& out) {
    float err = std::abs(x1-(x0+x2)/2)+std::abs(y1-(y0+y2)/2);
    if (err < tol) { out.push_back({x2,y2}); return; }
    float x01=(x0+x1)/2, y01=(y0+y1)/2, x12=(x1+x2)/2, y12=(y1+y2)/2;
    float xm=(x01+x12)/2, ym=(y01+y12)/2;
    flatten_bezier_quad(x0,y0,x01,y01,xm,ym,tol,out);
    flatten_bezier_quad(xm,ym,x12,y12,x2,y2,tol,out);
}

void SvgImporter::flatten_arc(float x0, float y0, float rx, float ry,
                               float x_rot_deg, float large_arc, float sweep_flag,
                               float ex, float ey, float /*flatness*/, Polyline& out)
{
    // SVG arc to center-point parameterisation (spec appendix B.2.4)
    float phi     = x_rot_deg * kPi / 180.f;
    float cos_phi = std::cos(phi);
    float sin_phi = std::sin(phi);

    float dx2 = (x0 - ex) / 2.f, dy2 = (y0 - ey) / 2.f;
    float x1p =  cos_phi * dx2 + sin_phi * dy2;
    float y1p = -sin_phi * dx2 + cos_phi * dy2;

    float x1p2 = x1p * x1p, y1p2 = y1p * y1p;
    if (rx < 0.f) rx = -rx;
    if (ry < 0.f) ry = -ry;
    float rx2 = rx * rx, ry2 = ry * ry;
    float L = x1p2 / (rx2 > 0.f ? rx2 : 1e-9f) + y1p2 / (ry2 > 0.f ? ry2 : 1e-9f);
    if (L > 1.f) { rx *= std::sqrt(L); ry *= std::sqrt(L); rx2 = rx*rx; ry2 = ry*ry; }

    float num  = std::max(0.f, rx2*ry2 - rx2*y1p2 - ry2*x1p2);
    float den  = rx2*y1p2 + ry2*x1p2;
    float sq   = (den > 1e-12f) ? std::sqrt(num / den) : 0.f;
    float sign = ((int)large_arc == (int)sweep_flag) ? -1.f : 1.f;
    float cxp  =  sign * sq * rx * y1p / (ry > 1e-9f ? ry : 1e-9f);
    float cyp  = -sign * sq * ry * x1p / (rx > 1e-9f ? rx : 1e-9f);

    float acx = cos_phi * cxp - sin_phi * cyp + (x0 + ex) / 2.f;
    float acy = sin_phi * cxp + cos_phi * cyp + (y0 + ey) / 2.f;

    auto vec_angle = [](float ux, float uy, float vx, float vy) -> float {
        float dot = ux*vx + uy*vy;
        float len = std::sqrt((ux*ux+uy*uy)*(vx*vx+vy*vy));
        if (len < 1e-12f) return 0.f;
        float a = std::acos(std::clamp(dot/len, -1.f, 1.f));
        if (ux*vy - uy*vx < 0.f) a = -a;
        return a;
    };

    float theta1 = vec_angle(1.f, 0.f,
                              (x1p - cxp) / (rx > 1e-9f ? rx : 1e-9f),
                              (y1p - cyp) / (ry > 1e-9f ? ry : 1e-9f));
    float dtheta = vec_angle(
        (x1p - cxp) / (rx > 1e-9f ? rx : 1e-9f),
        (y1p - cyp) / (ry > 1e-9f ? ry : 1e-9f),
        (-x1p - cxp) / (rx > 1e-9f ? rx : 1e-9f),
        (-y1p - cyp) / (ry > 1e-9f ? ry : 1e-9f));

    static constexpr float k2Pi = 6.28318530f;
    if (!sweep_flag && dtheta > 0.f) dtheta -= k2Pi;
    if ( sweep_flag && dtheta < 0.f) dtheta += k2Pi;

    int segs = std::max(4, static_cast<int>(std::fabs(dtheta) / 0.15f));
    for (int i = 1; i <= segs; ++i)
    {
        float t = theta1 + dtheta * static_cast<float>(i) / segs;
        float px = cos_phi * rx * std::cos(t) - sin_phi * ry * std::sin(t) + acx;
        float py = sin_phi * rx * std::cos(t) + cos_phi * ry * std::sin(t) + acy;
        out.push_back({px, py});
    }
}

void SvgImporter::normalize_paths(std::vector<Polyline>& paths) {
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    for (auto& p : paths) for (auto& pt : p) {
        minx=std::min(minx,pt.first); miny=std::min(miny,pt.second);
        maxx=std::max(maxx,pt.first); maxy=std::max(maxy,pt.second);
    }
    float w=maxx-minx, h=maxy-miny;
    if (w<1e-6f||h<1e-6f) return;
    float scale=2.f/std::max(w,h);
    float ox=(minx+maxx)*.5f, oy=(miny+maxy)*.5f;
    for (auto& p : paths) for (auto& pt : p) {
        pt.first=(pt.first-ox)*scale;
        pt.second=-(pt.second-oy)*scale; // flip Y
    }
}

void SvgImporter::optimize_path_order(std::vector<Polyline>& paths) {
    if (paths.size()<2) return;
    std::vector<bool> used(paths.size(),false);
    std::vector<Polyline> ordered; ordered.reserve(paths.size());
    float cx=0.f,cy=0.f;
    for (size_t iter=0;iter<paths.size();++iter) {
        float best_d=1e18f; size_t best_i=0; bool best_rev=false;
        for (size_t j=0;j<paths.size();++j) {
            if (used[j]) continue;
            float df=(paths[j].front().first-cx)*(paths[j].front().first-cx)
                    +(paths[j].front().second-cy)*(paths[j].front().second-cy);
            float dr=(paths[j].back().first-cx)*(paths[j].back().first-cx)
                    +(paths[j].back().second-cy)*(paths[j].back().second-cy);
            if (df<best_d){best_d=df;best_i=j;best_rev=false;}
            if (dr<best_d){best_d=dr;best_i=j;best_rev=true;}
        }
        used[best_i]=true;
        if (best_rev) std::reverse(paths[best_i].begin(),paths[best_i].end());
        cx=paths[best_i].back().first; cy=paths[best_i].back().second;
        ordered.push_back(std::move(paths[best_i]));
    }
    paths=std::move(ordered);
}

PointBuffer SvgImporter::paths_to_points(const std::vector<Polyline>& paths, const Options& opt) {
    PointBuffer result;
    for (auto& path : paths) {
        if (path.empty()) continue;
        auto& fp=path.front();
        result.push_back(LaserPoint::from_norm(fp.first,fp.second,0,0,0,true));
        for (int a=0;a<opt.anchor_repeats;++a)
            result.push_back(LaserPoint::from_norm(fp.first,fp.second,255,255,255,false));
        float prev_x=fp.first,prev_y=fp.second;
        for (size_t i=1;i<path.size();++i) {
            float px=path[i].first,py=path[i].second;
            float seg=std::sqrt((px-prev_x)*(px-prev_x)+(py-prev_y)*(py-prev_y));
            int steps=std::max(1,(int)(seg/opt.point_spacing));
            for (int s=1;s<=steps;++s) {
                float t=(float)s/steps;
                result.push_back(LaserPoint::from_norm(
                    prev_x+(px-prev_x)*t, prev_y+(py-prev_y)*t, 255,255,255,false));
            }
            prev_x=px; prev_y=py;
        }
        for (int a=0;a<opt.anchor_repeats;++a)
            result.push_back(LaserPoint::from_norm(prev_x,prev_y,255,255,255,false));
    }
    return result;
}

} // namespace idhmfis
