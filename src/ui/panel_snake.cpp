// panel_snake.cpp — Floating Snake game panel

#include "layout.h"
#include "theme.h"
#include "imgui.h"

#include <vector>
#include <utility>
#include <cstdlib>
#include <ctime>
#include <cmath>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Game state (static singleton — only one Snake window)
// ─────────────────────────────────────────────────────────────────────────────
struct SnakeGame {
    std::vector<std::pair<int,int>> body;  // head at [0]
    std::pair<int,int> food = {5, 5};
    int dx = 1, dy = 0;
    int next_dx = 1, next_dy = 0;
    double last_tick    = 0.0;
    double tick_interval = 0.15;
    bool dead        = false;
    int  score       = 0;
    int  grid_w      = 20;
    int  grid_h      = 20;
    bool initialized = false;
};
static SnakeGame g;

static void snake_place_food()
{
    // Try up to 200 times to avoid body
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        int fx = std::rand() % g.grid_w;
        int fy = std::rand() % g.grid_h;
        bool on_body = false;
        for (auto& seg : g.body)
            if (seg.first == fx && seg.second == fy) { on_body = true; break; }
        if (!on_body) { g.food = {fx, fy}; return; }
    }
    // fallback: scan sequentially for the first free cell
    g.food = {0, 0};
    bool placed = false;
    for (int fy = 0; fy < g.grid_h && !placed; ++fy)
    {
        for (int fx = 0; fx < g.grid_w && !placed; ++fx)
        {
            bool on_body = false;
            for (auto& seg : g.body)
                if (seg.first == fx && seg.second == fy) { on_body = true; break; }
            if (!on_body) { g.food = {fx, fy}; placed = true; }
        }
    }
}

static void snake_init()
{
    g.body.clear();
    int cx = g.grid_w / 2;
    int cy = g.grid_h / 2;
    g.body.push_back({cx,     cy});
    g.body.push_back({cx - 1, cy});
    g.body.push_back({cx - 2, cy});
    g.dx = 1; g.dy = 0;
    g.next_dx = 1; g.next_dy = 0;
    g.dead = false;
    g.score = 0;
    g.tick_interval = 0.15;
    g.last_tick = ImGui::GetTime();
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    snake_place_food();
    g.initialized = true;
}

void panel_snake(UIState& /*state*/, LayoutContext& ctx)
{
    ImGui::SetNextWindowSize(ImVec2(400.f, 440.f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(200.f, 220.f), ImVec2(FLT_MAX, FLT_MAX));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(8, 10, 14, 255));

    if (!ImGui::Begin("Snake##snk",
                      &ctx.snake_open,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse))
    {
        ImGui::End();
        ImGui::PopStyleColor(); // WindowBg
        return;
    }

    if (!g.initialized)
        snake_init();

    bool focused = ImGui::IsWindowFocused();

    // ── Input ─────────────────────────────────────────────────────────────────
    if (focused && !g.dead)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)    && g.dy == 0)
            { g.next_dx =  0; g.next_dy = -1; }
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)  && g.dy == 0)
            { g.next_dx =  0; g.next_dy =  1; }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  && g.dx == 0)
            { g.next_dx = -1; g.next_dy =  0; }
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && g.dx == 0)
            { g.next_dx =  1; g.next_dy =  0; }
    }

    // Restart on Enter when dead
    if (focused && g.dead && ImGui::IsKeyPressed(ImGuiKey_Enter))
        snake_init();

    // ── Tick ──────────────────────────────────────────────────────────────────
    if (!g.dead)
    {
        double now = ImGui::GetTime();
        if (now - g.last_tick > g.tick_interval)
        {
            g.last_tick = now;
            g.dx = g.next_dx;
            g.dy = g.next_dy;

            std::pair<int,int> new_head = {
                g.body[0].first  + g.dx,
                g.body[0].second + g.dy
            };

            // Wall collision
            if (new_head.first  < 0 || new_head.first  >= g.grid_w ||
                new_head.second < 0 || new_head.second >= g.grid_h)
            {
                g.dead = true;
            }
            else
            {
                // Self collision (skip tail — it moves away)
                for (int i = 0; i < static_cast<int>(g.body.size()) - 1; ++i)
                {
                    if (g.body[static_cast<size_t>(i)] == new_head)
                    {
                        g.dead = true;
                        break;
                    }
                }
            }

            if (!g.dead)
            {
                bool ate = (new_head == g.food);
                g.body.insert(g.body.begin(), new_head);
                if (ate)
                {
                    ++g.score;
                    // Speed up slightly, floor at 50 ms
                    g.tick_interval = std::max(0.05, g.tick_interval - 0.005);
                    snake_place_food();
                }
                else
                {
                    g.body.pop_back();
                }
            }
        }
    }

    // ── Render ────────────────────────────────────────────────────────────────
    ImVec2 canvas_pos  = ImGui::GetCursorScreenPos();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    // Leave a small top margin for score text (drawn first, top-right)
    // and a small bottom margin
    canvas_size.y -= 22.f;
    if (canvas_size.x < 10.f) canvas_size.x = 10.f;
    if (canvas_size.y < 10.f) canvas_size.y = 10.f;

    float cell_w = canvas_size.x / static_cast<float>(g.grid_w);
    float cell_h = canvas_size.y / static_cast<float>(g.grid_h);
    float cell   = (cell_w < cell_h) ? cell_w : cell_h;

    // Actual grid size in pixels (may be smaller than canvas)
    float grid_px_w = cell * static_cast<float>(g.grid_w);
    float grid_px_h = cell * static_cast<float>(g.grid_h);
    // Centre grid in available space
    float ox = canvas_pos.x + (canvas_size.x - grid_px_w) * 0.5f;
    float oy = canvas_pos.y + (canvas_size.y - grid_px_h) * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Background fill
    dl->AddRectFilled({ox, oy}, {ox + grid_px_w, oy + grid_px_h},
                      IM_COL32(8, 10, 14, 255));

    // Subtle grid lines
    ImU32 grid_col = IM_COL32(20, 24, 30, 255);
    for (int gx = 0; gx <= g.grid_w; ++gx)
    {
        float px = ox + static_cast<float>(gx) * cell;
        dl->AddLine({px, oy}, {px, oy + grid_px_h}, grid_col);
    }
    for (int gy = 0; gy <= g.grid_h; ++gy)
    {
        float py = oy + static_cast<float>(gy) * cell;
        dl->AddLine({ox, py}, {ox + grid_px_w, py}, grid_col);
    }

    // Border: 2px rect around the game grid
    dl->AddRect({ox, oy}, {ox + grid_px_w, oy + grid_px_h},
                IM_COL32(40, 45, 55, 255), 0.f, 0, 2.f);

    // Food: filled circle with glow
    {
        float fx  = ox + (static_cast<float>(g.food.first)  + 0.5f) * cell;
        float fy  = oy + (static_cast<float>(g.food.second) + 0.5f) * cell;
        float r   = cell * 0.5f - 1.f;
        // Glow: 40% alpha, 2px larger radius
        dl->AddCircleFilled({fx, fy}, r + 2.f, IM_COL32(255, 80, 80, 102)); // 40% of 255 ≈ 102
        dl->AddCircleFilled({fx, fy}, r,       IM_COL32(255, 80, 80, 255));
    }

    // Snake body: gradient from bright head to darker tail
    {
        int body_sz = static_cast<int>(g.body.size());
        for (int si = body_sz - 1; si >= 0; --si)
        {
            auto& seg = g.body[static_cast<size_t>(si)];
            float sx = ox + static_cast<float>(seg.first)  * cell + 1.f;
            float sy = oy + static_cast<float>(seg.second) * cell + 1.f;

            // t=0 → tail, t=1 → head
            float t = (body_sz > 1)
                      ? 1.f - static_cast<float>(si) / static_cast<float>(body_sz - 1)
                      : 1.f;
            // Interpolate: tail IM_COL32(0,120,110,255) → head IM_COL32(0,230,210,255)
            int r_val = 0;
            int g_val = static_cast<int>(120.f + t * (230.f - 120.f) + 0.5f);
            int b_val = static_cast<int>(110.f + t * (210.f - 110.f) + 0.5f);
            int a_val = static_cast<int>(180.f + t * (255.f - 180.f) + 0.5f);
            ImU32 col = IM_COL32(r_val, g_val, b_val, a_val);
            dl->AddRectFilled({sx, sy}, {sx + cell - 2.f, sy + cell - 2.f}, col);
        }
    }

    // Reserve the canvas area so ImGui doesn't overlap it
    ImGui::Dummy(ImVec2(canvas_size.x, canvas_size.y));

    // Score — top-right corner style: draw after Dummy so it flows below grid
    {
        char score_buf[32];
        std::snprintf(score_buf, sizeof(score_buf), "Score: %d", g.score);

        ImGui::SetWindowFontScale(1.2f);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 200, 230, 255));
        // Right-align inside content width
        float score_w = ImGui::CalcTextSize(score_buf).x;
        float avail_w = ImGui::GetContentRegionAvail().x;
        if (avail_w > score_w)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail_w - score_w);
        ImGui::TextUnformatted(score_buf);
        ImGui::PopStyleColor(); // Text
        ImGui::SetWindowFontScale(1.0f);
    }

    // Game over overlay
    if (g.dead)
    {
        // Semi-transparent dark rect covering the entire grid
        dl->AddRectFilled(
            {ox, oy},
            {ox + grid_px_w, oy + grid_px_h},
            IM_COL32(8, 10, 14, 190));

        char over_buf[64];
        std::snprintf(over_buf, sizeof(over_buf),
                      "GAME OVER  Score: %d  [Enter] to restart", g.score);

        ImVec2 centre  = { ox + grid_px_w * 0.5f, oy + grid_px_h * 0.5f };
        ImVec2 txt_sz  = ImGui::CalcTextSize(over_buf);
        ImVec2 txt_pos = { centre.x - txt_sz.x * 0.5f, centre.y - txt_sz.y * 0.5f };

        // Drop shadow: draw text offset by 1px in dark color
        dl->AddText(
            ImVec2(txt_pos.x + 1.f, txt_pos.y + 1.f),
            IM_COL32(0, 0, 0, 200),
            over_buf);
        // Main text in red
        dl->AddText(txt_pos, IM_COL32(255, 80, 80, 255), over_buf);
    }

    ImGui::End();
    ImGui::PopStyleColor(); // WindowBg
}

} // namespace idhmfis
