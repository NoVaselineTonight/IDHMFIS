#include "panel_3d_preview.h"
#include "ui_state.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <cmath>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct Vec3 {
    float x, y, z;
    Vec3() : x(0.f), y(0.f), z(0.f) {}
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3  operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3  operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3  operator*(float s)       const { return {x*s,   y*s,   z*s  }; }
    Vec3& operator+=(const Vec3& o)      { x+=o.x; y+=o.y; z+=o.z; return *this; }
    float dot(const Vec3& o)  const { return x*o.x + y*o.y + z*o.z; }
    Vec3  cross(const Vec3& o) const {
        return { y*o.z - z*o.y, z*o.x - x*o.z, x*o.y - y*o.x };
    }
    float len()        const { return std::sqrtf(x*x + y*y + z*z); }
    Vec3  normalized() const {
        float l = len();
        return (l > 1e-7f) ? Vec3(x/l, y/l, z/l) : Vec3(0,0,0);
    }
};

struct Camera {
    float azimuth   = 0.0f;  // 0 = facing straight on; +X (laser 1) appears left-most
    float elevation = 0.28f;
    float distance  = 20.f;
    Vec3  target    = {0.f, 2.5f, 10.f};
    float fov_deg   = 70.f;

    Vec3 position() const {
        float ce = std::cosf(elevation), se = std::sinf(elevation);
        float ca = std::cosf(azimuth),   sa = std::sinf(azimuth);
        return { target.x + distance * ce * sa,
                 target.y + distance * se,
                 target.z + distance * ce * ca };
    }
};

struct CamAxes { Vec3 right, up, forward, eye; };

static CamAxes build_axes(const Camera& cam) {
    Vec3 eye = cam.position();
    Vec3 fwd = (cam.target - eye).normalized();
    Vec3 world_up{0.f, 1.f, 0.f};
    Vec3 right = fwd.cross(world_up).normalized();
    Vec3 up    = right.cross(fwd).normalized();
    return {right, up, fwd, eye};
}

static bool project_point(Vec3 world, const CamAxes& axes, float fov_deg,
                           ImVec2 cpos, ImVec2 csz, ImVec2& out)
{
    Vec3 d = world - axes.eye;
    float cx = d.dot(axes.right);
    float cy = d.dot(axes.up);
    float cz = d.dot(axes.forward);
    if (cz < 0.05f) return false;
    float half_fov = (fov_deg * 3.14159265f / 180.f) * 0.5f;
    float f  = 1.f / std::tanf(half_fov);
    float asp = (csz.y > 0.f) ? (csz.x / csz.y) : 1.f;
    float sx = (cx / cz) * (f / asp);
    float sy = (cy / cz) * f;
    out.x = cpos.x + (0.5f + sx * 0.5f) * csz.x;
    out.y = cpos.y + (0.5f - sy * 0.5f) * csz.y;
    return true;
}

// Room dimensions — updated each frame from UIState::Preview3dSettings.
// Stored as module-level statics so all helper functions use the same values
// without threading them through every call signature.
static float s_rw   = 6.f;   // half-width  (±s_rw metres)
static float s_rh   = 5.f;   // full height (floor = 0, ceiling = s_rh)
static float s_rd   = 20.f;  // depth       (laser at z≈0, back wall at z≈s_rd)
static float s_ps   = 3.0f;  // projection half-scale (±s_ps in X and Y, square)

static Vec3 clamp_to_room(Vec3 p) {
    p.x = std::clamp(p.x, -s_rw, s_rw);
    p.y = std::clamp(p.y,  0.f,  s_rh);
    p.z = std::clamp(p.z,  0.f,  s_rd);
    return p;
}

static void draw_world_line(ImDrawList* dl, Vec3 a, Vec3 b,
                             const CamAxes& axes, float fov_deg,
                             ImVec2 cpos, ImVec2 csz, ImU32 col, float thick)
{
    ImVec2 sa, sb;
    if (project_point(a, axes, fov_deg, cpos, csz, sa) &&
        project_point(b, axes, fov_deg, cpos, csz, sb))
        dl->AddLine(sa, sb, col, thick);
}

static void draw_room(ImDrawList* dl, const CamAxes& axes, float fov_deg, ImVec2 cpos, ImVec2 csz)
{
    const ImU32 wc = IM_COL32(35, 48, 65, 150);
    Vec3 c[8] = {
        {-s_rw,0.f,0.f},{s_rw,0.f,0.f},{s_rw,s_rh,0.f},{-s_rw,s_rh,0.f},
        {-s_rw,0.f,s_rd},{s_rw,0.f,s_rd},{s_rw,s_rh,s_rd},{-s_rw,s_rh,s_rd}
    };
    draw_world_line(dl,c[0],c[1],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[1],c[2],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[2],c[3],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[3],c[0],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[4],c[5],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[5],c[6],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[6],c[7],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[7],c[4],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[0],c[4],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[1],c[5],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[2],c[6],axes,fov_deg,cpos,csz,wc,1.f);
    draw_world_line(dl,c[3],c[7],axes,fov_deg,cpos,csz,wc,1.f);
}

static void draw_floor_grid(ImDrawList* dl, const CamAxes& axes, float fov_deg, ImVec2 cpos, ImVec2 csz)
{
    const ImU32 gc = IM_COL32(22, 32, 45, 90);
    float grid_step = (s_rd > 30.f) ? 4.f : 2.f;
    for (float x = -s_rw; x <= s_rw + 0.01f; x += grid_step)
        draw_world_line(dl,{x,0.f,0.f},{x,0.f,s_rd},axes,fov_deg,cpos,csz,gc,0.7f);
    for (float z = 0.f; z <= s_rd + 0.01f; z += grid_step)
        draw_world_line(dl,{-s_rw,0.f,z},{s_rw,0.f,z},axes,fov_deg,cpos,csz,gc,0.7f);
}

// Draw a small laser unit icon: filled circle (lens) + outer ring.
static void draw_laser_icon(ImDrawList* dl, Vec3 pos,
                             const CamAxes& axes, float fov_deg,
                             ImVec2 cpos, ImVec2 csz)
{
    ImVec2 s;
    if (project_point(pos, axes, fov_deg, cpos, csz, s)) {
        dl->AddCircleFilled(s, 5.f, IM_COL32(255, 255, 255, 220));
        dl->AddCircle(s, 7.f, IM_COL32(200, 220, 255, 120), 12, 1.2f);
    }
}

// Legacy single-projector icon (fallback when no laser outputs are patched).
static void draw_projector(ImDrawList* dl, const CamAxes& axes, float fov_deg, ImVec2 cpos, ImVec2 csz)
{
    draw_laser_icon(dl, {0.f, 2.5f, 0.3f}, axes, fov_deg, cpos, csz);
}

// Apply yaw (Y-axis) and pitch (X-axis) rotation to a normalised ILDA direction vector.
// nx in [-1,1] maps to X; ny in [-1,1] maps to Y; depth = +Z.
static Vec3 apply_laser_rotation(float nx, float ny, float yaw, float pitch)
{
    float dx = nx;
    float dy = ny;
    float dz = 1.f;
    // Apply pitch (rotate around X axis)
    float ry = dy * std::cosf(pitch) - dz * std::sinf(pitch);
    float rz = dy * std::sinf(pitch) + dz * std::cosf(pitch);
    dy = ry; dz = rz;
    // Apply yaw (rotate around Y axis)
    float rx = dx * std::cosf(yaw) + dz * std::sinf(yaw);
    rz = -dx * std::sinf(yaw) + dz * std::cosf(yaw);
    dx = rx; dz = rz;
    return {dx, dy, dz};
}

// Compute wall-hit position for a single laser point from a given placement.
static Vec3 wall_hit(float nx, float ny,
                     float px, float py, float pz,
                     float yaw, float pitch)
{
    Vec3 dir = apply_laser_rotation(nx, ny, yaw, pitch);
    Vec3 origin{px, py, pz};
    float back_wall_z = s_rd - 0.2f;  // slightly in front of back wall
    float t = (back_wall_z - origin.z) / (dir.z > 0.001f ? dir.z : 0.001f);
    return clamp_to_room(origin + dir * t);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  draw_laser_beams_irl — IRL aerial mode for one laser
// ─────────────────────────────────────────────────────────────────────────────
namespace {

static void draw_laser_beams_irl(ImDrawList* dl,
                                  const idhmfis::PointBuffer& pts,
                                  float px, float py, float pz,
                                  float yaw, float pitch,
                                  float brt, float haze_a_f,
                                  float glow_r, float core_w,
                                  const CamAxes& axes, float fov_deg,
                                  ImVec2 canvas_pos, ImVec2 canvas_size)
{
    const int n = static_cast<int>(pts.size());

    Vec3 origin{px, py, pz};
    ImVec2 s_proj;
    bool proj_ok = project_point(origin, axes, fov_deg, canvas_pos, canvas_size, s_proj);

    // Pass 1: aerial haze — per-SEGMENT (not per-point) to avoid dot artefacts.
    // Each segment midpoint defines one hazy aerial beam from the projector.
    if (proj_ok) {
        for (int i = 0; i + 1 < n; ++i) {
            const auto& pa = pts[i];
            const auto& pb = pts[i + 1];
            if (pa.blanked || pb.blanked) continue;
            Vec3 wa = wall_hit(pa.nx(), pa.ny(), px, py, pz, yaw, pitch);
            Vec3 wb = wall_hit(pb.nx(), pb.ny(), px, py, pz, yaw, pitch);
            ImVec2 sa, sb;
            if (!project_point(wa, axes, fov_deg, canvas_pos, canvas_size, sa) ||
                !project_point(wb, axes, fov_deg, canvas_pos, canvas_size, sb))
                continue;
            // Aerial beam aims at midpoint of wall segment
            ImVec2 s_mid = ImVec2((sa.x + sb.x) * 0.5f, (sa.y + sb.y) * 0.5f);
            uint8_t r = static_cast<uint8_t>((pa.r + pb.r) / 2);
            uint8_t g = static_cast<uint8_t>((pa.g + pb.g) / 2);
            uint8_t b = static_cast<uint8_t>((pa.b + pb.b) / 2);
            uint8_t h1 = static_cast<uint8_t>(haze_a_f * 8.f);
            uint8_t h2 = static_cast<uint8_t>(haze_a_f * 18.f);
            dl->AddLine(s_proj, s_mid, IM_COL32(r, g, b, h1), glow_r * 1.2f);
            dl->AddLine(s_proj, s_mid, IM_COL32(r, g, b, h2), glow_r * 0.4f);
        }
    }

    // Pass 2: bright wall connections — primary visual (the actual laser pattern on wall).
    for (int i = 0; i + 1 < n; ++i) {
        const auto& pa = pts[i];
        const auto& pb = pts[i + 1];
        if (pa.blanked || pb.blanked) continue;
        Vec3 wa = wall_hit(pa.nx(), pa.ny(), px, py, pz, yaw, pitch);
        Vec3 wb = wall_hit(pb.nx(), pb.ny(), px, py, pz, yaw, pitch);
        ImVec2 sa, sb;
        if (!project_point(wa, axes, fov_deg, canvas_pos, canvas_size, sa) ||
            !project_point(wb, axes, fov_deg, canvas_pos, canvas_size, sb))
            continue;
        uint8_t wa_glow = static_cast<uint8_t>(std::clamp(brt * 55.f,  0.f, 255.f));
        uint8_t wa_core = static_cast<uint8_t>(std::clamp(brt * 210.f, 0.f, 255.f));
        uint8_t wa_hot  = static_cast<uint8_t>(std::clamp(brt * 160.f, 0.f, 255.f));
        dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, wa_glow), glow_r * 0.9f);
        dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, wa_core), core_w);
        dl->AddLine(sa, sb, IM_COL32(255,  255,  255,  wa_hot),  core_w * 0.3f);
    }
}

static void draw_laser_beams_scan(ImDrawList* dl,
                                   const idhmfis::PointBuffer& pts,
                                   float px, float py, float pz,
                                   float yaw, float pitch,
                                   float brt,
                                   double& scan_accum, double& last_t,
                                   float scan_speed, int trail_pct,
                                   const CamAxes& axes, float fov_deg,
                                   ImVec2 canvas_pos, ImVec2 canvas_size)
{
    const int n = static_cast<int>(pts.size());

    double now_t = ImGui::GetTime();
    if (last_t == 0.0) last_t = now_t;
    double frame_dt = now_t - last_t;
    last_t = now_t;

    double scans_per_sec = static_cast<double>(std::max(0.1f, scan_speed));
    scan_accum += frame_dt * scans_per_sec;
    scan_accum -= std::floor(scan_accum);

    int scan_head = (n > 0) ? static_cast<int>(scan_accum * n) % n : 0;
    int trail_len = (n > 0)
        ? std::max(4, n * std::clamp(trail_pct, 1, 100) / 100)
        : 0;

    Vec3 origin{px, py, pz};
    ImVec2 s_proj;
    bool proj_ok = project_point(origin, axes, fov_deg, canvas_pos, canvas_size, s_proj);

    // Pass 1: dim wall persistence
    for (int i = 0; i + 1 < n; ++i) {
        const auto& pa = pts[i];
        const auto& pb = pts[i + 1];
        if (pa.blanked || pb.blanked) continue;
        Vec3 wa = wall_hit(pa.nx(), pa.ny(), px, py, pz, yaw, pitch);
        Vec3 wb = wall_hit(pb.nx(), pb.ny(), px, py, pz, yaw, pitch);
        ImVec2 sa, sb;
        if (project_point(wa, axes, fov_deg, canvas_pos, canvas_size, sa) &&
            project_point(wb, axes, fov_deg, canvas_pos, canvas_size, sb))
        {
            uint8_t a1 = static_cast<uint8_t>(std::clamp(brt * 35.f, 0.f, 255.f));
            uint8_t a2 = static_cast<uint8_t>(std::clamp(brt * 85.f, 0.f, 255.f));
            dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, a1),  5.f);
            dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, a2),  1.2f);
        }
    }

    // Pass 2: scan trail
    if (proj_ok && trail_len > 0) {
        for (int ti = trail_len; ti >= 1; --ti) {
            int idx = (scan_head - ti + n) % n;
            const auto& pa = pts[idx];
            if (pa.blanked) continue;
            float age  = static_cast<float>(ti) / static_cast<float>(trail_len);
            float fade = (1.f - age) * (1.f - age);
            Vec3 wa = wall_hit(pa.nx(), pa.ny(), px, py, pz, yaw, pitch);
            ImVec2 s_wall;
            if (!project_point(wa, axes, fov_deg, canvas_pos, canvas_size, s_wall))
                continue;
            uint8_t ba = static_cast<uint8_t>(std::clamp(fade * brt * 55.f, 0.f, 255.f));
            dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, ba), 1.5f + fade * 2.f);
            uint8_t wa2 = static_cast<uint8_t>(std::clamp(fade * brt * 180.f, 0.f, 255.f));
            dl->AddCircleFilled(s_wall, 2.f + fade * 3.f, IM_COL32(pa.r, pa.g, pa.b, wa2));
        }
    }

    // Pass 3: current scan head
    if (proj_ok && n > 0) {
        const auto& pa = pts[scan_head];
        if (!pa.blanked) {
            Vec3 wa = wall_hit(pa.nx(), pa.ny(), px, py, pz, yaw, pitch);
            ImVec2 s_wall;
            if (project_point(wa, axes, fov_deg, canvas_pos, canvas_size, s_wall)) {
                uint8_t b1 = static_cast<uint8_t>(std::clamp(brt * 12.f, 0.f, 255.f));
                uint8_t b2 = static_cast<uint8_t>(std::clamp(brt * 28.f, 0.f, 255.f));
                uint8_t b3 = static_cast<uint8_t>(std::clamp(brt * 70.f, 0.f, 255.f));
                uint8_t b4 = static_cast<uint8_t>(std::clamp(brt * 180.f, 0.f, 255.f));
                dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b1), 9.f);
                dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b2), 5.f);
                dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b3), 2.5f);
                dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b4), 1.f);
                dl->AddCircleFilled(s_wall, 9.f,  IM_COL32(pa.r, pa.g, pa.b, static_cast<uint8_t>(brt * 50.f)));
                dl->AddCircleFilled(s_wall, 4.5f, IM_COL32(pa.r, pa.g, pa.b, static_cast<uint8_t>(brt * 160.f)));
                dl->AddCircleFilled(s_wall, 2.f,  IM_COL32(255, 255, 255, 230));
            }
        }
    }
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
//  Public entry point
// ─────────────────────────────────────────────────────────────────────────────
namespace idhmfis {

void panel_3d_preview(UIState& state, bool* p_open)
{
    bool began = false;
    try {
        ImGuiWindowFlags win_flags =
            ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoNav       |
            ImGuiWindowFlags_NoScrollWithMouse;

        // Provide a sensible first-use position/size so the window does not
        // spawn at (0,0) when no SetNextWindow* call was issued by the caller
        // (e.g. when open in Programmer view where it floats freely).
        ImGui::SetNextWindowPos(ImVec2(100.f, 100.f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(372.f, 320.f), ImGuiCond_FirstUseEver);

        began = true;
        bool visible = ImGui::Begin("3D Preview##3dp", p_open, win_flags);
        if (!visible) { ImGui::End(); began = false; return; }

        static Camera cam;

        ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size = ImGui::GetContentRegionAvail();
        if (canvas_size.x < 1.f) canvas_size.x = 1.f;
        if (canvas_size.y < 1.f) canvas_size.y = 1.f;

        ImGui::InvisibleButton("##3dp_canvas", canvas_size,
            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered    = ImGui::IsItemHovered();
        bool lmb_active = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool rmb_active = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right);

        if (hovered || lmb_active || rmb_active) {
            ImGuiIO& io = ImGui::GetIO();
            ImVec2 delta = io.MouseDelta;
            if (lmb_active && (delta.x != 0.f || delta.y != 0.f)) {
                cam.azimuth   -= delta.x * 0.007f;
                cam.elevation += delta.y * 0.007f;
                cam.elevation  = std::clamp(cam.elevation, -1.4f, 1.4f);
            }
            if (rmb_active && (delta.x != 0.f || delta.y != 0.f)) {
                cam.distance += delta.y * 0.05f;
                cam.distance  = std::clamp(cam.distance, 2.f, 60.f);
            }
            if (hovered && io.MouseWheel != 0.f) {
                cam.distance -= io.MouseWheel * 1.2f;
                cam.distance  = std::clamp(cam.distance, 2.f, 60.f);
            }
        }

        // Update room + projection statics from current settings.
        // These are read by all helper functions (draw_room, clamp_to_room, etc.).
        s_rw = std::max(1.f, state.preview_3d.room_half_width);
        s_rh = std::max(1.f, state.preview_3d.room_height);
        s_rd = std::max(5.f, state.preview_3d.room_depth);
        s_ps = std::max(0.1f, state.preview_3d.proj_scale);

        // Re-clamp camera target to new room bounds whenever room size changes.
        cam.target.x = std::clamp(cam.target.x, -s_rw, s_rw);
        cam.target.y = std::clamp(cam.target.y,  0.f,  s_rh);
        cam.target.z = std::clamp(cam.target.z,  0.f,  s_rd);

        CamAxes axes = build_axes(cam);
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(canvas_pos,
            ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
            IM_COL32(5, 6, 10, 255));

        draw_room(dl, axes, cam.fov_deg, canvas_pos, canvas_size);
        draw_floor_grid(dl, axes, cam.fov_deg, canvas_pos, canvas_size);

        const auto& p3d = state.preview_3d;
        const float brt = std::clamp(p3d.beam_brightness, 0.f, 2.f);

        // Determine whether we have any patched laser outputs with placements
        const bool has_laser_placements = !state.laser_placements.empty();

        if (!has_laser_placements) {
            // ── Fallback: single legacy projector at hardcoded origin ─────────
            draw_projector(dl, axes, cam.fov_deg, canvas_pos, canvas_size);

            const Vec3 proj_origin{0.f, 2.5f, 0.3f};
            const PointBuffer& pts = state.preview_points;
            const int n = static_cast<int>(pts.size());

            ImVec2 s_proj;
            bool proj_ok = project_point(proj_origin, axes, cam.fov_deg,
                                         canvas_pos, canvas_size, s_proj);

            if (!p3d.scan_mode) {
                const float glow_r   = std::clamp(p3d.wall_glow_px,  1.f, 20.f);
                const float core_w   = std::clamp(p3d.beam_width_px, 0.5f, 6.f);
                const float haze_a_f = std::clamp(p3d.haze_alpha * brt, 0.f, 1.f);

                // Pass 1: aerial haze — per-SEGMENT midpoints (avoids dot artefacts at individual
                // point positions that plagued the old per-point loop).
                if (proj_ok) {
                    for (int i = 0; i + 1 < n; ++i) {
                        const auto& pa = pts[i];
                        const auto& pb = pts[i + 1];
                        if (pa.blanked || pb.blanked) continue;
                        Vec3 wa = clamp_to_room({pa.nx() * s_ps, s_rh * 0.5f + pa.ny() * s_ps, s_rd - 0.2f});
                        Vec3 wb = clamp_to_room({pb.nx() * 5.1f, 2.5f + pb.ny() * 2.0f, 19.8f});
                        Vec3 w_mid = {(wa.x + wb.x) * 0.5f, (wa.y + wb.y) * 0.5f, (wa.z + wb.z) * 0.5f};
                        ImVec2 s_mid;
                        if (!project_point(w_mid, axes, cam.fov_deg, canvas_pos, canvas_size, s_mid))
                            continue;
                        uint8_t r = static_cast<uint8_t>((pa.r + pb.r) / 2);
                        uint8_t g = static_cast<uint8_t>((pa.g + pb.g) / 2);
                        uint8_t b = static_cast<uint8_t>((pa.b + pb.b) / 2);
                        uint8_t h1 = static_cast<uint8_t>(haze_a_f * 8.f);
                        uint8_t h2 = static_cast<uint8_t>(haze_a_f * 18.f);
                        dl->AddLine(s_proj, s_mid, IM_COL32(r, g, b, h1), glow_r * 1.2f);
                        dl->AddLine(s_proj, s_mid, IM_COL32(r, g, b, h2), glow_r * 0.4f);
                    }
                }
                // Pass 2: bright wall connections — primary visual (actual laser pattern on wall).
                for (int i = 0; i + 1 < n; ++i) {
                    const auto& pa = pts[i];
                    const auto& pb = pts[i + 1];
                    if (pa.blanked || pb.blanked) continue;
                    Vec3 wa = clamp_to_room({pa.nx() * s_ps, s_rh * 0.5f + pa.ny() * s_ps, s_rd - 0.2f});
                    Vec3 wb = clamp_to_room({pb.nx() * 5.1f, 2.5f + pb.ny() * 2.0f, 19.8f});
                    ImVec2 sa, sb;
                    if (!project_point(wa, axes, cam.fov_deg, canvas_pos, canvas_size, sa) ||
                        !project_point(wb, axes, cam.fov_deg, canvas_pos, canvas_size, sb))
                        continue;
                    uint8_t wa_glow = static_cast<uint8_t>(std::clamp(brt * 55.f,  0.f, 255.f));
                    uint8_t wa_core = static_cast<uint8_t>(std::clamp(brt * 210.f, 0.f, 255.f));
                    uint8_t wa_hot  = static_cast<uint8_t>(std::clamp(brt * 160.f, 0.f, 255.f));
                    dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, wa_glow), glow_r * 0.9f);
                    dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, wa_core), core_w);
                    dl->AddLine(sa, sb, IM_COL32(255,  255,  255,  wa_hot),  core_w * 0.3f);
                }
            } else {
                static double s_scan_accum = 0.0;
                static double s_last_t     = 0.0;
                double now_t = ImGui::GetTime();
                if (s_last_t == 0.0) s_last_t = now_t;
                double frame_dt = now_t - s_last_t;
                s_last_t = now_t;
                double scans_per_sec = static_cast<double>(std::max(0.1f, p3d.scan_speed));
                s_scan_accum += frame_dt * scans_per_sec;
                s_scan_accum -= std::floor(s_scan_accum);
                int scan_head = (n > 0) ? static_cast<int>(s_scan_accum * n) % n : 0;
                int trail_len = (n > 0)
                    ? std::max(4, n * std::clamp(p3d.trail_pct, 1, 100) / 100)
                    : 0;

                for (int i = 0; i + 1 < n; ++i) {
                    const auto& pa = pts[i];
                    const auto& pb = pts[i + 1];
                    if (pa.blanked || pb.blanked) continue;
                    Vec3 wa = clamp_to_room({pa.nx() * s_ps, s_rh * 0.5f + pa.ny() * s_ps, s_rd - 0.2f});
                    Vec3 wb = clamp_to_room({pb.nx() * 5.1f, 2.5f + pb.ny() * 2.0f, 19.8f});
                    ImVec2 sa, sb;
                    if (project_point(wa, axes, cam.fov_deg, canvas_pos, canvas_size, sa) &&
                        project_point(wb, axes, cam.fov_deg, canvas_pos, canvas_size, sb))
                    {
                        uint8_t a1 = static_cast<uint8_t>(std::clamp(brt * 35.f, 0.f, 255.f));
                        uint8_t a2 = static_cast<uint8_t>(std::clamp(brt * 85.f, 0.f, 255.f));
                        dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, a1),  5.f);
                        dl->AddLine(sa, sb, IM_COL32(pa.r, pa.g, pa.b, a2),  1.2f);
                    }
                }
                if (proj_ok && trail_len > 0) {
                    for (int ti = trail_len; ti >= 1; --ti) {
                        int idx = (scan_head - ti + n) % n;
                        const auto& pa = pts[idx];
                        if (pa.blanked) continue;
                        float age  = static_cast<float>(ti) / static_cast<float>(trail_len);
                        float fade = (1.f - age) * (1.f - age);
                        Vec3 wa = clamp_to_room({pa.nx() * s_ps, s_rh * 0.5f + pa.ny() * s_ps, s_rd - 0.2f});
                        ImVec2 s_wall;
                        if (!project_point(wa, axes, cam.fov_deg, canvas_pos, canvas_size, s_wall))
                            continue;
                        uint8_t ba = static_cast<uint8_t>(std::clamp(fade * brt * 55.f, 0.f, 255.f));
                        dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, ba), 1.5f + fade * 2.f);
                        uint8_t wa2 = static_cast<uint8_t>(std::clamp(fade * brt * 180.f, 0.f, 255.f));
                        dl->AddCircleFilled(s_wall, 2.f + fade * 3.f, IM_COL32(pa.r, pa.g, pa.b, wa2));
                    }
                }
                if (proj_ok && n > 0) {
                    const auto& pa = pts[scan_head];
                    if (!pa.blanked) {
                        Vec3 wa = clamp_to_room({pa.nx() * s_ps, s_rh * 0.5f + pa.ny() * s_ps, s_rd - 0.2f});
                        ImVec2 s_wall;
                        if (project_point(wa, axes, cam.fov_deg, canvas_pos, canvas_size, s_wall)) {
                            uint8_t b1 = static_cast<uint8_t>(std::clamp(brt * 12.f, 0.f, 255.f));
                            uint8_t b2 = static_cast<uint8_t>(std::clamp(brt * 28.f, 0.f, 255.f));
                            uint8_t b3 = static_cast<uint8_t>(std::clamp(brt * 70.f, 0.f, 255.f));
                            uint8_t b4 = static_cast<uint8_t>(std::clamp(brt * 180.f, 0.f, 255.f));
                            dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b1), 9.f);
                            dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b2), 5.f);
                            dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b3), 2.5f);
                            dl->AddLine(s_proj, s_wall, IM_COL32(pa.r, pa.g, pa.b, b4), 1.f);
                            dl->AddCircleFilled(s_wall, 9.f,  IM_COL32(pa.r, pa.g, pa.b, static_cast<uint8_t>(brt * 50.f)));
                            dl->AddCircleFilled(s_wall, 4.5f, IM_COL32(pa.r, pa.g, pa.b, static_cast<uint8_t>(brt * 160.f)));
                            dl->AddCircleFilled(s_wall, 2.f,  IM_COL32(255, 255, 255, 230));
                        }
                    }
                }
            }
        } else {
            // ── Multi-laser mode: one pass per placement ──────────────────────
            // Per-laser scan state: indexed by laser slot (up to kMaxLasers).
            static constexpr int kMaxLasers = 16;
            static double s_scan_accum[kMaxLasers] = {};
            static double s_last_t[kMaxLasers]     = {};

            const float glow_r   = std::clamp(p3d.wall_glow_px,  1.f, 20.f);
            const float core_w   = std::clamp(p3d.beam_width_px, 0.5f, 6.f);
            const float haze_a_f = std::clamp(p3d.haze_alpha * brt, 0.f, 1.f);

            int slot = 0;
            for (const auto& lp : state.laser_placements) {
                // Draw icon at laser position
                draw_laser_icon(dl, {lp.pos_x, lp.pos_y, lp.pos_z},
                                axes, cam.fov_deg, canvas_pos, canvas_size);

                // Find point buffer for this stream; fall back to preview_points
                const PointBuffer* pts_ptr = &state.preview_points;
                for (const auto& sp : state.stream_previews) {
                    if (sp.stream_id == lp.stream_id) {
                        pts_ptr = &sp.points;
                        break;
                    }
                }
                const PointBuffer& pts = *pts_ptr;

                if (!p3d.scan_mode) {
                    draw_laser_beams_irl(dl, pts,
                                         lp.pos_x, lp.pos_y, lp.pos_z,
                                         lp.yaw, lp.pitch,
                                         brt, haze_a_f, glow_r, core_w,
                                         axes, cam.fov_deg, canvas_pos, canvas_size);
                } else {
                    int s = slot < kMaxLasers ? slot : kMaxLasers - 1;
                    draw_laser_beams_scan(dl, pts,
                                          lp.pos_x, lp.pos_y, lp.pos_z,
                                          lp.yaw, lp.pitch,
                                          brt,
                                          s_scan_accum[s], s_last_t[s],
                                          p3d.scan_speed, p3d.trail_pct,
                                          axes, cam.fov_deg, canvas_pos, canvas_size);
                }
                ++slot;
            }
        }

        // Mode label
        {
            const char* label = p3d.scan_mode ? "Scan Mode | Drag to orbit | Scroll to zoom"
                                              : "IRL Mode | Drag to orbit | Scroll to zoom";
            ImVec2 lp{ canvas_pos.x + 6.f,
                       canvas_pos.y + canvas_size.y - ImGui::GetTextLineHeight() - 4.f };
            dl->AddText(lp, IM_COL32(55, 70, 90, 160), label);
        }

        ImGui::End();

    } catch (...) {
        if (began) ImGui::End();
    }
}

} // namespace idhmfis
