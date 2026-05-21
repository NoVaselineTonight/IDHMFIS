#pragma once
// ILDA file import. Supports format 0, 1, 4, 5.
#include "../core/types.h"
#include <string>
#include <vector>

namespace idhmfis {

class ILDASequence {
public:
    bool   load(const std::string& path);
    PointBuffer frame_at(double t, float fps = 30.0f) const;
    int    frame_count() const { return static_cast<int>(frames_.size()); }
    double duration(float fps = 30.f) const {
        return frames_.empty() ? 0.0 : frame_count() / fps;
    }
    std::string error() const { return error_; }
    bool is_loaded() const { return !frames_.empty(); }

private:
    struct ILDAFrame { PointBuffer points; std::string name; };
    std::vector<ILDAFrame> frames_;
    std::string error_;

    bool parse_ilda(std::ifstream& f);
    PointBuffer read_fmt_0(std::ifstream& f, int count);
    PointBuffer read_fmt_1(std::ifstream& f, int count);
    PointBuffer read_fmt_4(std::ifstream& f, int count);
    PointBuffer read_fmt_5(std::ifstream& f, int count);
};

} // namespace idhmfis
