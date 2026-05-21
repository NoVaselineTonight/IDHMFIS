// cue_thumbnailer.cpp — background rendering of 128x128 cue thumbnails.
//
// Design:
//   - render_loop() sleeps until has_pending_ is set, then drains the pending
//     queue one request at a time.
//   - render_one() calls the generator at t=0.0 to get a static frame, then
//     rasterizes the point buffer into a CueThumbnail.
//   - rasterize() maps ILDA [-32767..32767] to pixel [0..127] and draws
//     small filled circles for each lit point.

#include "cue_thumbnailer.h"
#include "../generators/igenerator.h"
#include "../core/logger.h"

#include <cstring>
#include <algorithm>
#include <cmath>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
CueThumbnailer::CueThumbnailer() = default;

CueThumbnailer::~CueThumbnailer()
{
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop
// ─────────────────────────────────────────────────────────────────────────────
void CueThumbnailer::start()
{
    if (running_.load(std::memory_order_acquire)) return;
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&CueThumbnailer::render_loop, this);
}

void CueThumbnailer::stop()
{
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    has_pending_.store(true, std::memory_order_release); // wake the thread
    if (thread_.joinable()) thread_.join();
}

// ─────────────────────────────────────────────────────────────────────────────
//  request — enqueue a render request
// ─────────────────────────────────────────────────────────────────────────────
void CueThumbnailer::request(int cue_idx, const std::string& generator_name,
                              const GeneratorParams& params)
{
    if (cue_idx < 0) return;

    {
        std::lock_guard<std::mutex> lk(req_mtx_);

        // Replace any existing pending request for the same index.
        for (auto& r : pending_) {
            if (r.idx == cue_idx) {
                r.gen_name = generator_name;
                r.params   = params;
                has_pending_.store(true, std::memory_order_release);
                return;
            }
        }
        pending_.push_back({ cue_idx, generator_name, params });
    }
    has_pending_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
//  get — return current thumbnail (copy, safe from any thread)
// ─────────────────────────────────────────────────────────────────────────────
CueThumbnail CueThumbnailer::get(int cue_idx) const
{
    if (cue_idx < 0) return {};
    std::lock_guard<std::mutex> lk(thumbs_mtx_);
    if (cue_idx < static_cast<int>(thumbs_.size()))
        return thumbs_[static_cast<size_t>(cue_idx)];
    return {};
}

// ─────────────────────────────────────────────────────────────────────────────
//  render_loop — background thread main
// ─────────────────────────────────────────────────────────────────────────────
void CueThumbnailer::render_loop()
{
    while (running_.load(std::memory_order_acquire))
    {
        // Wait until work is available (poll with short sleep to avoid busy-wait).
        if (!has_pending_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Drain one request per iteration so the queue stays responsive.
        Request req{};
        {
            std::lock_guard<std::mutex> lk(req_mtx_);
            if (pending_.empty()) {
                has_pending_.store(false, std::memory_order_release);
                continue;
            }
            req = std::move(pending_.front());
            pending_.erase(pending_.begin());
            if (pending_.empty())
                has_pending_.store(false, std::memory_order_release);
        }

        if (!running_.load(std::memory_order_acquire)) break;
        render_one(req);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  render_one — generate + rasterize one thumbnail
// ─────────────────────────────────────────────────────────────────────────────
void CueThumbnailer::render_one(const Request& req)
{
    CueThumbnail thumb{};

    IGenerator* gen = find_generator(req.gen_name);
    if (!gen) {
        // Draw a simple grey "?" pattern: two diagonal stripes.
        // Fill background with dark grey.
        for (int i = 0; i < kThumbPixels; ++i) {
            int base = i * 4;
            thumb.pixels[static_cast<size_t>(base)]     = 40;
            thumb.pixels[static_cast<size_t>(base + 1)] = 40;
            thumb.pixels[static_cast<size_t>(base + 2)] = 40;
            thumb.pixels[static_cast<size_t>(base + 3)] = 255;
        }
        // Draw a light "?" shape via simple pixel writes.
        auto put = [&](int x, int y) {
            if (x < 0 || x >= kThumbW || y < 0 || y >= kThumbH) return;
            int base = (y * kThumbW + x) * 4;
            thumb.pixels[static_cast<size_t>(base)]     = 180;
            thumb.pixels[static_cast<size_t>(base + 1)] = 180;
            thumb.pixels[static_cast<size_t>(base + 2)] = 180;
            thumb.pixels[static_cast<size_t>(base + 3)] = 255;
        };
        // Rough "?" centered at (64,64)
        for (int dx = -8; dx <= 8; ++dx) put(64 + dx, 48);
        for (int dy = -6; dy <=  0; ++dy) put(72, 48 + dy + 6);
        for (int dx = -8; dx <= 0;  ++dx) put(64 + dx, 55);
        for (int dy =  0; dy <= 8;  ++dy) put(64, 58 + dy);
        put(64, 72); put(64, 73); put(65, 72); put(65, 73); // dot
    } else {
        // Generate a static frame with a modest point count.
        GeneratorParams p = req.params;
        p.point_count = 512;
        p.intensity   = 1.f;
        p.blanked     = false;

        PointBuffer pts = gen->generate(p, 0.0);
        rasterize(pts, thumb);
    }

    // Store under lock.
    {
        std::lock_guard<std::mutex> lk(thumbs_mtx_);
        if (req.idx >= static_cast<int>(thumbs_.size()))
            thumbs_.resize(static_cast<size_t>(req.idx + 1));

        uint64_t ver = thumbs_[static_cast<size_t>(req.idx)].version + 1u;
        thumb.version = ver;
        thumb.ready   = true;
        thumbs_[static_cast<size_t>(req.idx)] = std::move(thumb);
    }

    dirty_.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
//  rasterize — convert PointBuffer to RGBA pixel grid
// ─────────────────────────────────────────────────────────────────────────────
void CueThumbnailer::rasterize(const PointBuffer& pts, CueThumbnail& thumb)
{
    // Clear to black, opaque.
    for (int i = 0; i < kThumbPixels; ++i) {
        int base = i * 4;
        thumb.pixels[static_cast<size_t>(base)]     = 0;
        thumb.pixels[static_cast<size_t>(base + 1)] = 0;
        thumb.pixels[static_cast<size_t>(base + 2)] = 0;
        thumb.pixels[static_cast<size_t>(base + 3)] = 255;
    }

    auto set_pixel = [&](int px, int py,
                         uint8_t r, uint8_t g, uint8_t b) {
        if (px < 0 || px >= kThumbW || py < 0 || py >= kThumbH) return;
        int base = (py * kThumbW + px) * 4;
        // Additive blend: clamp to 255.
        auto add = [](uint8_t a, uint8_t v) -> uint8_t {
            return static_cast<uint8_t>(std::min(255, static_cast<int>(a) + static_cast<int>(v)));
        };
        thumb.pixels[static_cast<size_t>(base)]     = add(thumb.pixels[static_cast<size_t>(base)],     r);
        thumb.pixels[static_cast<size_t>(base + 1)] = add(thumb.pixels[static_cast<size_t>(base + 1)], g);
        thumb.pixels[static_cast<size_t>(base + 2)] = add(thumb.pixels[static_cast<size_t>(base + 2)], b);
        // alpha stays 255
    };

    static constexpr int kRadius = 2;

    for (const LaserPoint& pt : pts) {
        if (pt.blanked) continue;

        // Map ILDA [-32767..32767] → [0..127]
        // ILDA x increases right, y increases up; pixel y increases down.
        float nx = static_cast<float>(pt.x + 32767) / 65534.f; // 0..1
        float ny = static_cast<float>(32767 - pt.y) / 65534.f; // 0..1, flip y

        int cx = static_cast<int>(nx * static_cast<float>(kThumbW - 1) + 0.5f);
        int cy = static_cast<int>(ny * static_cast<float>(kThumbH - 1) + 0.5f);

        // Choose color: use point color if non-zero, else white.
        uint8_t pr = (pt.r > 0 || pt.g > 0 || pt.b > 0) ? pt.r : 255u;
        uint8_t pg = (pt.r > 0 || pt.g > 0 || pt.b > 0) ? pt.g : 255u;
        uint8_t pb = (pt.r > 0 || pt.g > 0 || pt.b > 0) ? pt.b : 255u;

        // Draw filled circle of radius kRadius.
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                if (dx * dx + dy * dy <= kRadius * kRadius)
                    set_pixel(cx + dx, cy + dy, pr, pg, pb);
            }
        }
    }
}

} // namespace idhmfis
