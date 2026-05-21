// image_import.cpp — Raster-to-laser vectorizer implementation.
// Scanline approach: for each pixel row, emit lit runs of above-threshold pixels
// and blanked travel moves between runs.

// Windows headers must come before SDL3 to avoid macro conflicts
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <commdlg.h>
#endif

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <climits>
#include <vector>

// Pull in stb_image implementation (header-only, single translation unit).
// The copy in src/util/stb_image.h is SDL3's patched version which uses
// SDL types and macros — include SDL3 first so they are defined.
#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_LINEAR           // disable HDR path (SDL patched it out)
#define STBI_NO_STDIO            // disable FILE* functions — SDL patch broke them; use from_memory
// Silence warnings from the stb header under MSVC /WX
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4244)  // conversion from int to short
#  pragma warning(disable: 4100)  // unreferenced formal parameter
#  pragma warning(disable: 4456)  // declaration hides previous local
#  pragma warning(disable: 4457)  // declaration of 'x' hides function parameter
#  pragma warning(disable: 4505)  // unreferenced local function
#  pragma warning(disable: 4701)  // potentially uninitialized local variable
#  pragma warning(disable: 4703)  // potentially uninitialized local pointer variable
#endif
#include "../util/stb_image.h"
#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#include "image_import.h"
#include "imgui.h"

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  3×3 box blur — applied in-place on a grayscale (1-channel) image buffer.
// ─────────────────────────────────────────────────────────────────────────────
static void box_blur_3x3(std::vector<uint8_t>& buf, int w, int h)
{
    std::vector<uint8_t> tmp(buf.size());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int sum = 0, cnt = 0;
            for (int dy = -1; dy <= 1; ++dy) {
                int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    int nx = x + dx;
                    if (nx < 0 || nx >= w) continue;
                    sum += buf[ny * w + nx];
                    ++cnt;
                }
            }
            tmp[y * w + x] = static_cast<uint8_t>(sum / cnt);
        }
    }
    buf = std::move(tmp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  vectorize_image — public API
// ─────────────────────────────────────────────────────────────────────────────
PointBuffer vectorize_image(const std::string& path,
                             const ImageImportConfig& cfg,
                             std::string* error_out)
{
    PointBuffer result;

    // --- Load file into memory then decode via stbi_load_from_memory ----------
    // (STBI_NO_STDIO disables the filename-based loader because SDL's patched
    //  stb_image removes the FILE* functions; we read the file ourselves.)
    std::vector<uint8_t> file_buf;
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) {
            if (error_out) *error_out = "Cannot open file";
            return result;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsz <= 0) { fclose(f); if (error_out) *error_out = "Empty file"; return result; }
        file_buf.resize(static_cast<size_t>(fsz));
        fread(file_buf.data(), 1, file_buf.size(), f);
        fclose(f);
    }

    int w = 0, h = 0, channels_out = 0;
    stbi_uc* raw = stbi_load_from_memory(
        file_buf.data(), static_cast<int>(file_buf.size()),
        &w, &h, &channels_out, 1);
    (void)channels_out;
    if (!raw) {
        if (error_out)
            *error_out = "Failed to decode image (unsupported format or corrupt file)";
        return result;
    }

    // Copy into a managed buffer and free stb allocation
    std::vector<uint8_t> pixels(raw, raw + w * h);
    stbi_image_free(raw);

    // --- Optional blur -----------------------------------------------------
    if (cfg.blur_radius >= 1)
        box_blur_3x3(pixels, w, h);

    // --- Invert -----------------------------------------------------------
    if (cfg.invert) {
        for (auto& p : pixels)
            p = static_cast<uint8_t>(255 - p);
    }

    // --- Threshold and color ----------------------------------------------
    const uint8_t thresh_byte = static_cast<uint8_t>(
        std::clamp(cfg.threshold, 0.f, 1.f) * 255.f);

    const uint8_t cr = static_cast<uint8_t>(std::clamp(cfg.col_r, 0.f, 1.f) * 255.f);
    const uint8_t cg = static_cast<uint8_t>(std::clamp(cfg.col_g, 0.f, 1.f) * 255.f);
    const uint8_t cb = static_cast<uint8_t>(std::clamp(cfg.col_b, 0.f, 1.f) * 255.f);

    // --- Scanline vectorization -------------------------------------------
    // We iterate rows from top to bottom.  Within each row we collect contiguous
    // lit-pixel runs and emit:
    //   • a blanked travel point to the start of each run
    //   • lit points for each pixel in the run (decimated to stay within max_points)
    //
    // Normalised output space: image centre → (0,0), scale applied.
    // Y is flipped so the image appears right-side up (laser +Y = up).

    const float inv_w = (w > 1) ? 1.f / static_cast<float>(w - 1) : 1.f;
    const float inv_h = (h > 1) ? 1.f / static_cast<float>(h - 1) : 1.f;

    // First pass: count how many lit pixels there are so we can compute a
    // decimation step to stay within max_points.
    int lit_count = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (pixels[y * w + x] >= thresh_byte)
                ++lit_count;

    // Each run adds 1 blanked travel + N lit points.  A rough estimate:
    // reserve max_points * 1.25 slots to account for blanked points.
    int step = 1;
    if (lit_count > 0) {
        int effective_cap = static_cast<int>(static_cast<float>(cfg.max_points) * 0.9f);
        if (effective_cap < 1) effective_cap = 1;
        step = std::max(1, lit_count / effective_cap);
    }

    result.reserve(static_cast<size_t>(std::min(lit_count + h, cfg.max_points + h)));

    int pixel_counter = 0;  // used for step-based decimation

    for (int y = 0; y < h; ++y) {
        bool in_run = false;

        for (int x = 0; x <= w; ++x) {
            bool lit = (x < w) && (pixels[y * w + x] >= thresh_byte);

            if (lit && !in_run) {
                // Start of a new run — emit blanked travel to run start
                in_run = true;

                float nx =  (static_cast<float>(x) * inv_w - 0.5f) * 2.f * cfg.scale;
                float ny = -(static_cast<float>(y) * inv_h - 0.5f) * 2.f * cfg.scale;
                result.push_back(LaserPoint::from_norm(nx, ny, cr, cg, cb, /*blank=*/true));

            } else if (!lit && in_run) {
                in_run = false;
                // End of run — the last pixel of the run was at x-1 (already emitted below)
            }

            if (lit && in_run) {
                // Decimation: only emit every `step`-th lit pixel
                if (pixel_counter % step == 0) {
                    float nx =  (static_cast<float>(x) * inv_w - 0.5f) * 2.f * cfg.scale;
                    float ny = -(static_cast<float>(y) * inv_h - 0.5f) * 2.f * cfg.scale;
                    result.push_back(LaserPoint::from_norm(nx, ny, cr, cg, cb, /*blank=*/false));

                    // Hard cap: stop if we have hit the limit
                    if (static_cast<int>(result.size()) >= cfg.max_points) {
                        if (error_out) *error_out = "";
                        return result;
                    }
                }
                ++pixel_counter;
            }
        }
    }

    if (error_out) *error_out = "";
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
//  open_file_dialog — Windows GetOpenFileName wrapper
// ─────────────────────────────────────────────────────────────────────────────
static bool open_file_dialog(char* out_buf, int out_buf_len)
{
#ifdef _WIN32
    OPENFILENAMEA ofn{};
    out_buf[0] = '\0';
    ofn.lStructSize       = sizeof(ofn);
    ofn.hwndOwner         = nullptr;
    ofn.lpstrFilter       = "Image Files\0*.png;*.bmp;*.jpg;*.jpeg\0All Files\0*.*\0\0";
    ofn.lpstrFile         = out_buf;
    ofn.nMaxFile          = static_cast<DWORD>(out_buf_len);
    ofn.Flags             = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle        = "Import Image";
    return GetOpenFileNameA(&ofn) != 0;
#else
    (void)out_buf; (void)out_buf_len;
    return false;
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_image_import_panel
// ─────────────────────────────────────────────────────────────────────────────
bool draw_image_import_panel(ImageImportState& st)
{
    ImGui::SetNextWindowSize(ImVec2(320.f, 540.f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Import Image", &st.open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return false;
    }

    bool place_requested = false;

    // ── Browse button ────────────────────────────────────────────────────────
    if (ImGui::Button("Browse...")) {
        char tmp[512] = {};
        if (open_file_dialog(tmp, sizeof(tmp))) {
            std::strncpy(st.loaded_path, tmp, sizeof(st.loaded_path) - 1);
            st.loaded_path[sizeof(st.loaded_path) - 1] = '\0';
            st.dirty = true;
            st.error_msg.clear();
        }
    }
    ImGui::SameLine(0.f, 6.f);

    // Show truncated path
    {
        const char* p = st.loaded_path;
        size_t len = std::strlen(p);
        // Show last 34 chars so it fits in the panel
        if (len > 34)
            ImGui::TextDisabled("...%s", p + len - 34);
        else if (len > 0)
            ImGui::TextDisabled("%s", p);
        else
            ImGui::TextDisabled("(no file selected)");
    }

    ImGui::Separator();

    // ── Parameters ──────────────────────────────────────────────────────────
    bool changed = false;

    ImGui::SetNextItemWidth(200.f);
    changed |= ImGui::SliderFloat("Threshold##img", &st.cfg.threshold, 0.f, 1.f, "%.3f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Luminance threshold: pixels above this value are treated as lit");

    changed |= ImGui::Checkbox("Invert##img", &st.cfg.invert);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Invert luminance before thresholding (use for white-on-black images)");

    ImGui::SetNextItemWidth(200.f);
    changed |= ImGui::SliderFloat("Scale##img", &st.cfg.scale, 0.1f, 2.f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Output scale in normalised ±1 laser space");

    ImGui::SetNextItemWidth(200.f);
    {
        float col[3] = { st.cfg.col_r, st.cfg.col_g, st.cfg.col_b };
        if (ImGui::ColorEdit3("Color##img", col)) {
            st.cfg.col_r = col[0];
            st.cfg.col_g = col[1];
            st.cfg.col_b = col[2];
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Laser beam color for lit points");
    }

    ImGui::SetNextItemWidth(200.f);
    changed |= ImGui::SliderInt("Max Points##img", &st.cfg.max_points, 500, 8000);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hard cap on output point count");

    if (changed) st.dirty = true;

    ImGui::Separator();

    // ── Preview button ───────────────────────────────────────────────────────
    bool do_vectorize = ImGui::Button("Preview##img");
    if (st.dirty && std::strlen(st.loaded_path) > 0)
        do_vectorize = true;   // auto-preview once when path changes

    if (do_vectorize && std::strlen(st.loaded_path) > 0) {
        st.error_msg.clear();
        st.preview_pts = vectorize_image(st.loaded_path, st.cfg, &st.error_msg);
        st.preview_pt_count = static_cast<int>(st.preview_pts.size());
        st.dirty = false;
    }

    if (!st.error_msg.empty()) {
        ImGui::TextColored(ImVec4(1.f, 0.3f, 0.3f, 1.f), "Error: %s", st.error_msg.c_str());
    } else if (st.preview_pt_count > 0) {
        ImGui::Text("Points: %d", st.preview_pt_count);
    }

    // ── Mini canvas preview ───────────────────────────────────────────────────
    {
        ImVec2 canvas_sz(200.f, 200.f);
        ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Background
        dl->AddRectFilled(canvas_p0,
            ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
            IM_COL32(20, 20, 28, 255));
        dl->AddRect(canvas_p0,
            ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y),
            IM_COL32(60, 60, 80, 255));

        // Draw laser points
        if (!st.preview_pts.empty()) {
            const float cx = canvas_p0.x + canvas_sz.x * 0.5f;
            const float cy = canvas_p0.y + canvas_sz.y * 0.5f;
            const float half = canvas_sz.x * 0.5f * 0.92f;  // small inset

            // Only draw lit points as small dots; skip blanked travel points
            for (const auto& pt : st.preview_pts) {
                if (pt.blanked) continue;
                float px = cx + pt.nx() * half;
                float py = cy - pt.ny() * half;  // flip Y: laser +Y = up
                ImU32 col = IM_COL32(pt.r, pt.g, pt.b, 200);
                dl->AddCircleFilled(ImVec2(px, py), 1.2f, col, 4);
            }
        }

        // Advance cursor past the canvas
        ImGui::Dummy(canvas_sz);
    }

    ImGui::Separator();

    // ── Place in Frame button ─────────────────────────────────────────────────
    bool can_place = !st.preview_pts.empty() && st.error_msg.empty();
    if (!can_place) ImGui::BeginDisabled();
    if (ImGui::Button("Place in Frame##img", ImVec2(-1.f, 0.f))) {
        place_requested = true;
    }
    if (!can_place) ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && can_place)
        ImGui::SetTooltip("Add the vectorized image as a laser object in the frame editor");

    ImGui::End();
    return place_requested;
}

} // namespace idhmfis
