// IDHMFIS — Application implementation
// Backend: SDL3 + Direct3D 12 on Windows / SDL3 + Vulkan on macOS
// Dear ImGui docking branch required.

#include "app.h"
#include "version.h"
#include "theme.h"
#include "layout.h"
#include "ui_state.h"
#include "keyboard_shortcuts.h"
#include "command_palette.h"
#include "widgets.h"

// Engine subsystems
#include "../core/logger.h"
#include "../core/render_bus.h"
#include "../core/show_engine.h"
#include "../dac/dac_manager.h"
#include "../audio/audio_analyzer.h"
#include "../audio/timeline_audio_player.h"
#include "../project/project.h"
#include "../project/cue.h"
#include "../project/ilda_pool.h"
#include "../project/ilda.h"
#include "../project/auto_save.h"
#include "../project/project_io.h"

#include <filesystem>

// ImGui core
#include "imgui.h"
#include "imgui_internal.h"

// SDL3 backend (always included)
#include "backends/imgui_impl_sdl3.h"

// Platform-specific backend selection
#if defined(_WIN32)
    #include "backends/imgui_impl_dx12.h"
    #include <d3d12.h>
    #include <dxgi1_6.h>
    #include <d3dcompiler.h>
    #include <shellapi.h>
    #include <commdlg.h>
    #include <direct.h>
    #pragma comment(lib, "d3d12")
    #pragma comment(lib, "dxgi")
    #pragma comment(lib, "d3dcompiler")
    #pragma comment(lib, "shell32")
    #pragma comment(lib, "comdlg32")
    #pragma comment(lib, "gdi32")
#else
    // macOS / Linux — use Vulkan
    #include "backends/imgui_impl_vulkan.h"
    #include <vulkan/vulkan.h>
#endif

#include <SDL3/SDL.h>

#include <cstdio>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <string>
#include <memory>
#include <thread>
#include <unordered_map>
#include <fstream>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  File dialog helpers (Windows only)
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32)
static std::string win32_get_exe_dir() {
    char exe_path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
    std::string p(exe_path);
    auto sep = p.find_last_of("\\/");
    return (sep != std::string::npos) ? p.substr(0, sep) : p;
}

// Returns the Showfiles directory. Prefers the top-level Showfiles folder
// (two levels above build\bin\) so it appears tidy in the project root.
// Falls back to a Showfiles folder adjacent to the executable if the
// resolved path does not exist.
static std::string win32_get_showfiles_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path exe_dir(win32_get_exe_dir());
    // Walk up: bin -> build -> project root, then into Showfiles
    fs::path candidate = fs::weakly_canonical(exe_dir / ".." / ".." / "Showfiles", ec);
    if (!ec && !candidate.empty()) {
        fs::create_directories(candidate, ec);   // ensure it exists
        return candidate.string();
    }
    fs::path fallback = fs::path(exe_dir) / "Showfiles";
    fs::create_directories(fallback, ec);        // ensure fallback exists too
    return fallback.string();
}

static std::string win32_open_file_dialog(HWND parent) {
    char buf[MAX_PATH]{};
    OPENFILENAMEA ofn{};
    std::string sf = win32_get_showfiles_dir();
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFilter = "IDHMFIS Show\0*.idhmfis\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrInitialDir = sf.c_str();
    ofn.lpstrDefExt = "idhmfis";
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) ? std::string(buf) : std::string{};
}

static std::string win32_save_file_dialog(HWND parent, const std::string& default_name) {
    char buf[MAX_PATH]{};
    if (!default_name.empty())
        std::strncpy(buf, default_name.c_str(), MAX_PATH - 1);
    OPENFILENAMEA ofn{};
    std::string sf = win32_get_showfiles_dir();
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = parent;
    ofn.lpstrFilter = "IDHMFIS Show\0*.idhmfis\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrInitialDir = sf.c_str();
    ofn.lpstrDefExt = "idhmfis";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    return GetSaveFileNameA(&ofn) ? std::string(buf) : std::string{};
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  App settings persistence (settings.json next to the executable)
//  Currently stores: developer_logging (bool).
// ─────────────────────────────────────────────────────────────────────────────
static std::string get_settings_json_path() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    fs::path exe_dir(win32_get_exe_dir());
#else
    fs::path exe_dir = fs::current_path();
#endif
    return (exe_dir / "settings.json").string();
}

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4505)
#endif
static std::string get_dev_log_path() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    fs::path exe_dir(win32_get_exe_dir());
#else
    fs::path exe_dir = fs::current_path();
#endif
    return (exe_dir / "Logs" / "session.log").string();
}

// Returns true if developer_logging was set to true in settings.json.
// Any parse failure is silently ignored — default is false.
static bool load_developer_logging_setting() {
    std::string path = get_settings_json_path();
    std::ifstream f(path);
    if (!f.is_open()) return false;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("developer_logging") && j["developer_logging"].is_boolean())
            return j["developer_logging"].get<bool>();
    } catch (...) {}
    return false;
}

static void save_developer_logging_setting(bool enabled) {
    std::string path = get_settings_json_path();
    nlohmann::json j;
    // Preserve any existing keys
    {
        std::ifstream f(path);
        if (f.is_open()) {
            try { f >> j; } catch (...) { j = nlohmann::json::object(); }
        }
    }
    j["developer_logging"] = enabled;
    try {
        std::ofstream f(path, std::ios::trunc);
        f << j.dump(4) << "\n";
    } catch (...) {}
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int kTargetFPS        = 120;
static constexpr int kInitialWidth     = 1600;
static constexpr int kInitialHeight    = 900;
static constexpr int kDX12BackbufCount = 3;

// ─────────────────────────────────────────────────────────────────────────────
//  Windows / DX12 helpers
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_WIN32)

struct DX12State {
    ID3D12Device*              device            = nullptr;
    ID3D12DescriptorHeap*      rtv_heap          = nullptr;
    ID3D12DescriptorHeap*      srv_heap          = nullptr;
    ID3D12CommandQueue*        cmd_queue         = nullptr;
    ID3D12GraphicsCommandList* cmd_list          = nullptr;
    ID3D12Fence*               fence             = nullptr;
    UINT64                     fence_vals[kDX12BackbufCount]{};
    HANDLE                     fence_event       = nullptr;
    IDXGISwapChain4*           swapchain         = nullptr;
    ID3D12Resource*            backbufs[kDX12BackbufCount]{};
    ID3D12CommandAllocator*    cmd_allocs[kDX12BackbufCount]{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handles[kDX12BackbufCount]{};
    UINT                       frame_index       = 0;
    UINT                       rtv_descriptor_size = 0;
    bool                       valid             = false;
};

static bool dx12_create_device(HWND hwnd, int width, int height, DX12State& dx) {
#ifdef _DEBUG
    {
        ID3D12Debug* debug_ctrl = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_ctrl)))) {
            debug_ctrl->EnableDebugLayer();
            debug_ctrl->Release();
        }
    }
#endif

    // Factory
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

    // Adapter
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i,
            DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); adapter = nullptr; continue; }
        if (SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
            break;
        adapter->Release(); adapter = nullptr;
    }

    if (!adapter) {
        // Fallback to WARP
        factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    }

    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dx.device)))) {
        adapter->Release(); factory->Release(); return false;
    }
    adapter->Release();

    // Command queue
    D3D12_COMMAND_QUEUE_DESC cq_desc{};
    cq_desc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    cq_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(dx.device->CreateCommandQueue(&cq_desc, IID_PPV_ARGS(&dx.cmd_queue)))) return false;

    // RTV descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = kDX12BackbufCount;
    if (FAILED(dx.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&dx.rtv_heap)))) return false;
    dx.rtv_descriptor_size = dx.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRV descriptor heap (for ImGui textures)
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
    srv_heap_desc.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 64;
    srv_heap_desc.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dx.device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&dx.srv_heap)))) return false;

    // Command allocators + lists
    for (int i = 0; i < kDX12BackbufCount; ++i) {
        if (FAILED(dx.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                   IID_PPV_ARGS(&dx.cmd_allocs[i])))) return false;
    }
    if (FAILED(dx.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
              dx.cmd_allocs[0], nullptr, IID_PPV_ARGS(&dx.cmd_list)))) return false;
    dx.cmd_list->Close();

    // Fence
    if (FAILED(dx.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dx.fence)))) return false;
    dx.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!dx.fence_event) return false;

    // Swap chain
    DXGI_SWAP_CHAIN_DESC1 sc_desc{};
    sc_desc.BufferCount  = kDX12BackbufCount;
    sc_desc.Width        = (UINT)width;
    sc_desc.Height       = (UINT)height;
    sc_desc.Format       = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.SwapEffect   = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.SampleDesc   = { 1, 0 };

    IDXGISwapChain1* sc1 = nullptr;
    if (FAILED(factory->CreateSwapChainForHwnd(dx.cmd_queue, hwnd, &sc_desc,
               nullptr, nullptr, &sc1))) { factory->Release(); return false; }
    sc1->QueryInterface(IID_PPV_ARGS(&dx.swapchain));
    sc1->Release();
    factory->Release();

    // Create RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
        dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < (UINT)kDX12BackbufCount; ++i) {
        dx.rtv_handles[i] = rtv_handle;
        dx.swapchain->GetBuffer(i, IID_PPV_ARGS(&dx.backbufs[i]));
        dx.device->CreateRenderTargetView(dx.backbufs[i], nullptr, rtv_handle);
        rtv_handle.ptr += dx.rtv_descriptor_size;
    }

    dx.frame_index = dx.swapchain->GetCurrentBackBufferIndex();
    dx.valid = true;
    return true;
}

static void dx12_wait_for_frame(DX12State& dx, UINT frame_idx) {
    if (dx.fence->GetCompletedValue() < dx.fence_vals[frame_idx]) {
        dx.fence->SetEventOnCompletion(dx.fence_vals[frame_idx], dx.fence_event);
        WaitForSingleObject(dx.fence_event, INFINITE);
    }
}

static void dx12_wait_all(DX12State& dx) {
    for (int i = 0; i < kDX12BackbufCount; ++i) dx12_wait_for_frame(dx, i);
}

static void dx12_resize(DX12State& dx, int width, int height) {
    dx12_wait_all(dx);
    for (int i = 0; i < kDX12BackbufCount; ++i) {
        if (dx.backbufs[i]) { dx.backbufs[i]->Release(); dx.backbufs[i] = nullptr; }
        dx.fence_vals[i] = 0;
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    dx.swapchain->GetDesc1(&desc);
    dx.swapchain->ResizeBuffers((UINT)kDX12BackbufCount, (UINT)width, (UINT)height,
                                 desc.Format, 0);
    dx.frame_index = dx.swapchain->GetCurrentBackBufferIndex();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle =
        dx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < (UINT)kDX12BackbufCount; ++i) {
        dx.rtv_handles[i] = rtv_handle;
        dx.swapchain->GetBuffer(i, IID_PPV_ARGS(&dx.backbufs[i]));
        dx.device->CreateRenderTargetView(dx.backbufs[i], nullptr, rtv_handle);
        rtv_handle.ptr += dx.rtv_descriptor_size;
    }
}

static void dx12_cleanup(DX12State& dx) {
    if (!dx.valid) return;
    dx12_wait_all(dx);
    for (int i = 0; i < kDX12BackbufCount; ++i) {
        if (dx.backbufs[i])  dx.backbufs[i]->Release();
        if (dx.cmd_allocs[i]) dx.cmd_allocs[i]->Release();
    }
    if (dx.cmd_list)   dx.cmd_list->Release();
    if (dx.fence)      dx.fence->Release();
    if (dx.swapchain)  dx.swapchain->Release();
    if (dx.srv_heap)   dx.srv_heap->Release();
    if (dx.rtv_heap)   dx.rtv_heap->Release();
    if (dx.cmd_queue)  dx.cmd_queue->Release();
    if (dx.device)     dx.device->Release();
    if (dx.fence_event) CloseHandle(dx.fence_event);
    dx.valid = false;
}

#endif // _WIN32

// ─────────────────────────────────────────────────────────────────────────────
//  Application::Impl
// ─────────────────────────────────────────────────────────────────────────────
struct Application::Impl {
    // SDL window + renderer handles
    SDL_Window*   window       = nullptr;
    SDL_Window*   hdmi_window_ = nullptr;   // optional borderless output window (HDMI)

#if defined(_WIN32)
    DX12State     dx12;
    static constexpr ImVec4 kClearColor = { 0.05f, 0.06f, 0.07f, 1.f };
#else
    VkInstance          vk_instance        = VK_NULL_HANDLE;
    VkPhysicalDevice    vk_physical_dev    = VK_NULL_HANDLE;
    VkDevice            vk_device          = VK_NULL_HANDLE;
    VkQueue             vk_queue           = VK_NULL_HANDLE;
    VkDescriptorPool    vk_desc_pool       = VK_NULL_HANDLE;
    VkRenderPass        vk_render_pass     = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window vk_wd{};
    int                 vk_queue_family    = -1;
    static constexpr ImVec4 kClearColor = { 0.05f, 0.06f, 0.07f, 1.f };
#endif

    // ── Engine subsystems (owned here, lifetime == Application) ───────────────
    RenderBus                   render_bus;
    AudioAnalyzer               audio;
    std::unique_ptr<ShowEngine> engine;      // constructed after render_bus+audio
    std::unique_ptr<EngineWatchdog> watchdog;
    std::shared_ptr<Project>    project;
    IldaPool                    ilda_pool;

    // Uniform output maps — keyed by patched output id.
    // Laser outputs: one DacManager per id, started on a background thread.
    // HDMI outputs:  one borderless SDL_Window per id.
    // NDI outputs:   handled entirely inside the engine (no extra UI-side object needed).
    std::unordered_map<int, std::unique_ptr<DacManager>> laser_managers_;
    std::unordered_map<int, SDL_Window*>                  hdmi_windows_;

    // ── UI subsystems ─────────────────────────────────────────────────────────
    UIState          state;
    LayoutContext    layout_ctx;
    LayoutCallbacks  layout_cbs;
    CommandPalette   palette;
    ShortcutRegistry shortcuts;

    // ── Cue thumbnail background renderer ────────────────────────────────────
    CueThumbnailer   thumbnailer;
    AutoSave         auto_save_{ 30 };   // default 30s, configurable in Setup > General

    bool             running               = false;
    bool             want_fullscreen       = false;
    float            target_frame_ms       = 1000.f / kTargetFPS;
    bool             safety_eula_accepted  = false;
    bool             safety_eula_checkbox  = false;
    bool             quit_requested        = false;  // set on SDL_EVENT_QUIT, shows confirm dialog
    bool             show_startup_dialog   = true;   // true = show startup dialog on first frame

    // ── Loading screen state ──────────────────────────────────────────────────
    std::string      loading_status_       = "Starting...";
    float            loading_progress_     = 0.f;

    // ── Show-file loading overlay ─────────────────────────────────────────────
    // When a show file is to be loaded, store the path here and set
    // is_loading_show = true.  run_frame() renders the overlay for one full
    // frame (presented to the screen), then on the NEXT run_frame() call it
    // performs the actual blocking load via do_load_project() and clears both
    // fields.  This ensures the user sees the overlay before the OS-level
    // device-probe work (including any netsh/firewall calls) begins.
    bool             is_loading_show       = false;
    std::string      pending_load_path_;

    // ── Output patch applying overlay ─────────────────────────────────────────
    // Counts how many DacManager::stop() calls are running on background threads.
    // run_frame() shows a blocking overlay while this is > 0.
    std::atomic<int> dacs_stopping_{ 0 };

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool init();
    void pump_loading_frame();
    void run_frame();
    void shutdown();

    // ── Engine bridge ─────────────────────────────────────────────────────────
    static std::shared_ptr<Project> build_demo_project();
    void sync_state_from_engine();       // called each frame: snapshot -> UIState
    void do_load_project(const std::string& path); // blocking project load + state restore
    void do_new_project();                          // reset all state to a fresh empty project

    // ── Callback wiring ───────────────────────────────────────────────────────
    void wire_callbacks();
    void wire_shortcuts();
};

// ─────────────────────────────────────────────────────────────────────────────
//  build_demo_project — real Project with 8 cues the engine can actually run
// ─────────────────────────────────────────────────────────────────────────────
std::shared_ptr<Project> Application::Impl::build_demo_project() {
    auto proj = std::make_shared<Project>();
    proj->name        = "Demo Show";
    proj->point_rate  = kDefaultPointRate;
    proj->ndi_width   = kDefaultNDIWidth;
    proj->ndi_height  = kDefaultNDIHeight;
    proj->ndi_fps     = kDefaultNDIFPS;
    proj->beam_thickness = 2.0f;
    proj->bloom_radius   = 8.0f;
    proj->haze_density   = 0.35f;
    proj->exposure        = 1.0f;

    struct DemoCueDef {
        const char*   name;
        GeneratorType gen;
        uint32_t      color;   // RGBA
        float         speed;
        float         scale;
        float         duration;
    };

    static const DemoCueDef defs[] = {
        { "Starburst",    GeneratorType::Starburst,      0xFF44EEFFU, 0.8f, 1.0f, 6.0f },
        { "Lissajous",    GeneratorType::Lissajous,      0xFFFF6633U, 1.2f, 0.9f, 8.0f },
        { "Spiral Waves", GeneratorType::Waves,          0xFF22EE77U, 1.0f, 1.0f, 8.0f },
        { "Box Grid",     GeneratorType::Grid,           0xFFC42BF5U, 0.5f, 1.0f, 6.0f },
        { "Laser Text",   GeneratorType::TextScroller,   0xFFFFAA00U, 0.6f, 0.8f, 10.0f },
        { "Beam Fan",     GeneratorType::FanSweep,       0xFF00EEFFU, 1.4f, 1.0f, 6.0f },
        { "Tunnel Rush",  GeneratorType::Tunnel,         0xFFEE2255U, 1.5f, 1.0f, 8.0f },
        { "Spirograph",   GeneratorType::Spirograph,     0xFF00E5FFU, 0.7f, 1.0f, 8.0f },
    };

    double timeline_t = 0.0;
    for (const auto& d : defs) {
        Cue c;
        c.id        = Project::new_id();
        c.name      = d.name;
        c.generator = d.gen;
        c.color_tag = d.color;
        c.duration  = d.duration;
        c.loop      = true;

        c.params.speed       = d.speed;
        c.params.scale       = d.scale;
        c.params.density     = 0.6f;
        c.params.intensity   = 1.0f;
        c.params.point_count = kDefaultPointRate;
        c.params.color_a     = { 0.f, 0.9f, 1.f, 1.f };
        c.params.color_b     = { 1.f, 0.3f, 0.6f, 1.f };

        // Decode color tag for color_a
        float r = ((d.color >> 24) & 0xFF) / 255.f;
        float g = ((d.color >> 16) & 0xFF) / 255.f;
        float b = ((d.color >>  8) & 0xFF) / 255.f;
        c.params.color_a = { r, g, b, 1.f };

        CueListEntry entry;
        entry.cue_id   = c.id;
        entry.in_time  = timeline_t;
        entry.out_time = timeline_t + d.duration;
        entry.fade_in  = 0.5f;
        entry.fade_out = 0.5f;

        proj->cues.push_back(std::move(c));
        proj->cue_list.push_back(entry);
        timeline_t += d.duration;
    }

    // Add default DMX patch
    proj->dmx_patches.push_back({ 0, 1, DmxFixtureProfile::make_default_40ch() });

    // Seed default outputs so the STREAMS grid is populated on a new show.
    {
        OutputStreamConfig laser;
        laser.id      = 0;
        laser.name    = "Laser 1";
        laser.type    = OutputStreamType::Laser;
        laser.enabled = true;
        proj->output_patch.push_back(laser);

        OutputStreamConfig ndi;
        ndi.id      = 1;
        ndi.name    = "NDI";
        ndi.type    = OutputStreamType::NDI;
        ndi.enabled = true;
        proj->output_patch.push_back(ndi);

        proj->output_patch_next_id = 2;
    }

    return proj;
}

// ─────────────────────────────────────────────────────────────────────────────
//  sync_state_from_engine — consume engine snapshot into UIState each frame
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::sync_state_from_engine() {
    if (!engine) return;
    EngineSnapshot snap = engine->snapshot();

    state.playing        = snap.playing;
    state.paused         = !snap.playing && snap.time > 0.0;
    state.playhead_s     = snap.time;
    state.loop_end_s     = snap.loop_end;
    state.engine_fps     = snap.engine_fps;
    state.bpm            = snap.bpm;
    // master_intensity is UI-owned (slider writes it, we poll changes in run_frame)
    state.active_cue_idx = snap.active_cue;
    state.audio          = snap.audio;
    state.dmx_uni_0      = snap.dmx[0];
    state.dmx_uni_1      = snap.dmx[1];

    // Copy preview points from engine — always overwrite (including empty)
    // so that STOP/CLEAR immediately clears the UI preview panel.
    state.preview_points = snap.preview_points;

    // Copy playback snapshot array
    state.snap.playback_count = snap.playback_count;
    for (int pi = 0; pi < UIState::kMaxPlaybacks; ++pi) {
        if (pi < snap.playback_count) {
            const auto& src = snap.playbacks[pi];
            auto& dst = state.snap.playbacks[pi];
            dst.id               = src.id;
            dst.name             = src.name;
            dst.active           = src.active;
            dst.blind            = src.blind;
            dst.intensity        = src.intensity;
            dst.cue_count        = src.cue_count;
            dst.current_cue      = src.current_cue;
            dst.current_cue_name = src.current_cue_name;
            dst.fade_level       = src.fade_level;
            dst.fade_in_dur      = src.fade_in_dur;
            dst.fade_elapsed     = src.fade_elapsed;
        } else {
            state.snap.playbacks[pi] = UIState::PlaybackSnap{};
        }
    }

    // Rebuild CueInfo list from project and cue_list entries.
    // Trigger when project cue count or cue-list entry count changes.
    bool cues_dirty = project &&
        ((int)state.cues.size() != (int)project->cues.size() ||
         state.cuelist_entry_count != snap.cuelist_entry_count);
    if (cues_dirty) {
        state.cues.clear();
        state.timeline_cues.clear();
        for (int i = 0; i < (int)project->cues.size(); ++i) {
            const Cue& c = project->cues[i];
            CueInfo ci;
            ci.index      = i;
            ci.name       = c.name;
            ci.duration_s = (float)c.duration;
            float r = ((c.color_tag >> 24) & 0xFF) / 255.f;
            float g = ((c.color_tag >> 16) & 0xFF) / 255.f;
            float b = ((c.color_tag >>  8) & 0xFF) / 255.f;
            ci.card_color = { r, g, b };
            static const char* gen_names[] = {
                "beams","waves","lissajous","tunnel","text","oscilloscope",
                "fft_bars","spirograph","particles","geomorph","ribbon",
                "grid","starburst","fan_sweep","cone_sweep","ilda","custom"
            };
            int gi = (int)c.generator;
            ci.generator = (gi >= 0 && gi < (int)std::size(gen_names)) ? gen_names[gi] : "unknown";
            state.cues.push_back(ci);
        }

        // Overlay name and duration from cue_list entries (FullCueEntry timing).
        // CueListEntry.fade_in + fade_out maps to the FullCueEntry timing used
        // in the engine, giving proper duration_s for the Cue Sheet display.
        for (int i = 0; i < (int)project->cue_list.size(); ++i) {
            const CueListEntry& e = project->cue_list[i];
            int cue_idx = project->find_cue(e.cue_id);
            if (cue_idx < 0 || cue_idx >= (int)state.cues.size()) continue;
            float hold = (e.out_time > 0.0)
                ? (float)(e.out_time - e.in_time)
                : (float)project->cues[cue_idx].duration;
            // duration_s = fade_in + hold + fade_out (MagicQ convention)
            state.cues[cue_idx].duration_s = e.fade_in + hold + e.fade_out;
            // Bug2: populate trigger_type from auto_next flag (0=Halt, 1=Follow)
            state.cues[cue_idx].trigger_type = e.auto_next ? 1 : 0;
            // Bug3: populate actual fade times from CueListEntry
            state.cues[cue_idx].fade_in  = e.fade_in;
            state.cues[cue_idx].fade_out = e.fade_out;
        }

        // Timeline cues from cue list entries
        for (int i = 0; i < (int)project->cue_list.size(); ++i) {
            const CueListEntry& e = project->cue_list[i];
            int ci = project->find_cue(e.cue_id);
            if (ci < 0) continue;
            TimelineCue tc;
            tc.cue_index  = ci;
            tc.start_s    = (float)e.in_time;
            tc.duration_s = (float)(e.out_time > 0 ? (e.out_time - e.in_time) : project->cues[ci].duration);
            tc.track      = i % 2;
            state.timeline_cues.push_back(tc);
        }
    }

    // Request thumbnail renders for new or updated cues.
    if (cues_dirty && project) {
        static const char* gen_names_thumb[] = {
            "beams","waves","lissajous","tunnel","text","oscilloscope",
            "fft_bars","spirograph","particles","geomorph","ribbon",
            "grid","starburst","fan_sweep","cone_sweep","ilda","beams"
        };
        for (int i = 0; i < (int)project->cues.size(); ++i) {
            const Cue& c = project->cues[i];
            int gi = static_cast<int>(c.generator);
            const char* gn = (gi >= 0 && gi < static_cast<int>(std::size(gen_names_thumb)))
                             ? gen_names_thumb[gi] : "beams";
            thumbnailer.request(i, gn, c.params);
        }
    }

    // Pull updated thumbnails from the background renderer.
    if (thumbnailer.has_dirty()) {
        thumbnailer.consume_dirty();
        if (project) {
            state.cue_thumbnails.resize(project->cues.size());
            for (int i = 0; i < (int)project->cues.size(); ++i) {
                CueThumbnail ct = thumbnailer.get(i);
                auto& ref = state.cue_thumbnails[static_cast<size_t>(i)];
                if (ct.ready && ct.version > ref.version) {
                    ref.ready   = true;
                    ref.version = ct.version;
                    ref.pixels  = ct.pixels;
                }
            }
        }
    }

    // Sync active cue's params back to inspector
    if (snap.active_cue >= 0 && snap.active_cue < (int)project->cues.size())
        state.active_params = project->cues[snap.active_cue].params;

    // FX stack sync
    state.active_fx_stack.clear();
    for (int i = 0; i < snap.fx_block_count; ++i) {
        const auto& bs = snap.fx_blocks[i];
        UIState::FxBlockInfo bi;
        bi.name     = bs.name;
        bi.category = bs.category;
        bi.enabled  = bs.enabled;
        bi.bypassed = bs.bypassed;
        bi.wet      = bs.wet;
        for (int pi = 0; pi < bs.param_count; ++pi) {
            UIState::FxBlockInfo::Param p;
            p.name     = bs.params[pi].name;
            p.val      = bs.params[pi].effective_val;
            p.base_val = bs.params[pi].base_val;
            bi.params.push_back(std::move(p));
        }
        state.active_fx_stack.push_back(std::move(bi));
    }

    // Cue list state sync
    state.cuelist_entry_count = snap.cuelist_entry_count;
    state.cuelist_current_idx = snap.cuelist_current_idx;
    state.cuelist_fade_level  = snap.cuelist_fade_level;
    state.next_cue_name       = snap.next_cue_name;
    state.record_mode         = snap.record_mode;

    // Update latency ring
    float sim_lat = (float)snap.artnet_latency_ms;
    state.latency_history[state.latency_history_idx] = (sim_lat > 0.f ? sim_lat : 2.f);
    state.latency_history_idx = (state.latency_history_idx + 1) % UIState::kLatencyHistoryLen;
    state.artnet_latency_ms   = state.latency_history[state.latency_history_idx];

    // DAC list and zone routing sync
    state.available_dacs = snap.available_dacs;
    state.zones          = snap.zones;

    // Multi-output patch state sync — merge engine status (ndi_active, ndi_conns) back
    // into the UIState.patched_outputs list which the Patch view owns.
    // The config (name, type, settings) is UI-owned; the status fields come from the engine.
    state.active_stream_ids = snap.active_stream_ids;

    // Sync per-stream point buffers for 3D preview
    state.stream_previews.clear();
    for (const auto& of : snap.output_frames) {
        UIState::StreamPreview sp;
        sp.stream_id = of.stream_id;
        sp.points    = of.points;
        state.stream_previews.push_back(std::move(sp));
    }

    // Sync laser_placements: always recompute positions sorted by stream ID.
    // Output with the lowest ID goes leftmost; higher IDs march right.
    {
        // Collect sorted laser IDs from current patch
        std::vector<int> laser_ids;
        for (const auto& po : state.patched_outputs)
            if (po.type == OutputStreamType::Laser)
                laser_ids.push_back(po.id);
        std::sort(laser_ids.begin(), laser_ids.end());

        // Remove placements for outputs that are no longer patched
        state.laser_placements.erase(
            std::remove_if(state.laser_placements.begin(), state.laser_placements.end(),
                [&](const UIState::LaserPlacement3D& lp) {
                    return std::find(laser_ids.begin(), laser_ids.end(),
                                     lp.stream_id) == laser_ids.end();
                }),
            state.laser_placements.end());

        // Ensure an entry exists for every laser
        for (int id : laser_ids) {
            bool found = std::any_of(state.laser_placements.begin(),
                                     state.laser_placements.end(),
                                     [id](const UIState::LaserPlacement3D& lp) {
                                         return lp.stream_id == id;
                                     });
            if (!found) {
                UIState::LaserPlacement3D lp;
                lp.stream_id = id;
                state.laser_placements.push_back(lp);
            }
        }

        // Recompute X positions from sorted order so the view always matches:
        // lowest ID = leftmost, spacing = 2 m, centred at X=0.
        const int n = static_cast<int>(laser_ids.size());
        for (int i = 0; i < n; ++i) {
            for (auto& lp : state.laser_placements) {
                if (lp.stream_id == laser_ids[i]) {
                    lp.pos_x = static_cast<float>(i) * 2.f
                             - static_cast<float>(n - 1) * 1.f;
                    lp.pos_y = 3.f;
                    lp.pos_z = 0.5f;
                    lp.yaw   = 0.f;
                    lp.pitch = 0.f;
                    break;
                }
            }
        }

        // Sort the placement vector itself by stream_id for consistent iteration
        std::sort(state.laser_placements.begin(), state.laser_placements.end(),
                  [](const UIState::LaserPlacement3D& a, const UIState::LaserPlacement3D& b) {
                      return a.stream_id < b.stream_id;
                  });
    }

    // Merge engine runtime status (ndi_active, ndi_conns) into UI patch entries.
    bool any_ndi_active = snap.ndi_active;  // legacy path status
    for (const auto& snap_s : snap.output_streams) {
        if (snap_s.ndi_active) any_ndi_active = true;
        for (auto& po : state.patched_outputs) {
            if (po.id == snap_s.id) {
                po.ndi_active = snap_s.ndi_active;
                po.ndi_conns  = snap_s.ndi_conns;
                break;
            }
        }
    }
    state.ndi_streaming = any_ndi_active;

    // Safety / BAM state sync
    state.bam.safety_ok     = snap.safety_ok;
    state.bam.safety_status = snap.safety_status;
    state.bam.enabled       = snap.bam_enabled;
    std::memcpy(state.bam.cells, snap.bam_cells, sizeof(state.bam.cells));

    // MIDI learn state sync
    state.midi_learn_armed  = snap.midi_learn_armed;
    state.midi_learn_target = snap.midi_learn_target;
    state.active_color_slot    = snap.active_color_slot;
    state.active_position_slot = snap.active_position_slot;

    // Sync full cue list when version changes
    if (snap.cuelist_version != state.cuelist_version) {
        state.full_cue_list = engine->read_full_cue_list();
        state.cuelist_version = snap.cuelist_version;
        // Also refresh per-playback cuelist if one is open
        if (layout_ctx.cuestack_selected_pb >= 0) {
            state.pb_cuelist = engine->read_playback_cuelist(layout_ctx.cuestack_selected_pb);
            state.pb_cuelist_pb_id = layout_ctx.cuestack_selected_pb;
        }
    }
    // Load per-playback cuelist when a different PB is selected
    if (layout_ctx.cuestack_selected_pb != state.pb_cuelist_pb_id
        && layout_ctx.cuestack_selected_pb >= 0) {
        state.pb_cuelist = engine->read_playback_cuelist(layout_ctx.cuestack_selected_pb);
        state.pb_cuelist_pb_id = layout_ctx.cuestack_selected_pb;
    }

    // ── Sync timeline state ───────────────────────────────────────────────────
    state.timelines.clear();
    for (const auto& ts : snap.timelines) {
        UIState::TimelineInfo ti;
        ti.id              = ts.id;
        ti.name            = ts.name;
        ti.state           = ts.state;
        ti.position_frames = ts.position_frames;
        ti.fps             = ts.fps;
        ti.length_frames   = ts.length_frames;
        ti.tc_slot         = ts.tc_slot;
        ti.link_mode       = ts.link_mode;
        ti.record_armed    = ts.record_armed;
        ti.source_status   = ts.source_status;
        ti.track_count     = ts.track_count;
        ti.tracks          = ts.tracks;
        ti.audio_track     = ts.audio_track;
        ti.audio_peaks     = ts.audio_peaks;
        state.timelines.push_back(std::move(ti));
    }
    state.tc_config = snap.tc_config;
}

// ─────────────────────────────────────────────────────────────────────────────
//  do_load_project — blocking project load + full UI state restore.
//  Called from run_frame() on the frame AFTER the loading overlay was shown,
//  so the user sees the overlay before any blocking I/O or device-probe work.
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::do_load_project(const std::string& path) {
    log::info("project: loading \"%s\"", path.c_str());
    try {
        Project loaded = Project::load(path);
        // Check for crash-recovery autosave — prefer it when it's newer than the main file.
        {
            std::string recovery = AutoSave::recover_path(path);
            if (!recovery.empty()) {
                namespace fs = std::filesystem;
                std::error_code ec1, ec2;
                auto main_time     = fs::last_write_time(path,     ec1);
                auto recovery_time = fs::last_write_time(recovery, ec2);
                if (!ec1 && !ec2 && recovery_time > main_time) {
                    try {
                        Project recovered = Project::load(recovery);
                        loaded = std::move(recovered);
                        log::info("project: crash-recovery loaded from '%s'", recovery.c_str());
                    } catch (...) {
                        log::warn("project: crash-recovery load failed for '%s'", recovery.c_str());
                    }
                }
            }
        }
        *project = std::move(loaded);
        state.project_path = path;
        {
            auto sep = path.find_last_of("\\/");
            state.project_name = (sep != std::string::npos) ? path.substr(sep + 1) : path;
            auto dot = state.project_name.find_last_of('.');
            if (dot != std::string::npos) state.project_name = state.project_name.substr(0, dot);
        }
        log::info("project: loaded \"%s\" ok", state.project_name.c_str());
        engine->send(cmd::LoadProject{ project });
        state.project_dirty = false;
        // Persist to recent files
        try {
            auto rf = RecentFiles::load();
            rf.push(path, project->name);
            rf.save();
            state.recent_files.clear();
            for (const auto& e : rf.entries())
                state.recent_files.push_back(e.path);
        } catch (...) {}
        // Restore UI key bindings
        state.bpm_tap_key           = project->bpm_tap_key;
        state.emergency_shutoff_key = project->emergency_shutoff_key;
        for (auto& pb : project->playbacks) {
            int idx = pb.id;
            if (idx >= 0 && idx < UIState::kMaxPlaybacks) {
                state.pb_conf[idx].keyboard_key                = pb.config.keyboard_go_key;
                state.pb_conf[idx].fade_on_first_trigger       = pb.config.fade_on_first_trigger;
                state.pb_conf[idx].first_trigger_fade_s        = pb.config.first_trigger_fade_s;
                state.pb_conf[idx].remember_cuelist_position   = pb.config.remember_cuelist_position;
                state.pb_conf[idx].output_stream_ids           = pb.config.output_stream_ids;
            }
        }
        // Restore palettes
        state.color_palette    = project->color_palette;
        state.position_palette = project->position_palette;
        static_assert(UIState::kNumSwatches == Project::kNumSwatches,
                      "Swatch count mismatch between UIState and Project");
        for (int i = 0; i < UIState::kNumSwatches; ++i) {
            state.color_swatches[i].r    = project->color_swatches[i].r;
            state.color_swatches[i].g    = project->color_swatches[i].g;
            state.color_swatches[i].b    = project->color_swatches[i].b;
            state.color_swatches[i].used = project->color_swatches[i].used;
        }
        // Restore safety blackout
        {
            const auto& src = project->safety_blackout;
            state.safety_blackout.enabled        = src.enabled;
            state.safety_blackout.borders.left   = src.borders.left;
            state.safety_blackout.borders.right  = src.borders.right;
            state.safety_blackout.borders.top    = src.borders.top;
            state.safety_blackout.borders.bottom = src.borders.bottom;
            state.safety_blackout.borders.tilt   = src.borders.tilt;
            state.safety_blackout.zones.clear();
            for (const auto& z : src.zones) {
                UIState::SafetyBlackoutConfig::BlockZone uz;
                uz.enabled   = z.enabled;
                uz.name      = z.name;
                uz.cx        = z.cx;
                uz.cy        = z.cy;
                uz.hw        = z.hw;
                uz.hh        = z.hh;
                uz.angle_deg = z.angle_deg;
                state.safety_blackout.zones.push_back(uz);
            }
            layout_cbs.on_safety_blackout_changed(state.safety_blackout);
        }
        // Restore output patch
        {
            // Tear down all existing DacManagers before rebuilding so that
            // any output whose bus ordinal changed (e.g. different number of
            // outputs in the new project) gets a fresh manager with the correct
            // bus pointer.  on_output_patch_changed({}) stops and removes them all.
            // Always tear down before rebuilding so stale DacManagers from the
            // previous project are stopped before new ones are created.
            if (layout_cbs.on_output_patch_changed)
                layout_cbs.on_output_patch_changed({});

            while (dacs_stopping_.load(std::memory_order_acquire) > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));

            state.patched_outputs.clear();
            for (const auto& cfg : project->output_patch) {
                UIState::PatchedOutput po;
                po.id          = cfg.id;
                po.name        = cfg.name;
                po.type        = cfg.type;
                po.enabled     = cfg.enabled;
                po.dac_type    = cfg.dac_type;
                po.dac_address = cfg.dac_address;
                po.config      = cfg;
                state.patched_outputs.push_back(std::move(po));
            }
            layout_ctx.patch_next_id = project->output_patch_next_id;
            if (!state.patched_outputs.empty())
                layout_ctx.patch_selected_id = state.patched_outputs.front().id;
            else
                layout_ctx.patch_selected_id = -1;
            if (layout_cbs.on_output_patch_changed) {
                std::vector<OutputStreamConfig> cfgs;
                cfgs.reserve(state.patched_outputs.size());
                for (const auto& po : state.patched_outputs)
                    cfgs.push_back(po.config);
                layout_cbs.on_output_patch_changed(cfgs);
            }
        }
        // Restore output groups
        {
            state.output_groups.clear();
            state.output_groups.reserve(project->output_groups.size());
            for (const auto& pg : project->output_groups) {
                UIState::OutputGroup g;
                g.id           = pg.id;
                g.name         = pg.name;
                g.member_ids   = pg.member_ids;
                g.mirrored_ids = pg.mirrored_ids;
                state.output_groups.push_back(std::move(g));
            }
            state.output_group_next_id = project->output_group_next_id;
        }
        // Restore network config
        {
            const auto& src = project->net_config;
            auto& dst = state.net_config;
            dst.iface_auto           = src.iface_auto;
            dst.iface_index          = src.iface_index;
            dst.artnet_enabled       = src.artnet_enabled;
            dst.artnet_auto          = src.artnet_auto;
            dst.artnet_universe      = src.artnet_universe;
            dst.artnet_net           = src.artnet_net;
            dst.artnet_subnet        = src.artnet_subnet;
            dst.artnet_listen_ip     = src.artnet_listen_ip;
            dst.artnet_port          = src.artnet_port;
            dst.artnet_merge_htp     = src.artnet_merge_htp;
            dst.artnet_merge_mode    = src.artnet_merge_mode;
            dst.artnet_priority      = src.artnet_priority;
            dst.sacn_enabled         = src.sacn_enabled;
            dst.sacn_auto            = src.sacn_auto;
            dst.sacn_universe        = src.sacn_universe;
            dst.sacn_priority        = src.sacn_priority;
            dst.sacn_multicast_ip    = src.sacn_multicast_ip;
            dst.sacn_port            = src.sacn_port;
            dst.sacn_per_universe    = src.sacn_per_universe;
            dst.osc_in_enabled       = src.osc_in_enabled;
            dst.osc_in_auto          = src.osc_in_auto;
            dst.osc_in_port          = src.osc_in_port;
            dst.osc_in_ip            = src.osc_in_ip;
            dst.osc_out_enabled      = src.osc_out_enabled;
            dst.osc_out_auto         = src.osc_out_auto;
            dst.osc_out_ip           = src.osc_out_ip;
            dst.osc_out_port         = src.osc_out_port;
            dst.osc_prefix           = src.osc_prefix;
            dst.citp_enabled         = src.citp_enabled;
            dst.citp_auto            = src.citp_auto;
            dst.citp_tcp_port        = src.citp_tcp_port;
            dst.citp_multicast_group = src.citp_multicast_group;
            dst.citp_multicast_port  = src.citp_multicast_port;
            dst.citp_source_name     = src.citp_source_name;
            dst.citp_respond_capture = src.citp_respond_capture;
            dst.idn_enabled          = src.idn_enabled;
            dst.idn_auto             = src.idn_auto;
            dst.idn_broadcast_addr   = src.idn_broadcast_addr;
            dst.idn_port             = src.idn_port;
            dst.idn_channel          = src.idn_channel;
            dst.cls_enabled          = src.cls_enabled;
            dst.cls_auto             = src.cls_auto;
            dst.cls_tcp_port         = src.cls_tcp_port;
            dst.cls_udp_port         = src.cls_udp_port;
            dst.cls_udp_broadcast    = src.cls_udp_broadcast;
            dst.etherdream_enabled   = src.etherdream_enabled;
            dst.etherdream_auto      = src.etherdream_auto;
            dst.etherdream_timeout   = src.etherdream_timeout;
            dst.etherdream_preferred_ip = src.etherdream_preferred_ip;
            dst.ndi_net_auto         = src.ndi_net_auto;
            dst.ndi_net_source_name  = src.ndi_net_source_name;
            dst.ndi_net_bandwidth    = src.ndi_net_bandwidth;
            dst.ndi_net_fps          = src.ndi_net_fps;
        }
        // Restore output config
        {
            const auto& src = project->output_config;
            auto& dst = state.output_config;
            dst.ndi_enabled            = src.ndi_enabled;
            dst.ndi_name               = src.ndi_name;
            dst.ndi_width              = src.ndi_width;
            dst.ndi_height             = src.ndi_height;
            dst.ndi_fps                = src.ndi_fps;
            dst.ndi_fps_N              = src.ndi_fps_N;
            dst.ndi_fps_D              = src.ndi_fps_D;
            dst.ndi_clock_video        = src.ndi_clock_video;
            dst.beam_radius            = src.beam_radius;
            dst.glow_radius            = src.glow_radius;
            dst.glow_alpha             = src.glow_alpha;
            dst.hdmi_window_enabled        = src.hdmi_window_enabled;
            dst.dac_enabled                = src.dac_enabled;
            dst.idn_stream_enabled         = src.idn_stream_enabled;
            dst.virtual_camera_enabled     = src.virtual_camera_enabled;
            dst.stream_type_dac_enabled    = src.stream_type_dac_enabled;
            dst.stream_type_ndi_enabled    = src.stream_type_ndi_enabled;
            dst.stream_type_artnet_enabled = src.stream_type_artnet_enabled;
            dst.stream_type_idn_enabled    = src.stream_type_idn_enabled;
        }
        // Restore 3D preview settings
        {
            const auto& src = project->preview_3d;
            auto& dst = state.preview_3d;
            dst.scan_mode       = src.scan_mode;
            dst.scan_speed      = src.scan_speed;
            dst.trail_pct       = src.trail_pct;
            dst.beam_brightness = src.beam_brightness;
            dst.haze_alpha      = src.haze_alpha;
            dst.wall_glow_px    = src.wall_glow_px;
            dst.beam_width_px   = src.beam_width_px;
        }
        // Restore laser placements
        {
            state.laser_placements.clear();
            for (const auto& lp : project->laser_placements) {
                UIState::LaserPlacement3D ulp;
                ulp.stream_id = lp.stream_id;
                ulp.pos_x     = lp.pos_x;
                ulp.pos_y     = lp.pos_y;
                ulp.pos_z     = lp.pos_z;
                ulp.yaw       = lp.yaw;
                ulp.pitch     = lp.pitch;
                state.laser_placements.push_back(ulp);
            }
        }
        state.color_input_mode = static_cast<UIState::ColorInputMode>(project->color_input_mode);
        // Restore Otaniemi config
        {
            const auto& src = project->otaniemi;
            auto& dst = state.otaniemi;
            dst.enabled          = src.enabled;
            dst.line_thickness   = src.line_thickness;
            dst.auto_fill_shapes = src.auto_fill_shapes;
            dst.brightness_boost = src.brightness_boost;
            dst.glow_radius      = src.glow_radius;
        }
        state.active_stream_ids = project->active_stream_ids;
        // Restore ui_layout context — reset init flags so dockspace/layout rebuilds cleanly
        layout_ctx.active_view             = static_cast<LayoutContext::ViewMode>(project->ui_layout.active_view);
        layout_ctx.show_layout_initialised = false;
        layout_ctx.dockspace_initialised   = false;
        // Seed timeline engine from project.
        // Peaks are never serialised; decode audio here on the UI thread so
        // the engine thread (1000 Hz) never has to block on file I/O.
        for (const auto& td : project->timelines) {
            TimelineDef def = td;
            if (def.audio_track.has_value() && def.audio_peaks.empty())
                def.audio_peaks = audio_compute_peaks(def.audio_track->file_path);
            engine->send(cmd::CreateTimeline{ std::move(def) });
        }
        engine->send(cmd::SetTimecodeSettings{ project->tc_config });
        // on_output_patch_changed (called above during patch restoration) sets
        // project_dirty = true so the autosave fires for patch-only edits.
        // Clear the flag here — after ALL restoration is complete — so that a
        // freshly loaded project does not appear modified.
        state.project_dirty = false;
        log::info("do_load_project: loaded '%s'", path.c_str());
    } catch (...) {
        log::warn("do_load_project: failed to load '%s'", path.c_str());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  do_new_project — reset all UI and engine state to a fresh empty show.
//  Mirrors do_load_project but with empty/default values instead of file data.
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::do_new_project() {
    project = std::make_shared<Project>();
    project->name = "New Show";

    // Project identity
    state.project_path  = "";
    state.project_name  = "New Show";
    state.project_dirty = false;

    // Clear cue / timeline state
    state.cues.clear();
    state.timeline_cues.clear();
    state.full_cue_list.clear();
    state.timelines.clear();

    // Clear output groups
    state.output_groups.clear();
    state.output_group_next_id = 1;

    // Clear active streams
    state.active_stream_ids.clear();

    // Reset playback snap
    state.snap.playback_count = 0;
    for (int i = 0; i < UIState::kMaxPlaybacks; ++i)
        state.snap.playbacks[i] = {};

    // Reset layout programmer state
    layout_ctx.frame_editor.objects.clear();
    layout_ctx.programmer_feeds.clear();
    layout_ctx.incl_armed          = false;
    layout_ctx.included_cue_idx    = -1;
    layout_ctx.included_pb_id      = -1;
    layout_ctx.included_pb_cue_idx = -1;

    // Force dockspace/layout rebuild on next frame
    layout_ctx.dockspace_initialised   = false;
    layout_ctx.show_layout_initialised = false;

    // Clear output patch — leave it empty so the user can add outputs manually
    state.patched_outputs.clear();
    layout_ctx.patch_next_id     = 2;
    layout_ctx.patch_selected_id = -1;

    // Engine reset
    engine->send(cmd::LoadProject{ project });
    engine->send(cmd::ClearProgrammer{});

    // Notify engine of empty patch and empty active-stream set.
    // Wait for all in-flight DacManager stops to complete before returning
    // so the new show starts with a clean slate (mirrors do_load_project).
    if (layout_cbs.on_output_patch_changed)
        layout_cbs.on_output_patch_changed({});
    while (dacs_stopping_.load(std::memory_order_acquire) > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (layout_cbs.on_active_streams_changed)
        layout_cbs.on_active_streams_changed(state.active_stream_ids, {});
    state.active_stream_ids.clear();

    log::info("do_new_project: blank show created");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Callback wiring
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::wire_callbacks() {
    layout_cbs.on_new_project   = [this]() {
        do_new_project();
    };
    layout_cbs.on_open_project  = [this]() {
#if defined(_WIN32)
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        HWND hwnd_dlg = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        std::string path = win32_open_file_dialog(hwnd_dlg);
        if (!path.empty()) {
            // Defer the actual load to the next frame so the overlay can be
            // presented to the user before the blocking parse + device probe.
            pending_load_path_ = path;
            is_loading_show    = true;
        }
#endif
    };

    // Recent Files: open a specific path directly (no dialog)
    layout_cbs.on_open_recent = [this](const std::string& path) {
        if (!path.empty()) {
            pending_load_path_ = path;
            is_loading_show    = true;
        }
    };
    layout_cbs.on_save_project  = [this]() {
        if (project) {
#if defined(_WIN32)
            if (state.project_path.empty()) {
                // No path yet — treat as Save As
                SDL_PropertiesID props = SDL_GetWindowProperties(window);
                HWND hwnd_dlg = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
                std::string path = win32_save_file_dialog(hwnd_dlg, state.project_name + ".idhmfis");
                if (!path.empty()) {
                    state.project_path = path;
                    auto sep = path.find_last_of("\\/");
                    state.project_name = (sep != std::string::npos) ? path.substr(sep+1) : path;
                    auto dot = state.project_name.find_last_of('.');
                    if (dot != std::string::npos) state.project_name = state.project_name.substr(0, dot);
                }
            }
#endif
            if (!state.project_path.empty()) {
                // Flush UI-only key bindings back into the project before saving
                project->bpm_tap_key           = state.bpm_tap_key;
                project->emergency_shutoff_key = state.emergency_shutoff_key;
                for (auto& pb : project->playbacks) {
                    int idx = pb.id;
                    if (idx >= 0 && idx < UIState::kMaxPlaybacks) {
                        pb.config.keyboard_go_key              = state.pb_conf[idx].keyboard_key;
                        pb.config.fade_on_first_trigger        = state.pb_conf[idx].fade_on_first_trigger;
                        pb.config.first_trigger_fade_s         = state.pb_conf[idx].first_trigger_fade_s;
                        pb.config.remember_cuelist_position    = state.pb_conf[idx].remember_cuelist_position;
                        pb.config.output_stream_ids            = state.pb_conf[idx].output_stream_ids;
                    }
                }
                // Flush palettes
                project->color_palette    = state.color_palette;
                project->position_palette = state.position_palette;
                // Flush color swatches
                for (int i = 0; i < UIState::kNumSwatches; ++i) {
                    project->color_swatches[i].r    = state.color_swatches[i].r;
                    project->color_swatches[i].g    = state.color_swatches[i].g;
                    project->color_swatches[i].b    = state.color_swatches[i].b;
                    project->color_swatches[i].used = state.color_swatches[i].used;
                }
                // Flush safety blackout config
                {
                    const auto& src = state.safety_blackout;
                    auto& dst = project->safety_blackout;
                    dst.enabled        = src.enabled;
                    dst.borders.left   = src.borders.left;
                    dst.borders.right  = src.borders.right;
                    dst.borders.top    = src.borders.top;
                    dst.borders.bottom = src.borders.bottom;
                    dst.borders.tilt   = src.borders.tilt;
                    dst.zones.clear();
                    for (const auto& z : src.zones) {
                        Project::SafetyBlackoutConfig::BlockZone pz;
                        pz.enabled   = z.enabled;
                        pz.name      = z.name;
                        pz.cx        = z.cx;
                        pz.cy        = z.cy;
                        pz.hw        = z.hw;
                        pz.hh        = z.hh;
                        pz.angle_deg = z.angle_deg;
                        dst.zones.push_back(pz);
                    }
                }
                // Flush network config
                {
                    const auto& src = state.net_config;
                    auto& dst = project->net_config;
                    dst.iface_auto          = src.iface_auto;
                    dst.iface_index         = src.iface_index;
                    dst.artnet_enabled      = src.artnet_enabled;
                    dst.artnet_auto         = src.artnet_auto;
                    dst.artnet_universe     = src.artnet_universe;
                    dst.artnet_net          = src.artnet_net;
                    dst.artnet_subnet       = src.artnet_subnet;
                    dst.artnet_listen_ip    = src.artnet_listen_ip;
                    dst.artnet_port         = src.artnet_port;
                    dst.artnet_merge_htp    = src.artnet_merge_htp;
                    dst.artnet_merge_mode   = src.artnet_merge_mode;
                    dst.artnet_priority     = src.artnet_priority;
                    dst.sacn_enabled        = src.sacn_enabled;
                    dst.sacn_auto           = src.sacn_auto;
                    dst.sacn_universe       = src.sacn_universe;
                    dst.sacn_priority       = src.sacn_priority;
                    dst.sacn_multicast_ip   = src.sacn_multicast_ip;
                    dst.sacn_port           = src.sacn_port;
                    dst.sacn_per_universe   = src.sacn_per_universe;
                    dst.osc_in_enabled      = src.osc_in_enabled;
                    dst.osc_in_auto         = src.osc_in_auto;
                    dst.osc_in_port         = src.osc_in_port;
                    dst.osc_in_ip           = src.osc_in_ip;
                    dst.osc_out_enabled     = src.osc_out_enabled;
                    dst.osc_out_auto        = src.osc_out_auto;
                    dst.osc_out_ip          = src.osc_out_ip;
                    dst.osc_out_port        = src.osc_out_port;
                    dst.osc_prefix          = src.osc_prefix;
                    dst.citp_enabled        = src.citp_enabled;
                    dst.citp_auto           = src.citp_auto;
                    dst.citp_tcp_port       = src.citp_tcp_port;
                    dst.citp_multicast_group= src.citp_multicast_group;
                    dst.citp_multicast_port = src.citp_multicast_port;
                    dst.citp_source_name    = src.citp_source_name;
                    dst.citp_respond_capture= src.citp_respond_capture;
                    dst.idn_enabled         = src.idn_enabled;
                    dst.idn_auto            = src.idn_auto;
                    dst.idn_broadcast_addr  = src.idn_broadcast_addr;
                    dst.idn_port            = src.idn_port;
                    dst.idn_channel         = src.idn_channel;
                    dst.cls_enabled         = src.cls_enabled;
                    dst.cls_auto            = src.cls_auto;
                    dst.cls_tcp_port        = src.cls_tcp_port;
                    dst.cls_udp_port        = src.cls_udp_port;
                    dst.cls_udp_broadcast   = src.cls_udp_broadcast;
                    dst.etherdream_enabled  = src.etherdream_enabled;
                    dst.etherdream_auto     = src.etherdream_auto;
                    dst.etherdream_timeout  = src.etherdream_timeout;
                    dst.etherdream_preferred_ip = src.etherdream_preferred_ip;
                    dst.ndi_net_auto        = src.ndi_net_auto;
                    dst.ndi_net_source_name = src.ndi_net_source_name;
                    dst.ndi_net_bandwidth   = src.ndi_net_bandwidth;
                    dst.ndi_net_fps         = src.ndi_net_fps;
                }
                // Flush output config
                {
                    const auto& src = state.output_config;
                    auto& dst = project->output_config;
                    dst.ndi_enabled                = src.ndi_enabled;
                    dst.ndi_name                   = src.ndi_name;
                    dst.ndi_width                  = src.ndi_width;
                    dst.ndi_height                 = src.ndi_height;
                    dst.ndi_fps                    = src.ndi_fps;
                    dst.ndi_fps_N                  = src.ndi_fps_N;
                    dst.ndi_fps_D                  = src.ndi_fps_D;
                    dst.ndi_clock_video            = src.ndi_clock_video;
                    dst.beam_radius                = src.beam_radius;
                    dst.glow_radius                = src.glow_radius;
                    dst.glow_alpha                 = src.glow_alpha;
                    dst.hdmi_window_enabled        = src.hdmi_window_enabled;
                    dst.dac_enabled                = src.dac_enabled;
                    dst.idn_stream_enabled         = src.idn_stream_enabled;
                    dst.virtual_camera_enabled     = src.virtual_camera_enabled;
                    dst.stream_type_dac_enabled    = src.stream_type_dac_enabled;
                    dst.stream_type_ndi_enabled    = src.stream_type_ndi_enabled;
                    dst.stream_type_artnet_enabled = src.stream_type_artnet_enabled;
                    dst.stream_type_idn_enabled    = src.stream_type_idn_enabled;
                }
                // Flush 3D preview settings
                {
                    const auto& src = state.preview_3d;
                    auto& dst = project->preview_3d;
                    dst.scan_mode       = src.scan_mode;
                    dst.scan_speed      = src.scan_speed;
                    dst.trail_pct       = src.trail_pct;
                    dst.beam_brightness = src.beam_brightness;
                    dst.haze_alpha      = src.haze_alpha;
                    dst.wall_glow_px    = src.wall_glow_px;
                    dst.beam_width_px   = src.beam_width_px;
                }
                // Flush laser placements
                {
                    project->laser_placements.clear();
                    for (const auto& lp : state.laser_placements) {
                        Project::LaserPlacement3D plp;
                        plp.stream_id = lp.stream_id;
                        plp.pos_x     = lp.pos_x;
                        plp.pos_y     = lp.pos_y;
                        plp.pos_z     = lp.pos_z;
                        plp.yaw       = lp.yaw;
                        plp.pitch     = lp.pitch;
                        project->laser_placements.push_back(plp);
                    }
                }
                // Flush color input mode
                project->color_input_mode = static_cast<int>(state.color_input_mode);
                // Flush Otaniemi config
                {
                    const auto& src = state.otaniemi;
                    auto& dst = project->otaniemi;
                    dst.enabled          = src.enabled;
                    dst.line_thickness   = src.line_thickness;
                    dst.auto_fill_shapes = src.auto_fill_shapes;
                    dst.brightness_boost = src.brightness_boost;
                    dst.glow_radius      = src.glow_radius;
                }
                // Flush active stream ids
                project->active_stream_ids = state.active_stream_ids;
                // Flush ui_layout state
                project->ui_layout.active_view             = static_cast<int>(layout_ctx.active_view);
                project->ui_layout.show_layout_initialised = layout_ctx.show_layout_initialised;
                project->ui_layout.dockspace_initialised   = layout_ctx.dockspace_initialised;
                // Flush output patch from UIState back into the project document
                // so it is saved together with the rest of the show.
                {
                    project->output_patch.clear();
                    project->output_patch.reserve(state.patched_outputs.size());
                    for (const auto& po : state.patched_outputs)
                        project->output_patch.push_back(po.config);
                    project->output_patch_next_id = layout_ctx.patch_next_id;
                }
                // Flush output groups
                {
                    project->output_groups.clear();
                    project->output_groups.reserve(state.output_groups.size());
                    for (const auto& g : state.output_groups) {
                        Project::OutputGroup pg;
                        pg.id           = g.id;
                        pg.name         = g.name;
                        pg.member_ids   = g.member_ids;
                        pg.mirrored_ids = g.mirrored_ids;
                        project->output_groups.push_back(std::move(pg));
                    }
                    project->output_group_next_id = state.output_group_next_id;
                }
                // Flush engine's current cue list state (mutex-protected snapshot) so
                // the save captures all FullCueEntry data rather than racing against
                // sync_cuelist_to_project()'s clear()+repopulate pattern.
                project->full_cue_list = engine->read_full_cue_list();
                for (auto& pb : project->playbacks)
                    pb.cuelist = engine->read_playback_cuelist(pb.id);
                // Flush live timeline state (tracks, events, record_armed) so that
                // events recorded during the session are not lost on save.
                project->timelines = engine->read_timelines();
                try {
                    project->save(state.project_path);
                    log::info("project: saved \"%s\"", state.project_path.c_str());
                    state.project_dirty = false;
                    AutoSave::clear_recover(state.project_path);
                    // Persist to recent files and refresh UI list
                    auto rf = RecentFiles::load();
                    rf.push(state.project_path, project->name);
                    rf.save();
                    state.recent_files.clear();
                    for (const auto& e : rf.entries())
                        state.recent_files.push_back(e.path);
                } catch (const std::exception& ex) {
                    log::error("project: save failed \"%s\": %s", state.project_path.c_str(), ex.what());
                } catch (...) {
                    log::error("project: save failed \"%s\": unknown exception", state.project_path.c_str());
                }
            }
        }
    };
    layout_cbs.on_save_as       = [this]() {
        if (project) {
#if defined(_WIN32)
            SDL_PropertiesID props = SDL_GetWindowProperties(window);
            HWND hwnd_dlg = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
            std::string path = win32_save_file_dialog(hwnd_dlg, state.project_name + ".idhmfis");
            if (!path.empty()) {
                state.project_path = path;
                auto sep = path.find_last_of("\\/");
                state.project_name = (sep != std::string::npos) ? path.substr(sep+1) : path;
                auto dot = state.project_name.find_last_of('.');
                if (dot != std::string::npos) state.project_name = state.project_name.substr(0, dot);
                project->bpm_tap_key           = state.bpm_tap_key;
                project->emergency_shutoff_key = state.emergency_shutoff_key;
                for (auto& pb : project->playbacks) {
                    int idx = pb.id;
                    if (idx >= 0 && idx < UIState::kMaxPlaybacks) {
                        pb.config.keyboard_go_key              = state.pb_conf[idx].keyboard_key;
                        pb.config.fade_on_first_trigger        = state.pb_conf[idx].fade_on_first_trigger;
                        pb.config.first_trigger_fade_s         = state.pb_conf[idx].first_trigger_fade_s;
                        pb.config.remember_cuelist_position    = state.pb_conf[idx].remember_cuelist_position;
                        pb.config.output_stream_ids            = state.pb_conf[idx].output_stream_ids;
                    }
                }
                // Flush palettes
                project->color_palette    = state.color_palette;
                project->position_palette = state.position_palette;
                // Flush color swatches
                for (int i = 0; i < UIState::kNumSwatches; ++i) {
                    project->color_swatches[i].r    = state.color_swatches[i].r;
                    project->color_swatches[i].g    = state.color_swatches[i].g;
                    project->color_swatches[i].b    = state.color_swatches[i].b;
                    project->color_swatches[i].used = state.color_swatches[i].used;
                }
                // Flush safety blackout config
                {
                    const auto& src = state.safety_blackout;
                    auto& dst = project->safety_blackout;
                    dst.enabled        = src.enabled;
                    dst.borders.left   = src.borders.left;
                    dst.borders.right  = src.borders.right;
                    dst.borders.top    = src.borders.top;
                    dst.borders.bottom = src.borders.bottom;
                    dst.borders.tilt   = src.borders.tilt;
                    dst.zones.clear();
                    for (const auto& z : src.zones) {
                        Project::SafetyBlackoutConfig::BlockZone pz;
                        pz.enabled   = z.enabled;
                        pz.name      = z.name;
                        pz.cx        = z.cx;
                        pz.cy        = z.cy;
                        pz.hw        = z.hw;
                        pz.hh        = z.hh;
                        pz.angle_deg = z.angle_deg;
                        dst.zones.push_back(pz);
                    }
                }
                // Flush network config
                {
                    const auto& src = state.net_config;
                    auto& dst = project->net_config;
                    dst.iface_auto          = src.iface_auto;
                    dst.iface_index         = src.iface_index;
                    dst.artnet_enabled      = src.artnet_enabled;
                    dst.artnet_auto         = src.artnet_auto;
                    dst.artnet_universe     = src.artnet_universe;
                    dst.artnet_net          = src.artnet_net;
                    dst.artnet_subnet       = src.artnet_subnet;
                    dst.artnet_listen_ip    = src.artnet_listen_ip;
                    dst.artnet_port         = src.artnet_port;
                    dst.artnet_merge_htp    = src.artnet_merge_htp;
                    dst.artnet_merge_mode   = src.artnet_merge_mode;
                    dst.artnet_priority     = src.artnet_priority;
                    dst.sacn_enabled        = src.sacn_enabled;
                    dst.sacn_auto           = src.sacn_auto;
                    dst.sacn_universe       = src.sacn_universe;
                    dst.sacn_priority       = src.sacn_priority;
                    dst.sacn_multicast_ip   = src.sacn_multicast_ip;
                    dst.sacn_port           = src.sacn_port;
                    dst.sacn_per_universe   = src.sacn_per_universe;
                    dst.osc_in_enabled      = src.osc_in_enabled;
                    dst.osc_in_auto         = src.osc_in_auto;
                    dst.osc_in_port         = src.osc_in_port;
                    dst.osc_in_ip           = src.osc_in_ip;
                    dst.osc_out_enabled     = src.osc_out_enabled;
                    dst.osc_out_auto        = src.osc_out_auto;
                    dst.osc_out_ip          = src.osc_out_ip;
                    dst.osc_out_port        = src.osc_out_port;
                    dst.osc_prefix          = src.osc_prefix;
                    dst.citp_enabled        = src.citp_enabled;
                    dst.citp_auto           = src.citp_auto;
                    dst.citp_tcp_port       = src.citp_tcp_port;
                    dst.citp_multicast_group= src.citp_multicast_group;
                    dst.citp_multicast_port = src.citp_multicast_port;
                    dst.citp_source_name    = src.citp_source_name;
                    dst.citp_respond_capture= src.citp_respond_capture;
                    dst.idn_enabled         = src.idn_enabled;
                    dst.idn_auto            = src.idn_auto;
                    dst.idn_broadcast_addr  = src.idn_broadcast_addr;
                    dst.idn_port            = src.idn_port;
                    dst.idn_channel         = src.idn_channel;
                    dst.cls_enabled         = src.cls_enabled;
                    dst.cls_auto            = src.cls_auto;
                    dst.cls_tcp_port        = src.cls_tcp_port;
                    dst.cls_udp_port        = src.cls_udp_port;
                    dst.cls_udp_broadcast   = src.cls_udp_broadcast;
                    dst.etherdream_enabled  = src.etherdream_enabled;
                    dst.etherdream_auto     = src.etherdream_auto;
                    dst.etherdream_timeout  = src.etherdream_timeout;
                    dst.etherdream_preferred_ip = src.etherdream_preferred_ip;
                    dst.ndi_net_auto        = src.ndi_net_auto;
                    dst.ndi_net_source_name = src.ndi_net_source_name;
                    dst.ndi_net_bandwidth   = src.ndi_net_bandwidth;
                    dst.ndi_net_fps         = src.ndi_net_fps;
                }
                // Flush output config
                {
                    const auto& src = state.output_config;
                    auto& dst = project->output_config;
                    dst.ndi_enabled                = src.ndi_enabled;
                    dst.ndi_name                   = src.ndi_name;
                    dst.ndi_width                  = src.ndi_width;
                    dst.ndi_height                 = src.ndi_height;
                    dst.ndi_fps                    = src.ndi_fps;
                    dst.ndi_fps_N                  = src.ndi_fps_N;
                    dst.ndi_fps_D                  = src.ndi_fps_D;
                    dst.ndi_clock_video            = src.ndi_clock_video;
                    dst.beam_radius                = src.beam_radius;
                    dst.glow_radius                = src.glow_radius;
                    dst.glow_alpha                 = src.glow_alpha;
                    dst.hdmi_window_enabled        = src.hdmi_window_enabled;
                    dst.dac_enabled                = src.dac_enabled;
                    dst.idn_stream_enabled         = src.idn_stream_enabled;
                    dst.virtual_camera_enabled     = src.virtual_camera_enabled;
                    dst.stream_type_dac_enabled    = src.stream_type_dac_enabled;
                    dst.stream_type_ndi_enabled    = src.stream_type_ndi_enabled;
                    dst.stream_type_artnet_enabled = src.stream_type_artnet_enabled;
                    dst.stream_type_idn_enabled    = src.stream_type_idn_enabled;
                }
                // Flush 3D preview settings
                {
                    const auto& src = state.preview_3d;
                    auto& dst = project->preview_3d;
                    dst.scan_mode       = src.scan_mode;
                    dst.scan_speed      = src.scan_speed;
                    dst.trail_pct       = src.trail_pct;
                    dst.beam_brightness = src.beam_brightness;
                    dst.haze_alpha      = src.haze_alpha;
                    dst.wall_glow_px    = src.wall_glow_px;
                    dst.beam_width_px   = src.beam_width_px;
                }
                // Flush laser placements
                {
                    project->laser_placements.clear();
                    for (const auto& lp : state.laser_placements) {
                        Project::LaserPlacement3D plp;
                        plp.stream_id = lp.stream_id;
                        plp.pos_x     = lp.pos_x;
                        plp.pos_y     = lp.pos_y;
                        plp.pos_z     = lp.pos_z;
                        plp.yaw       = lp.yaw;
                        plp.pitch     = lp.pitch;
                        project->laser_placements.push_back(plp);
                    }
                }
                // Flush color input mode
                project->color_input_mode = static_cast<int>(state.color_input_mode);
                // Flush Otaniemi config
                {
                    const auto& src = state.otaniemi;
                    auto& dst = project->otaniemi;
                    dst.enabled          = src.enabled;
                    dst.line_thickness   = src.line_thickness;
                    dst.auto_fill_shapes = src.auto_fill_shapes;
                    dst.brightness_boost = src.brightness_boost;
                    dst.glow_radius      = src.glow_radius;
                }
                // Flush active stream ids
                project->active_stream_ids = state.active_stream_ids;
                // Flush ui_layout state
                project->ui_layout.active_view             = static_cast<int>(layout_ctx.active_view);
                project->ui_layout.show_layout_initialised = layout_ctx.show_layout_initialised;
                project->ui_layout.dockspace_initialised   = layout_ctx.dockspace_initialised;
                // Flush output patch
                {
                    project->output_patch.clear();
                    project->output_patch.reserve(state.patched_outputs.size());
                    for (const auto& po : state.patched_outputs)
                        project->output_patch.push_back(po.config);
                    project->output_patch_next_id = layout_ctx.patch_next_id;
                }
                // Flush output groups
                {
                    project->output_groups.clear();
                    project->output_groups.reserve(state.output_groups.size());
                    for (const auto& g : state.output_groups) {
                        Project::OutputGroup pg;
                        pg.id           = g.id;
                        pg.name         = g.name;
                        pg.member_ids   = g.member_ids;
                        pg.mirrored_ids = g.mirrored_ids;
                        project->output_groups.push_back(std::move(pg));
                    }
                    project->output_group_next_id = state.output_group_next_id;
                }
                project->full_cue_list = engine->read_full_cue_list();
                for (auto& pb : project->playbacks)
                    pb.cuelist = engine->read_playback_cuelist(pb.id);
                project->timelines = engine->read_timelines();
                try {
                    project->save(state.project_path);
                    log::info("project: saved (save-as) \"%s\"", state.project_path.c_str());
                    state.project_dirty = false;
                    AutoSave::clear_recover(state.project_path);
                    // Persist to recent files and refresh UI list
                    auto rf = RecentFiles::load();
                    rf.push(state.project_path, project->name);
                    rf.save();
                    state.recent_files.clear();
                    for (const auto& e : rf.entries())
                        state.recent_files.push_back(e.path);
                } catch (const std::exception& ex) {
                    log::error("project: save-as failed \"%s\": %s", state.project_path.c_str(), ex.what());
                } catch (...) {
                    log::error("project: save-as failed \"%s\": unknown exception", state.project_path.c_str());
                }
            }
#endif
        }
    };
    layout_cbs.on_undo          = [this]() {
        state.project_dirty = true;
        if (!layout_ctx.programmer_undo_stack.empty() && !layout_ctx.fe_window_focused) {
            ProgrammerUndoEntry entry = std::move(layout_ctx.programmer_undo_stack.back());
            layout_ctx.programmer_undo_stack.pop_back();
            layout_ctx.frame_editor        = std::move(entry.frame_editor);
            layout_ctx.programmer_global   = std::move(entry.programmer_global);
            layout_ctx.programmer_fx_layer = std::move(entry.programmer_fx_layer);
            layout_ctx.programmer_feeds    = std::move(entry.programmer_feeds);
            state.active_stream_ids        = entry.active_stream_ids;
            state.active_group_id          = entry.active_group_id;
            engine->send(cmd::SetActiveStreams{ state.active_stream_ids });
        }
    };
    layout_cbs.on_redo          = [this]() { state.project_dirty = true; };

    // Transport — send engine commands
    layout_cbs.on_play   = [this]() { log::info("transport: play"); engine->send(cmd::Play{}); };
    layout_cbs.on_pause  = [this]() { log::info("transport: pause"); engine->send(cmd::Pause{}); };
    layout_cbs.on_stop   = [this]() { log::info("transport: stop"); engine->send(cmd::Stop{}); };

    layout_cbs.on_fullscreen = [this]() { want_fullscreen = !want_fullscreen; };

    // Cue selection → activate cue in engine
    layout_cbs.on_cue_selected  = [this](int idx) {
        engine->send(cmd::ActivateCue{ idx, 0.5f });
    };
    layout_cbs.on_cue_duplicate = [this](int idx) {
        if (!project || idx < 0 || idx >= (int)project->cues.size()) return;
        Cue copy = project->cues[idx];
        copy.id   = Project::new_id();
        copy.name += " Copy";
        project->cues.push_back(copy);
        state.cues.clear(); // force rebuild next sync
        state.project_dirty = true;
    };
    layout_cbs.on_cue_delete    = [this](int idx) {
        if (!project || idx < 0 || idx >= (int)project->cues.size()) return;
        project->cues.erase(project->cues.begin() + idx);
        state.cues.clear();
        state.project_dirty = true;
        // Also tell engine to remove from its cue list to prevent desync
        engine->send(cmd::DeleteCue{idx});
    };

    // Timeline scrub → engine Seek
    layout_cbs.on_seek = [this](double t) {
        engine->send(cmd::Seek{ t });
    };

    layout_cbs.on_help = [this]() {
        // Locate docs relative to the exe: build/bin/IDHMFIS.exe -> ../../docs/
        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        std::string path(exe_path);
        auto sep = path.find_last_of("\\/");
        if (sep != std::string::npos) path = path.substr(0, sep); // strip filename
        sep = path.find_last_of("\\/");
        if (sep != std::string::npos) path = path.substr(0, sep); // strip "bin"
        sep = path.find_last_of("\\/");
        if (sep != std::string::npos) path = path.substr(0, sep); // strip "build"
        path += "\\docs\\IDHMFIS_V1.00_Manual.html";
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, SW_SHOW);
        (void)this;
    };

    // Inspector param edits → engine command
    layout_cbs.on_param_changed = [this](const char* name, float val) {
        if (state.active_cue_idx >= 0)
            engine->send(cmd::SetGeneratorParam{ state.active_cue_idx, name, val });
    };

    // Master intensity slider
    layout_cbs.on_master_intensity = [this](float v) {
        engine->send(cmd::SetMasterIntensity{ v });
    };

    // Cue list transport controls
    layout_cbs.on_go   = [this]() { engine->send(cmd::CueListGo{}); };
    layout_cbs.on_back = [this]() { engine->send(cmd::CueListBack{}); };
    layout_cbs.on_cuelist_jump = [this](int maj, int min) {
        engine->send(cmd::CueListJump{ maj, min });
    };

    // FX engine controls
    layout_cbs.on_fx_param_changed = [this](int ci, int fi, const char* p, float v) {
        engine->send(cmd::SetFxParam{ ci, fi, std::string(p), v });
    };
    layout_cbs.on_fx_enabled  = [this](int ci, int fi, bool en) {
        engine->send(cmd::SetFxEnabled{ ci, fi, en });
    };
    layout_cbs.on_fx_bypassed = [this](int ci, int fi, bool bp) {
        engine->send(cmd::SetFxBypassed{ ci, fi, bp });
    };
    layout_cbs.on_fx_wet      = [this](int ci, int fi, float w) {
        engine->send(cmd::SetFxWet{ ci, fi, w });
    };

    // §B6: BPM — only explicit user interaction changes BPM in the engine
    layout_cbs.on_bpm_changed = [this](float val) {
        engine->send(cmd::SetBPM{ val });
    };

    // §Macro: trigger macro slot
    layout_cbs.on_macro_trigger = [this](int slot) {
        engine->send(cmd::TriggerMacro{ slot });
    };

    // Record mode controls
    layout_cbs.on_set_record_mode = [this](bool active) {
        engine->send(cmd::SetRecordMode{active});
    };
    layout_cbs.on_record_cue = [this](const std::string& name, float fi, float fo) {
        engine->send(cmd::RecordCue{name, fi, fo});
    };
    layout_cbs.on_delete_cue = [this](int idx) {
        engine->send(cmd::DeleteCue{idx});
    };
    layout_cbs.on_set_cue_timing = [this](int idx, float fi, float fo, float di, float hold) {
        engine->send(cmd::SetCueTiming{idx, fi, fo, di, hold});
    };
    layout_cbs.on_rename_cue = [this](int idx, const std::string& name) {
        engine->send(cmd::RenameCue{idx, name});
    };

    // Output kill switch
    layout_cbs.on_output_enable = [this](bool enabled) {
        state.output_enabled = enabled;
        engine->send(cmd::SetOutputEnable{enabled});
    };

    // Enabled output streams — master enable per stream kind
    layout_cbs.on_stream_type_enabled = [this](int kind, bool enabled) {
        using Kind = cmd::SetStreamTypeEnabled::Kind;
        engine->send(cmd::SetStreamTypeEnabled{ static_cast<Kind>(kind), enabled });
        // IDN sidecar lives in DacManager — propagate there as well
        if (kind == 3) {
            for (auto& kv : laser_managers_)
                kv.second->set_idn_sidecar_enabled(enabled);
        }
    };

    // Emergency shutoff — PANIC button click
    layout_cbs.on_emergency_shutoff = [this](bool active) {
        log::warn("safety: emergency shutoff %s", active ? "ACTIVATED" : "released");
        engine->send(cmd::SetEmergencyShutoff{ active });
        if (!active) {
            state.output_enabled = true;
            engine->send(cmd::SetOutputEnable{ true });
        }
    };

    // HDMI borderless window — create a borderless window the user drags to their projector/display
    layout_cbs.on_hdmi_window_toggle = [this](bool enabled) {
        state.output_config.hdmi_window_enabled = enabled;
        if (enabled && !hdmi_window_) {
            // Create a plain 1920x1080 borderless window; user drags it to the HDMI display
            hdmi_window_ = SDL_CreateWindow("IDHMFIS Output",
                                            1920, 1080,
                                            SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
            if (!hdmi_window_) {
                // Fall back to a regular window if borderless fails
                hdmi_window_ = SDL_CreateWindow("IDHMFIS Output", 1920, 1080, 0);
            }
            // SDL3 windows (especially borderless) must be explicitly shown after
            // creation — they are not visible by default on all platforms/WMs.
            if (hdmi_window_) {
                SDL_SetWindowPosition(hdmi_window_, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                SDL_ShowWindow(hdmi_window_);
                SDL_RaiseWindow(hdmi_window_);
            }
        } else if (!enabled && hdmi_window_) {
            SDL_DestroyWindow(hdmi_window_);
            hdmi_window_ = nullptr;
        }
    };

    // BAM editor callbacks
    layout_cbs.on_bam_paint = [this](int row, int col, int brush, uint8_t val) {
        engine->send(cmd::BamPaint{ row, col, brush, val });
    };
    layout_cbs.on_bam_clear = [this]() {
        engine->send(cmd::BamClear{});
    };
    layout_cbs.on_bam_enabled = [this](bool en) {
        log::info("safety: BAM %s", en ? "enabled" : "disabled");
        engine->send(cmd::BamSetEnabled{ en });
    };
    layout_cbs.on_reset_scan_fail = [this]() {
        log::info("safety: scan-fail latch reset by operator");
        engine->send(cmd::ResetScanFail{});
    };
    layout_cbs.on_scan_fail_enabled = [this](bool enabled) {
        log::info("safety: scan-fail protection %s", enabled ? "enabled" : "disabled");
        engine->send(cmd::SetScanFailEnabled{ enabled });
    };

    // Safety Blackout Zones
    layout_cbs.on_autosave_changed = [this](bool enabled, int interval_s) {
        auto_save_.set_enabled(enabled);
        auto_save_.set_interval(interval_s);
    };

    // Developer logging toggle — enable/disable session log and persist preference
    layout_cbs.on_dev_logging_changed = [this](bool enabled) {
        layout_ctx.developer_logging = enabled;
        if (enabled) {
            std::string log_path = get_dev_log_path();
            log::enable_dev_logging(log_path);
            log::info("dev log: enabled — writing to %s", log_path.c_str());
        } else {
            log::info("dev log: disabled by user");
            log::disable_dev_logging();
        }
        save_developer_logging_setting(enabled);
    };

    layout_cbs.on_safety_blackout_changed = [this](const UIState::SafetyBlackoutConfig& cfg) {
        cmd::SetSafetyBlackout cmd;
        cmd.enabled = cfg.enabled;
        cmd.borders = { cfg.borders.left, cfg.borders.right,
                        cfg.borders.top,  cfg.borders.bottom, cfg.borders.tilt };
        for (const auto& z : cfg.zones)
            cmd.zones.push_back({ z.cx, z.cy, z.hw, z.hh, z.angle_deg, z.enabled });
        engine->send(cmd);
    };

    // ILDA import — stub: loads from a hardcoded path so the plumbing is wired.
    // Replace the hardcoded path with a real file-dialog call when available.
    layout_cbs.on_import_ilda = [this]() {
        std::string path = "test.ild";
        int id = ilda_pool.load(path);
        if (id >= 0) {
            // Successfully loaded — in a full implementation we would notify
            // the engine or update the project's media pool reference.
            (void)id;
        }
    };

    // ILDA export — stub: exports the active cue's last preview frame as format 4.
    layout_cbs.on_export_ilda = [this]() {
        std::string path = "export.ild";
        std::vector<PointBuffer> frames;
        if (!state.preview_points.empty())
            frames.push_back(state.preview_points);
        std::string err;
        ilda_export(path, frames, 0, &err);
    };

    // Zone routing callbacks
    layout_cbs.on_zone_set = [this](const Zone& z) {
        engine->send(cmd::SetZone{ z });
    };
    layout_cbs.on_zone_remove = [this](int id) {
        engine->send(cmd::RemoveZone{ id });
    };

    // MIDI learn callbacks
    layout_cbs.on_midi_bind = [this](MidiBinding b) {
        engine->send(cmd::SetMidiBinding{ std::move(b) });
    };
    layout_cbs.on_midi_unbind = [this](const std::string& target) {
        engine->send(cmd::RemoveMidiBinding{ target });
    };
    layout_cbs.on_midi_clear = [this]() {
        engine->send(cmd::ClearMidiBindings{});
        state.midi_bindings.clear();
    };

    // Palette selection callbacks
    layout_cbs.on_color_select = [this](int slot) {
        state.active_color_slot = slot;
        engine->send(cmd::SetActiveColor{ slot });
    };
    layout_cbs.on_position_select = [this](int slot) {
        state.active_position_slot = slot;
        engine->send(cmd::SetActivePosition{ slot });
    };
    layout_cbs.on_dmx_patch = [this](int universe, int channel, uint8_t value) {
        engine->send(cmd::PatchDmxChannel{ universe, channel, value });
    };

    // ArtNet output configuration — persist to project so settings survive reload
    layout_cbs.on_artnet_out_config = [this](const std::string& ip, int u, bool en) {
        state.output_config.artnet_out_ip              = ip;
        state.output_config.artnet_out_universe_offset = u;
        state.output_config.artnet_out_enabled         = en;
        if (project) {
            project->artnet_out_ip              = ip;
            project->artnet_out_universe_offset = u;
            project->artnet_out_enabled         = en;
        }
        state.project_dirty = true;
        engine->send(cmd::SetArtNetOutput{ ip, u, en });
    };

    // Chaser cue editing
    layout_cbs.on_cuelist_update_entry = [this](int idx, FullCueEntry entry) {
        engine->send(cmd::UpdateCueListEntry{idx, std::move(entry)});
        state.project_dirty = true;
    };
    layout_cbs.on_new_chaser_cue = [this]() {
        engine->send(cmd::NewChaserCue{"New Chaser"});
    };

    // Frame editor — export static frame to a new cue
    layout_cbs.on_frame_export = [this](const std::vector<FrameEditorState::PlacedPrim>& prims) {
        if (!project) return;
        Cue c;
        c.id       = Project::new_id();
        c.name     = "Frame " + std::to_string(project->cues.size() + 1);
        c.generator = GeneratorType::Custom;
        c.duration  = 0.0;  // loop forever
        project->cues.push_back(c);
        state.cues.clear();  // force rebuild next sync
        state.project_dirty = true;
        // Also notify engine so the cue list stays in sync
        FullCueEntry fce;
        fce.name   = c.name;
        fce.cue_id = c.id;
        fce.number.major = static_cast<int>(project->cues.size());
        engine->send(cmd::InsertCue{-1, fce});
        (void)prims;  // frame prim data stored in project in a future iteration
    };

    // Frame editor — export keyframe animation to a new cue
    layout_cbs.on_frame_animation_export = [this](const std::vector<FrameEditorState::FrameKeyframe>& keyframes) {
        if (!project) return;
        Cue c;
        c.id       = Project::new_id();
        c.name     = "Anim " + std::to_string(project->cues.size() + 1);
        c.generator = GeneratorType::Custom;
        c.duration  = keyframes.empty() ? 4.0 : static_cast<double>(keyframes.back().time_s);
        project->cues.push_back(c);
        state.cues.clear();
        state.project_dirty = true;
        FullCueEntry fce;
        fce.name   = c.name;
        fce.cue_id = c.id;
        fce.number.major = static_cast<int>(project->cues.size());
        engine->send(cmd::InsertCue{-1, fce});
        (void)keyframes;
    };

    // Setup window — NDI config callback
    layout_cbs.on_setup_ndi_config = [this](const LayoutCallbacks::NdiConfig& cfg) {
        log::info("ndi: config applied name=\"%s\" %dx%d fps=%d/%d enabled=%d",
                  cfg.source_name.c_str(), cfg.width, cfg.height,
                  cfg.fps_N, cfg.fps_D, (int)cfg.enabled);
        state.output_config.ndi_name    = cfg.source_name;
        state.output_config.ndi_width   = cfg.width;
        state.output_config.ndi_height  = cfg.height;
        state.output_config.ndi_fps_N     = cfg.fps_N;
        state.output_config.ndi_fps_D     = cfg.fps_D;
        state.output_config.ndi_fps       = cfg.fps_N / std::max(1, cfg.fps_D);  // legacy
        state.output_config.ndi_enabled   = cfg.enabled;
        state.output_config.ndi_clock_video = cfg.clock_video;
        // Persist to project so it survives save/reload.
        if (project) {
            project->ndi_fps_N      = cfg.fps_N;
            project->ndi_fps_D      = cfg.fps_D;
            project->ndi_fps        = state.output_config.ndi_fps;
            project->ndi_width      = cfg.width;
            project->ndi_height     = cfg.height;
            project->ndi_clock_video = cfg.clock_video;
        }
        state.project_dirty = true;
        engine->send(cmd::SetNdiConfig{
            cfg.source_name, cfg.width, cfg.height, cfg.fps_N, cfg.fps_D, cfg.enabled,
            cfg.clock_video
        });
    };

    // Setup window — OSC port callback
    layout_cbs.on_setup_osc_port = [this](int osc_port) {
        (void)osc_port;
        state.project_dirty = true;
    };

    // Network configuration applied — store in UIState; engine subsystems read directly
    layout_cbs.on_network_config_apply = [this](const UIState::NetworkConfig& cfg) {
        log::info("network: config applied artnet=%d osc_in=%d osc_out=%d citp=%d",
                  (int)cfg.artnet_enabled, (int)cfg.osc_in_enabled,
                  (int)cfg.osc_out_enabled, (int)cfg.citp_enabled);
        if (cfg.artnet_enabled)
            log::info("network: artnet listen %s:%d uni=%d",
                      cfg.artnet_listen_ip.c_str(), cfg.artnet_port, cfg.artnet_universe);
        if (cfg.osc_in_enabled)
            log::info("network: osc-in %s:%d", cfg.osc_in_ip.c_str(), cfg.osc_in_port);
        state.net_config = cfg;
        state.project_dirty = true;
    };

    // Setup window — beam render params callback (also merges Otaniemi settings)
    layout_cbs.on_setup_beam_params = [this](float thickness, float glow_r, float glow_a) {
        state.output_config.beam_radius = thickness;
        state.output_config.glow_radius = glow_r;
        state.output_config.glow_alpha  = glow_a;
        layout_ctx.beam_thickness = thickness;  // sync to existing beam_thickness in LayoutContext
        cmd::SetRasterConfig rc;
        rc.beam_radius               = thickness;
        rc.glow_radius               = glow_r;
        rc.glow_alpha                = glow_a;
        rc.otaniemi_enabled          = state.otaniemi.enabled;
        rc.otaniemi_line_thickness   = state.otaniemi.line_thickness;
        rc.otaniemi_brightness_boost = state.otaniemi.brightness_boost;
        rc.otaniemi_glow_radius      = state.otaniemi.glow_radius;
        rc.otaniemi_auto_fill        = state.otaniemi.auto_fill_shapes;
        engine->send(rc);
    };

    // Playback DMX trigger + end-behavior + BPM configuration
    layout_cbs.on_playback_config = [this](int pb_id, const UIState::PlaybackConf& conf) {
        PlaybackConfig cfg;
        cfg.dmx_mode      = static_cast<PlaybackConfig::DmxMode>(static_cast<int>(conf.dmx_mode));
        cfg.dmx_universe  = conf.dmx_universe;
        cfg.dmx_channel   = conf.dmx_channel - 1;  // UI is 1-based, engine is 0-based
        cfg.dmx_threshold = static_cast<uint8_t>(conf.dmx_threshold);
        cfg.end_behavior  = static_cast<PlaybackConfig::EndBehavior>(static_cast<int>(conf.end_behavior));
        cfg.go_at_bpm               = conf.go_at_bpm;
        cfg.fx_at_bpm               = conf.fx_at_bpm;
        cfg.keyboard_go_key         = conf.keyboard_key;
        cfg.fade_on_first_trigger        = conf.fade_on_first_trigger;
        cfg.first_trigger_fade_s         = conf.first_trigger_fade_s;
        cfg.remember_cuelist_position    = conf.remember_cuelist_position;
        cfg.output_stream_ids            = conf.output_stream_ids;
        engine->send(cmd::SetPlaybackConfig{pb_id, cfg});
    };

    // Direct record from programmer into a playback slot
    layout_cbs.on_record_frame_to_playback = [this](int pb_id, FullCueEntry fce) {
        static int frame_counter = 0;
        fce.name = "Frame " + std::to_string(++frame_counter);
        engine->send(cmd::RecordToPlayback{ pb_id, fce });
        state.project_dirty = true;
    };

    // Include cue into programmer — sync engine latches for deselected streams
    layout_cbs.on_include_cue = [this](int cue_idx) {
        // Sync engine latches: for each deselected stream that has per-stream FX in
        // the included cue, update stream_prog_ so hardware output reflects the include.
        if (cue_idx < 0 || cue_idx >= (int)state.full_cue_list.size()) return;
        const FullCueEntry& fce = state.full_cue_list[cue_idx];
        KeyframeLayer kf;
        kf.objects       = layout_ctx.frame_editor.objects;
        kf.symmetry_mode = static_cast<int>(layout_ctx.frame_editor.symmetry);
        for (const auto& [sid, sfx] : fce.per_stream_fx) {
            bool active = std::find(state.active_stream_ids.begin(),
                                    state.active_stream_ids.end(), sid)
                          != state.active_stream_ids.end();
            if (!active) {
                engine->send(cmd::LatchProgrammer{
                    { sid },
                    kf,
                    layout_ctx.programmer_global,
                    sfx
                });
            }
        }
    };

    // Include playback cue into programmer — sync engine latches for deselected streams
    layout_cbs.on_include_playback_cue = [this](int pb_id, int cue_idx) {
        // Find the playback's cuelist and update engine latches for included per-stream FX
        if (state.pb_cuelist_pb_id == pb_id &&
            cue_idx >= 0 && cue_idx < (int)state.pb_cuelist.size()) {
            const FullCueEntry& fce = state.pb_cuelist[cue_idx];
            KeyframeLayer kf;
            kf.objects       = layout_ctx.frame_editor.objects;
            kf.symmetry_mode = static_cast<int>(layout_ctx.frame_editor.symmetry);
            for (const auto& [sid, sfx] : fce.per_stream_fx) {
                bool active = std::find(state.active_stream_ids.begin(),
                                        state.active_stream_ids.end(), sid)
                              != state.active_stream_ids.end();
                if (!active) {
                    engine->send(cmd::LatchProgrammer{
                        { sid },
                        kf,
                        layout_ctx.programmer_global,
                        sfx
                    });
                }
            }
        }
    };

    // Update: replace the included library cue with the current programmer content
    layout_cbs.on_update_included_cue = [this](int cue_idx, FullCueEntry fce) {
        engine->send(cmd::UpdateCueListEntry{cue_idx, std::move(fce)});
        state.project_dirty = true;
    };

    // Playback GO — advance to next cue and activate
    layout_cbs.on_playback_go = [this](int pb_id) {
        engine->send(cmd::SetPlaybackCueGo{ pb_id });
    };

    // Playback STOP — deactivate
    layout_cbs.on_playback_stop = [this](int pb_id) {
        engine->send(cmd::SetPlaybackStop{ pb_id });
    };

    // Playback CLEAR — wipe cue position
    layout_cbs.on_playback_clear = [this](int pb_id) {
        engine->send(cmd::SetPlaybackClear{ pb_id });
    };

    // Playback blind toggle
    layout_cbs.on_playback_blind = [this](int pb_id, bool blind) {
        engine->send(cmd::SetPlaybackBlind{ pb_id, blind });
    };

    // Playback intensity fader
    layout_cbs.on_playback_intensity = [this](int pb_id, float intensity) {
        engine->send(cmd::SetPlaybackIntensity{ pb_id, intensity });
    };

    // Playback rename — sent through the engine command queue so the rename
    // happens on the engine thread (avoids a data race on PlaybackDef::name).
    layout_cbs.on_playback_rename = [this](int pb_id, const std::string& name) {
        engine->send(cmd::RenamePlayback{ pb_id, name });
        state.project_dirty = true;
    };

    // New playback slot
    layout_cbs.on_playback_new = [this]() {
        engine->send(cmd::NewPlayback{ "Playback" });
        state.project_dirty = true;
    };

    // Cuestack editor: update a cue's timing/name
    layout_cbs.on_playback_cue_update = [this](int pb_id, int cue_idx, FullCueEntry entry) {
        engine->send(cmd::UpdatePlaybackCue{ pb_id, cue_idx, std::move(entry) });
        state.project_dirty = true;
    };

    // Cuestack editor: delete a cue
    layout_cbs.on_playback_cue_delete = [this](int pb_id, int cue_idx) {
        engine->send(cmd::DeletePlaybackCue{ pb_id, cue_idx });
        state.project_dirty = true;
    };

    // Cuestack editor: step back one cue in a playback
    layout_cbs.on_playback_cue_back = [this](int pb_id) {
        engine->send(cmd::SetPlaybackCueBack{ pb_id });
    };

    // Cuestack editor: jump to specific cue by index
    layout_cbs.on_playback_cue_jump = [this](int pb_id, int cue_idx) {
        // Convert index to major cue number (1-based) for SetPlaybackJump
        if (!project) return;
        for (const auto& pb : project->playbacks) {
            if (pb.id == pb_id && cue_idx >= 0 && cue_idx < static_cast<int>(pb.cuelist.size())) {
                int major = pb.cuelist[static_cast<size_t>(cue_idx)].number.major;
                engine->send(cmd::SetPlaybackJump{ pb_id, major, 0 });
                engine->send(cmd::SetPlaybackGo{ pb_id });
                break;
            }
        }
    };

    layout_cbs.on_groups_changed = [this]() {
        state.project_dirty = true;
    };

    // Output patch: full list of stream configs changed by the Patch view.
    // The engine reconciles its output_streams_ list; ui_app reconciles
    // laser_managers_ (one DacManager per Laser output) and hdmi_windows_ (one
    // SDL_Window per HDMI output).  NDI outputs are fully engine-managed.
    layout_cbs.on_output_patch_changed = [this](const std::vector<OutputStreamConfig>& configs) {

        // ── 1. Build the set of ids present in the new patch ─────────────────
        // Track which laser outputs exist and their ordinal bus index.
        // The engine wires laser buses positionally (0th laser → bus_,
        // 1st laser → extra_laser_buses_[0], …) so we must use the same ordinal.
        std::unordered_map<int, int> laser_ordinal; // output_id -> bus ordinal
        {
            int ord = 0;
            for (const auto& cfg : configs) {
                if (cfg.type == OutputStreamType::Laser) {
                    laser_ordinal[cfg.id] = ord++;
                }
            }
        }

        // ── 2. Create new DacManagers for Laser outputs not yet in the map ───
        for (const auto& cfg : configs) {
            if (cfg.type != OutputStreamType::Laser) continue;

            auto it = laser_managers_.find(cfg.id);
            if (it == laser_managers_.end()) {
                // New laser output — create a DacManager draining the engine bus
                // that corresponds to this output's positional laser slot.
                int ord = laser_ordinal[cfg.id];
                RenderBus* bus = engine->extra_laser_bus(ord);
                auto mgr = std::make_unique<DacManager>(*bus, ord);
                // Apply explicit DAC config if provided
                if (cfg.dac_type != "auto" && !cfg.dac_type.empty())
                    mgr->force_dac(cfg.dac_type, cfg.dac_address);
                if (cfg.point_rate > 0)
                    mgr->set_point_rate(cfg.point_rate);
                // Start on a background thread — never blocks the UI thread
                log::info("dac: starting DacManager id=%d type=%s addr=%s pps=%d",
                          cfg.id, cfg.dac_type.c_str(), cfg.dac_address.c_str(), cfg.point_rate);
                DacManager* raw = mgr.get();
                std::thread([raw]{ raw->start(); }).detach();
                laser_managers_.emplace(cfg.id, std::move(mgr));
            } else {
                // Existing manager — apply any updated config
                if (cfg.dac_type != "auto" && !cfg.dac_type.empty())
                    it->second->force_dac(cfg.dac_type, cfg.dac_address);
                if (cfg.point_rate > 0)
                    it->second->set_point_rate(cfg.point_rate);
            }
        }

        // ── 3. Create new SDL windows for HDMI outputs not yet in the map ────
        for (const auto& cfg : configs) {
            if (cfg.type != OutputStreamType::HDMI) continue;

            if (hdmi_windows_.find(cfg.id) == hdmi_windows_.end()) {
                SDL_Window* w = SDL_CreateWindow(cfg.name.c_str(),
                                                 1920, 1080,
                                                 SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
                if (!w)
                    w = SDL_CreateWindow(cfg.name.c_str(), 1920, 1080, 0);
                if (w) {
                    SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                    SDL_ShowWindow(w);
                    SDL_RaiseWindow(w);
                    hdmi_windows_[cfg.id] = w;
                }
            }
        }

        // ── 4. Stop and remove DacManagers for outputs no longer in the patch ─
        // stop() can block for up to ~150ms (scan-thread wakeup + EtherDream probe),
        // so we release ownership and stop on a background thread to keep the UI
        // responsive. dacs_stopping_ counts in-flight stops; run_frame() shows an
        // "Applying patch..." overlay while it is > 0.
        {
            std::vector<int> to_erase;
            for (auto& [id, mgr] : laser_managers_) {
                bool found = false;
                for (const auto& cfg : configs)
                    if (cfg.type == OutputStreamType::Laser && cfg.id == id)
                        { found = true; break; }
                if (!found) {
                    DacManager* raw = mgr.release();
                    dacs_stopping_.fetch_add(1, std::memory_order_relaxed);
                    std::thread([raw, this]() {
                        raw->stop();
                        delete raw;
                        dacs_stopping_.fetch_sub(1, std::memory_order_release);
                    }).detach();
                    to_erase.push_back(id);
                }
            }
            for (int id : to_erase)
                laser_managers_.erase(id);
        }

        // ── 5. Destroy SDL windows for HDMI outputs no longer in the patch ────
        {
            std::vector<int> to_erase;
            for (auto& [id, w] : hdmi_windows_) {
                bool found = false;
                for (const auto& cfg : configs)
                    if (cfg.type == OutputStreamType::HDMI && cfg.id == id)
                        { found = true; break; }
                if (!found) {
                    SDL_DestroyWindow(w);
                    to_erase.push_back(id);
                }
            }
            for (int id : to_erase)
                hdmi_windows_.erase(id);
        }

        // ── 6. Tell the engine about the new patch ────────────────────────────
        engine->send(cmd::SetOutputPatch{ configs });

        // ── 7. Compute active stream ids ──────────────────────────────────────
        // Broadcast mode: send all enabled laser IDs so the engine knows the set.
        // Per-output mode: programmer routing is owned by the streams panel
        // (on_active_streams_changed). Here we only remove IDs that no longer
        // exist in the patch — we never ADD new IDs in per-output mode.
        std::vector<int> new_active;
        if (state.broadcast_to_all) {
            for (const auto& cfg : configs)
                if (cfg.type == OutputStreamType::Laser && cfg.enabled)
                    new_active.push_back(cfg.id);
        } else {
            // Keep whatever the streams panel selected, but drop IDs no longer patched.
            for (int id : state.active_stream_ids) {
                bool still_exists = false;
                for (const auto& cfg : configs)
                    if (cfg.id == id) { still_exists = true; break; }
                if (still_exists)
                    new_active.push_back(id);
            }
        }
        if (new_active != state.active_stream_ids) {
            engine->send(cmd::SetActiveStreams{ new_active });
            state.active_stream_ids = new_active;
        }

        // ── 8. Propagate CITP stream names into UIState ────────────────────────
        for (auto& po : state.patched_outputs) {
            if (po.type != OutputStreamType::Laser) continue;
            auto it = laser_managers_.find(po.id);
            if (it != laser_managers_.end())
                po.citp_stream_name = it->second->citp_stream_name();
        }

        // Output patch changes are user edits — mark the project modified so
        // autosave fires even when no cuelist content has changed.
        // do_load_project resets this flag after all restoration is complete.
        state.project_dirty = true;
    };

    // Active stream selection: which outputs the programmer sends its frame to.
    // When broadcast_to_all is false the authoritative source is
    // selected_output_ids; use it directly to prevent stale IDs from a snapshot
    // overwrite from silently undoing a dot-click selection.
    // When broadcast_to_all is true pass the caller-provided list (all IDs).
    layout_cbs.on_active_streams_changed = [this](const std::vector<int>& old_ids,
                                                   const std::vector<int>& ids) {
        // Per-head latch model: when a stream is removed from the selection,
        // freeze its current programmer content onto it permanently (until CLR).
        // NOTE: do NOT send ClearProgrammer here — that would wipe the latches.
        // The engine stops sending programmer when active_stream_ids_ is empty
        // (programmer_will_run requires !active_stream_ids_.empty()).
        bool any_deselected = false;
        for (int old_id : old_ids) {
            if (std::find(ids.begin(), ids.end(), old_id) == ids.end()) {
                KeyframeLayer kf;
                kf.objects       = layout_ctx.frame_editor.objects;
                kf.symmetry_mode = static_cast<int>(layout_ctx.frame_editor.symmetry);
                // Check if there is actually programmer content to latch
                bool has_objects = !kf.objects.empty();
                bool has_global_fx = !layout_ctx.programmer_global.fx.empty();
                // Use per-stream FX if available, otherwise fall back to global programmer FX
                const std::string stream_key = std::to_string(old_id);
                FxLayer latch_fx = layout_ctx.programmer_fx_layer;
                auto feed_it = layout_ctx.programmer_feeds.find(stream_key);
                bool has_stream_fx = (feed_it != layout_ctx.programmer_feeds.end() &&
                                      !feed_it->second.fx.fx.empty());
                if (has_stream_fx)
                    latch_fx = feed_it->second.fx;
                bool has_fx = !latch_fx.fx.empty();
                // Only latch if there is content to preserve
                if (has_objects || has_global_fx || has_fx) {
                    engine->send(cmd::LatchProgrammer{
                        { old_id },
                        kf,
                        layout_ctx.programmer_global,
                        latch_fx
                    });
                }
                any_deselected = true;
            }
        }
        // Clear the UI editor only when ALL streams are gone AND INCL is not armed.
        // If streams remain, the programmer stays live for the remaining selected heads.
        // If INCL is armed (or a cue is already included), preserve content for UPDT.
        if (any_deselected && ids.empty()
            && !layout_ctx.incl_armed
            && layout_ctx.included_cue_idx < 0
            && layout_ctx.included_pb_id < 0) {
            layout_ctx.frame_editor.objects.clear();
            layout_ctx.programmer_global   = {};
            layout_ctx.programmer_fx_layer = {};
        }

        engine->send(cmd::SetActiveStreams{ ids });
        state.active_stream_ids = ids;
    };
    layout_cbs.on_clear_programmer = [this]() {
        // ChamSys rule: explicit CLR button is the ONLY path that wipes
        // per-stream programmer content in the engine.
        engine->send(cmd::ClearProgrammer{});
    };
    layout_cbs.on_mirrored_streams_changed = [this](const std::vector<int>& ids) {
        engine->send(cmd::SetMirroredStreams{ ids });
    };

    // ── Timeline callbacks ────────────────────────────────────────────────────
    layout_cbs.on_timeline_create = [this](TimelineDef def) {
        project->timelines.push_back(def);
        engine->send(cmd::CreateTimeline{ std::move(def) });
    };
    layout_cbs.on_timeline_delete = [this](const std::string& id) {
        auto it = std::find_if(project->timelines.begin(), project->timelines.end(),
                               [&](const TimelineDef& d){ return d.id == id; });
        if (it != project->timelines.end()) project->timelines.erase(it);
        engine->send(cmd::DeleteTimeline{ id });
    };
    layout_cbs.on_timeline_rename = [this](const std::string& id, const std::string& name) {
        for (auto& d : project->timelines) if (d.id == id) { d.name = name; break; }
        engine->send(cmd::RenameTimeline{ id, name });
    };
    layout_cbs.on_timeline_set_armed = [this](const std::string& id, bool armed) {
        engine->send(cmd::SetTimelineArmed{ id, armed });
    };
    layout_cbs.on_timeline_set_record_armed = [this](const std::string& id, bool armed) {
        engine->send(cmd::SetTimelineRecordArmed{ id, armed });
    };
    layout_cbs.on_timeline_play = [this](const std::string& id) {
        engine->send(cmd::TimelinePlay{ id });
    };
    layout_cbs.on_timeline_pause = [this](const std::string& id) {
        engine->send(cmd::TimelinePause{ id });
    };
    layout_cbs.on_timeline_stop = [this](const std::string& id) {
        engine->send(cmd::TimelineStop{ id });
    };
    layout_cbs.on_timeline_rewind = [this](const std::string& id) {
        engine->send(cmd::TimelineRewind{ id });
    };
    layout_cbs.on_timeline_seek = [this](const std::string& id, int64_t frame) {
        engine->send(cmd::TimelineSeek{ id, frame });
    };
    layout_cbs.on_timeline_set_source = [this](const std::string& id,
                                                const std::string& slot) {
        for (auto& d : project->timelines) if (d.id == id) { d.tc_slot = slot; break; }
        engine->send(cmd::SetTimelineSource{ id, slot });
    };
    layout_cbs.on_timeline_set_link = [this](const std::string& id, bool link) {
        for (auto& d : project->timelines) if (d.id == id) { d.link_mode = link; break; }
        engine->send(cmd::SetTimelineLink{ id, link });
    };
    layout_cbs.on_timeline_set_offset = [this](const std::string& id, int64_t off) {
        for (auto& d : project->timelines)
            if (d.id == id) { d.time_offset_frames = off; break; }
        engine->send(cmd::SetTimelineOffset{ id, off });
    };
    layout_cbs.on_timeline_add_track = [this](const std::string& tid,
                                               const std::string& name) {
        engine->send(cmd::AddTimelineTrack{ tid, name });
    };
    layout_cbs.on_timeline_remove_track = [this](const std::string& tid, int track_id) {
        engine->send(cmd::RemoveTimelineTrack{ tid, track_id });
    };
    layout_cbs.on_timeline_update_track = [this](const std::string& tid,
                                                  int track_id,
                                                  std::string name,
                                                  bool muted, bool locked, bool collapsed) {
        engine->send(cmd::UpdateTimelineTrack{ tid, track_id, name, muted, locked, collapsed });
    };
    layout_cbs.on_timeline_add_event = [this](const std::string& tid, int track_id,
                                               TimelineEvent ev) {
        engine->send(cmd::AddTimelineEvent{ tid, track_id, std::move(ev) });
    };
    layout_cbs.on_timeline_remove_event = [this](const std::string& tid, int track_id,
                                                   int64_t event_id) {
        engine->send(cmd::RemoveTimelineEvent{ tid, track_id, event_id });
    };
    layout_cbs.on_timeline_update_event = [this](const std::string& tid, int track_id,
                                                   TimelineEvent ev) {
        engine->send(cmd::UpdateTimelineEvent{ tid, track_id, std::move(ev) });
    };
    layout_cbs.on_timeline_wait_for_go = [this](const std::string& id) {
        engine->send(cmd::TimelineWaitForGoAdvance{ id });
    };
    layout_cbs.on_tc_config_changed = [this](ProjectTimecodeConfig cfg) {
        project->tc_config = cfg;
        engine->send(cmd::SetTimecodeSettings{ std::move(cfg) });
    };
    layout_cbs.on_timeline_set_audio = [this](const std::string& id, AudioTrackDef track) {
        // Compute peak envelope here on the UI thread (blocking decode is fine here;
        // it must NOT happen on the 1000 Hz engine thread).
        std::vector<float> peaks = audio_compute_peaks(track.file_path);
        engine->send(cmd::SetTimelineAudio{ id, std::move(track), std::move(peaks) });
    };
    layout_cbs.on_timeline_clear_audio = [this](const std::string& id) {
        engine->send(cmd::ClearTimelineAudio{ id });
    };
}

void Application::Impl::wire_shortcuts() {
    shortcuts.register_all(
        state,
        layout_cbs.on_new_project,
        layout_cbs.on_open_project,
        layout_cbs.on_save_project,
        layout_cbs.on_save_as,
        layout_cbs.on_undo,
        layout_cbs.on_redo,
        [this]() { palette.open(); },           // Ctrl+K
        layout_cbs.on_fullscreen,
        [this]() { if (layout_cbs.on_cue_duplicate) layout_cbs.on_cue_duplicate(state.active_cue_idx); },
        [this]() { if (layout_cbs.on_cue_delete)    layout_cbs.on_cue_delete(state.active_cue_idx); },
        []() { /* Ctrl+A: select-all is handled locally by each focused panel */ },
        layout_cbs.on_help
    );
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loading screen frame pump
//  Call this during init() after DX12 + ImGui backends are ready.
//  Updates loading_status_ / loading_progress_ before calling.
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::pump_loading_frame() {
    // Poll events so the OS doesn't consider the window unresponsive.
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        ImGui_ImplSDL3_ProcessEvent(&ev);
        if (ev.type == SDL_EVENT_WINDOW_RESIZED) {
#if defined(_WIN32)
            dx12_resize(dx12, ev.window.data1, ev.window.data2);
#endif
        }
        // Ignore SDL_EVENT_QUIT during loading — can't quit mid-init cleanly.
    }

#if defined(_WIN32)
    UINT frame_idx = dx12.frame_index;
    dx12_wait_for_frame(dx12, frame_idx);
    dx12.cmd_allocs[frame_idx]->Reset();
    dx12.cmd_list->Reset(dx12.cmd_allocs[frame_idx], nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = dx12.backbufs[frame_idx];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dx12.cmd_list->ResourceBarrier(1, &barrier);

    const float clear_color[4] = { kClearColor.x, kClearColor.y, kClearColor.z, kClearColor.w };
    dx12.cmd_list->ClearRenderTargetView(dx12.rtv_handles[frame_idx], clear_color, 0, nullptr);
    dx12.cmd_list->OMSetRenderTargets(1, &dx12.rtv_handles[frame_idx], FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { dx12.srv_heap };
    dx12.cmd_list->SetDescriptorHeaps(1, heaps);
#endif

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    // Full-viewport loading window (no title bar, no decoration)
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowBgAlpha(1.f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40.f, 40.f));
    ImGui::Begin("##loading", nullptr,
        ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize    |
        ImGuiWindowFlags_NoCollapse  | ImGuiWindowFlags_NoMove      |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);

    // -- Logo text, centered horizontally
    {
        ImGui::SetWindowFontScale(3.0f);
        const char* logo = "IDHMFIS";
        ImVec2 text_size = ImGui::CalcTextSize(logo);
        float center_x = (io.DisplaySize.x - text_size.x) * 0.5f - 40.f; // subtract padding
        if (center_x > 0.f)
            ImGui::SetCursorPosX(center_x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.95f, 1.0f, 1.f));
        ImGui::TextUnformatted(logo);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // -- Info text box (read-only, minimal border, low-alpha bg)
    {
        static char kInfoText[] =
            "IDHMFIS \xe2\x80\x94 Integrated Dynamic High-Fidelity Multi-Interface Show System\n"
            "Version: 0.1.0-dev  |  Build: May 17 2026  |  Platform: Windows x64\n"
            "\n"
            "Professional laser show control and output software.\n"
            "Supports ILDA, EtherDream, Helios, NDI, CITP/CAEX, and virtual preview outputs.";

        ImGui::PushStyleColor(ImGuiCol_FrameBg,     ImVec4(1.f, 1.f, 1.f, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_Border,       ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_Text,         ImVec4(0.7f, 0.75f, 0.8f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);

        float box_width  = io.DisplaySize.x - 80.f; // account for window padding on both sides
        float box_height = ImGui::GetTextLineHeightWithSpacing() * 6.f;
        ImGui::InputTextMultiline(
            "##info_box",
            kInfoText,
            std::strlen(kInfoText) + 1,
            ImVec2(box_width, box_height),
            ImGuiInputTextFlags_ReadOnly);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
    }

    ImGui::Spacing();
    ImGui::Spacing();

    // -- Progress bar spanning full available width, status text as overlay
    ImGui::ProgressBar(loading_progress_, ImVec2(-1.f, 0.f), loading_status_.c_str());

    ImGui::End();
    ImGui::PopStyleVar();

    ImGui::Render();

#if defined(_WIN32)
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12.cmd_list);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    dx12.cmd_list->ResourceBarrier(1, &barrier);
    dx12.cmd_list->Close();

    ID3D12CommandList* lists[] = { dx12.cmd_list };
    dx12.cmd_queue->ExecuteCommandLists(1, lists);

    dx12.swapchain->Present(1, 0);

    UINT64 fence_val = ++dx12.fence_vals[frame_idx];
    dx12.cmd_queue->Signal(dx12.fence, fence_val);
    dx12.fence_vals[frame_idx] = fence_val;
    dx12.frame_index = dx12.swapchain->GetCurrentBackBufferIndex();
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  Init
// ─────────────────────────────────────────────────────────────────────────────
bool Application::Impl::init() {
#if defined(_WIN32)
    // Raw diagnostic trace — written even if the logger fails to open.
    auto raw_trace = [](const char* msg) {
        char appdata[MAX_PATH]{};
        if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) == 0) return;
        char path[MAX_PATH + 64]{};
        std::snprintf(path, sizeof(path), "%s\\IDHMFIS\\logs\\init_trace.log", appdata);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "a") == 0 && f) {
            fputs(msg, f); fputs("\n", f); fflush(f); fclose(f);
        }
    };
    raw_trace("=== init() START ===");

    // Install vectored exception handler to catch AV in any thread.
    struct VehGuard {
        PVOID h;
        VehGuard() {
            h = AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* ep) -> LONG {
                if (ep->ExceptionRecord->ExceptionCode == 0xE06D7363) // C++ exception
                    return EXCEPTION_CONTINUE_SEARCH;
                char appdata[MAX_PATH]{};
                if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
                    char path[MAX_PATH + 64]{}, msg[128]{};
                    std::snprintf(path, sizeof(path), "%s\\IDHMFIS\\logs\\init_trace.log", appdata);
                    std::snprintf(msg, sizeof(msg), "VEH: exception code=0x%08X addr=%p",
                        (unsigned)ep->ExceptionRecord->ExceptionCode,
                        ep->ExceptionRecord->ExceptionAddress);
                    FILE* f = nullptr;
                    if (fopen_s(&f, path, "a") == 0 && f) {
                        fputs(msg, f); fputs("\n", f); fflush(f); fclose(f);
                    }
                }
                return EXCEPTION_CONTINUE_SEARCH;
            });
        }
        ~VehGuard() { if (h) RemoveVectoredExceptionHandler(h); }
    } veh_guard;
#endif

    // Logger: write to %APPDATA%\IDHMFIS\logs\  (never CWD / desktop)
#if defined(_WIN32)
    {
        char appdata[MAX_PATH]{};
        if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
            std::string log_dir = std::string(appdata) + "\\IDHMFIS\\logs";
            log::init(log_dir);
        }
    }
    raw_trace("logger init done");
#else
    log::init("/tmp/idhmfis/logs");
#endif

    log::info("init: log ready");

    // Load persisted app settings and start developer logging if previously enabled
    {
        bool dev_logging = load_developer_logging_setting();
        layout_ctx.developer_logging = dev_logging;
        if (dev_logging) {
            std::string dev_log_path = get_dev_log_path();
            log::enable_dev_logging(dev_log_path);
            log::info("init: developer logging enabled -> %s", dev_log_path.c_str());
        }
    }

    // Ensure the Showfiles directory exists (prefers project-root Showfiles,
    // falls back to a folder adjacent to the executable).
#if defined(_WIN32)
    {
        std::string sf_dir = win32_get_showfiles_dir();
        CreateDirectoryA(sf_dir.c_str(), nullptr);
    }
#endif

    log::info("init: SDL_Init starting");
    raw_trace("SDL_Init starting");
    // SDL3 init
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        raw_trace("SDL_Init FAILED");
        return false;
    }
    log::info("init: SDL_Init ok");
    raw_trace("SDL_Init ok");

    SDL_WindowFlags sdl_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(_WIN32)
    // DX12 doesn't need an SDL renderer flag
#else
    sdl_flags |= SDL_WINDOW_VULKAN;
#endif

    window = SDL_CreateWindow("IDHMFIS  v" IDHMFIS_VERSION,
                              kInitialWidth, kInitialHeight,
                              sdl_flags);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        log::error("init: SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }
    log::info("init: window created (%dx%d)", kInitialWidth, kInitialHeight);

#if defined(_WIN32)
    // Get native HWND from SDL3
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    HWND hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd) {
        SDL_Log("Could not get HWND from SDL window");
        return false;
    }
    log::info("init: DX12 init starting");
    raw_trace("DX12 starting");
    if (!dx12_create_device(hwnd, kInitialWidth, kInitialHeight, dx12)) {
        SDL_Log("DX12 device creation failed");
        raw_trace("DX12 FAILED");
        return false;
    }
    log::info("init: DX12 ok");
    raw_trace("DX12 ok");
#else
    // macOS/Linux Vulkan init would happen here.
    // Abbreviated for portability — full VkInstance / device creation follows
    // the official imgui example in examples/example_sdl3_vulkan/.
    SDL_Log("Vulkan backend selected (macOS/Linux)");
    // ... (vulkan init code here) ...
#endif

    // ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // ViewportsEnable requires per-viewport DX12 swapchains — not set up here; leave disabled
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr;  // DockBuilder owns the layout; prevent stale ini from overriding it

    // Font: load Segoe UI for clean anti-aliased rendering on Windows
    {
        bool loaded = false;
#if defined(_WIN32)
        // Get system DPI for font scaling
        float dpi_scale = 1.0f;
        if (HDC screen_dc = GetDC(nullptr)) {
            int dpi = GetDeviceCaps(screen_dc, LOGPIXELSX);
            ReleaseDC(nullptr, screen_dc);
            dpi_scale = static_cast<float>(dpi) / 96.f;
        }
        float font_size = std::clamp(std::round(15.f * dpi_scale), 13.f, 22.f);
        ImFontConfig cfg;
        cfg.OversampleH = 3;
        cfg.OversampleV = 2;
        cfg.PixelSnapH  = false;
        // Try Segoe UI first (Windows 7+), then Arial as fallback
        for (const char* path : { "C:/Windows/Fonts/segoeui.ttf",
                                   "C:/Windows/Fonts/arial.ttf" }) {
            if (io.Fonts->AddFontFromFileTTF(path, font_size, &cfg)) {
                loaded = true;
                break;
            }
        }
#endif
        if (!loaded) {
            ImFontConfig cfg2;
            cfg2.SizePixels = 15.f;
            io.Fonts->AddFontDefault(&cfg2);
        }
    }

    // Apply theme
    theme::apply_dark();

    // Platform/Renderer backends
    ImGui_ImplSDL3_InitForOther(window);

#if defined(_WIN32)
    ImGui_ImplDX12_InitInfo dx12_info{};
    dx12_info.Device              = dx12.device;
    dx12_info.CommandQueue        = dx12.cmd_queue;
    dx12_info.NumFramesInFlight   = kDX12BackbufCount;
    dx12_info.RTVFormat           = DXGI_FORMAT_R8G8B8A8_UNORM;
    dx12_info.SrvDescriptorHeap   = dx12.srv_heap;
    // Allocate SRV descriptors for ImGui font texture
    dx12_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* info,
                                         D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu,
                                         D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
        // Simple bump allocator in slot 0 of our SRV heap
        static int alloc_slot = 0;
        ID3D12DescriptorHeap* heap = info->SrvDescriptorHeap;
        UINT incr = info->Device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        out_cpu->ptr = heap->GetCPUDescriptorHandleForHeapStart().ptr + alloc_slot * incr;
        out_gpu->ptr = heap->GetGPUDescriptorHandleForHeapStart().ptr + alloc_slot * incr;
        ++alloc_slot;
    };
    dx12_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE,
                                        D3D12_GPU_DESCRIPTOR_HANDLE) { /* no-op */ };
    ImGui_ImplDX12_Init(&dx12_info);
#else
    // ImGui_ImplVulkan_Init would go here
    (void)0;
#endif

    // Loading screen is now safe to pump — DX12 + ImGui backends are ready.
    loading_status_   = "Initializing renderer...";
    loading_progress_ = 0.05f;
    pump_loading_frame();

    // ── Start show engine ─────────────────────────────────────────────────────
    log::info("init: constructing ShowEngine");
    raw_trace("ShowEngine constructing");
    engine   = std::make_unique<ShowEngine>(render_bus, audio);
    watchdog = std::make_unique<EngineWatchdog>(*engine);
    log::info("init: ShowEngine constructed");
    log::debug("engine: started at %d Hz target", kTargetFPS);
    raw_trace("ShowEngine constructed");

    loading_status_   = "Starting engine...";
    loading_progress_ = 0.15f;
    pump_loading_frame();

    // Load recent files list from disk and populate UI state
    {
        auto rf = RecentFiles::load();
        rf.prune_missing();
        state.recent_files.clear();
        for (const auto& e : rf.entries())
            state.recent_files.push_back(e.path);
    }

    // Create a blank demo project to seed the engine while the startup dialog
    // is displayed. The startup dialog will replace this with the user's choice.
    project = build_demo_project();
    state.project_name = project->name;
    engine->send(cmd::LoadProject{ project });
    show_startup_dialog = true;

    // Check if EULA was already accepted on a previous launch.
#if defined(_WIN32)
    {
        char appdata[MAX_PATH]{};
        if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
            std::string flag = std::string(appdata) + "\\IDHMFIS\\eula_accepted";
            FILE* f = nullptr;
            if (fopen_s(&f, flag.c_str(), "r") == 0 && f) {
                fclose(f);
                safety_eula_accepted = true;
                safety_eula_checkbox = true;
            }
        }
    }
#endif

    loading_status_   = "Loading project...";
    loading_progress_ = 0.25f;
    pump_loading_frame();

    // Start the 1000 Hz engine thread
    log::info("init: engine->start()");
    raw_trace("engine->start()");
    engine->start();
    raw_trace("engine->start() done");
    if (!safety_eula_accepted) {
        // Laser output stays disabled until the user accepts the EULA safety dialog.
        engine->send(cmd::SetOutputEnable{false});
        state.output_enabled = false;
    }
    log::info("init: watchdog->start()");
    watchdog->start();
    log::info("init: engine threads started");
    raw_trace("engine threads started");

    loading_status_   = "Scanning for DAC devices...";
    loading_progress_ = 0.35f;
    pump_loading_frame();

    // Restore output patch from project file (if any).
    // If the project has a saved patch, restore it into UIState exactly.
    // If not (new show or old file), start with an empty patch — the user
    // builds their own via the Add button in the Patch view.
    state.patched_outputs.clear();
    if (!project->output_patch.empty()) {
        for (const auto& cfg : project->output_patch) {
            UIState::PatchedOutput po;
            po.id          = cfg.id;
            po.name        = cfg.name;
            po.type        = cfg.type;
            po.enabled     = cfg.enabled;
            po.dac_type    = cfg.dac_type;
            po.dac_address = cfg.dac_address;
            po.config      = cfg;
            state.patched_outputs.push_back(std::move(po));
        }
        layout_ctx.patch_next_id = project->output_patch_next_id;
        // Select first output so something is visible right away
        if (!state.patched_outputs.empty())
            layout_ctx.patch_selected_id = state.patched_outputs.front().id;
    }
    // patch_next_id default (layout.h) is 2; honour project value if larger
    if (project->output_patch_next_id > layout_ctx.patch_next_id)
        layout_ctx.patch_next_id = project->output_patch_next_id;
    // Restore output groups
    {
        state.output_groups.clear();
        state.output_groups.reserve(project->output_groups.size());
        for (const auto& pg : project->output_groups) {
            UIState::OutputGroup g;
            g.id           = pg.id;
            g.name         = pg.name;
            g.member_ids   = pg.member_ids;
            g.mirrored_ids = pg.mirrored_ids;
            state.output_groups.push_back(std::move(g));
        }
        state.output_group_next_id = project->output_group_next_id;
    }

    loading_status_   = "Starting output managers...";
    loading_progress_ = 0.50f;
    pump_loading_frame();

    // Fire on_output_patch_changed to create all DacManagers / HDMI windows
    // from the restored patch and send SetOutputPatch + SetActiveStreams to the engine.
    // This is the single authoritative path: callbacks are registered in wire_callbacks()
    // which runs later, so we replicate the reconciliation directly here at startup.
    {
        std::vector<OutputStreamConfig> init_cfgs;
        init_cfgs.reserve(state.patched_outputs.size());
        for (const auto& po : state.patched_outputs)
            init_cfgs.push_back(po.config);

        // Create DacManagers for all Laser outputs
        int laser_ord = 0;
        for (const auto& cfg : init_cfgs) {
            if (cfg.type != OutputStreamType::Laser) continue;
            int citp_ord = laser_ord++;
            RenderBus* bus = engine->extra_laser_bus(citp_ord);
            auto mgr = std::make_unique<DacManager>(*bus, citp_ord);
            if (cfg.dac_type != "auto" && !cfg.dac_type.empty())
                mgr->force_dac(cfg.dac_type, cfg.dac_address);
            if (cfg.point_rate > 0)
                mgr->set_point_rate(cfg.point_rate);
            DacManager* raw = mgr.get();
            std::thread([raw]{ raw->start(); }).detach();
            log::info("init: started DacManager for output id=%d", cfg.id);
            laser_managers_.emplace(cfg.id, std::move(mgr));
        }

        // Create SDL windows for HDMI outputs
        for (const auto& cfg : init_cfgs) {
            if (cfg.type != OutputStreamType::HDMI) continue;
            SDL_Window* w = SDL_CreateWindow(cfg.name.c_str(),
                                             1920, 1080,
                                             SDL_WINDOW_BORDERLESS | SDL_WINDOW_RESIZABLE);
            if (!w)
                w = SDL_CreateWindow(cfg.name.c_str(), 1920, 1080, 0);
            if (w) {
                SDL_SetWindowPosition(w, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
                SDL_ShowWindow(w);
                SDL_RaiseWindow(w);
                hdmi_windows_[cfg.id] = w;
            }
        }

        // Send patch to engine
        engine->send(cmd::SetOutputPatch{ init_cfgs });

        // Set active streams: all enabled laser outputs receive programmer content.
        std::vector<int> laser_ids;
        for (const auto& cfg : init_cfgs)
            if (cfg.type == OutputStreamType::Laser && cfg.enabled)
                laser_ids.push_back(cfg.id);
        engine->send(cmd::SetActiveStreams{ laser_ids });
        state.active_stream_ids = laser_ids;

        // Propagate CITP stream names
        for (auto& po : state.patched_outputs) {
            if (po.type != OutputStreamType::Laser) continue;
            auto it = laser_managers_.find(po.id);
            if (it != laser_managers_.end())
                po.citp_stream_name = it->second->citp_stream_name();
        }
    }

    // Auto-initialize NDI output with project defaults
    engine->send(cmd::SetNdiConfig{
        "IDHMFIS",
        project->ndi_width,
        project->ndi_height,
        project->ndi_fps_N,
        project->ndi_fps_D,
        true,
        project->ndi_clock_video
    });

    // Restore ArtNet output settings from project
    state.output_config.artnet_out_ip              = project->artnet_out_ip;
    state.output_config.artnet_out_universe_offset = project->artnet_out_universe_offset;
    state.output_config.artnet_out_enabled         = project->artnet_out_enabled;
    if (project->artnet_out_enabled) {
        engine->send(cmd::SetArtNetOutput{
            project->artnet_out_ip, project->artnet_out_universe_offset, true
        });
    }

    // Restore enabled output stream types from project
    {
        using Kind = cmd::SetStreamTypeEnabled::Kind;
        state.output_config.stream_type_dac_enabled    = project->output_config.stream_type_dac_enabled;
        state.output_config.stream_type_ndi_enabled    = project->output_config.stream_type_ndi_enabled;
        state.output_config.stream_type_artnet_enabled = project->output_config.stream_type_artnet_enabled;
        state.output_config.stream_type_idn_enabled    = project->output_config.stream_type_idn_enabled;
        engine->send(cmd::SetStreamTypeEnabled{ Kind::DAC,    state.output_config.stream_type_dac_enabled    });
        engine->send(cmd::SetStreamTypeEnabled{ Kind::NDI,    state.output_config.stream_type_ndi_enabled    });
        engine->send(cmd::SetStreamTypeEnabled{ Kind::ArtNet, state.output_config.stream_type_artnet_enabled });
        engine->send(cmd::SetStreamTypeEnabled{ Kind::IDN,    state.output_config.stream_type_idn_enabled    });
        // IDN propagates to DacManagers (created above in this function)
        for (auto& kv : laser_managers_)
            kv.second->set_idn_sidecar_enabled(state.output_config.stream_type_idn_enabled);
    }

    // Restore NDI fps state and clock mode
    state.output_config.ndi_fps_N       = project->ndi_fps_N;
    state.output_config.ndi_fps_D       = project->ndi_fps_D;
    state.output_config.ndi_clock_video = project->ndi_clock_video;

    loading_status_   = "Configuring outputs...";
    loading_progress_ = 0.60f;
    pump_loading_frame();

    // Start background thumbnail renderer
    thumbnailer.start();

    // Init audio (WASAPI loopback, non-fatal if it fails)
    log::info("audio: starting WASAPI loopback analyzer");
    audio.init(AudioAnalyzer::InputMode::WasapiLoopback);

    loading_status_   = "Starting audio analyzer...";
    loading_progress_ = 0.70f;
    pump_loading_frame();

    // Initialize palettes with defaults
    state.color_palette.reset_to_defaults();
    state.position_palette.reset_to_defaults();

    // Default color swatches — RGBCMY + secondaries + neutrals (slots 0-15)
    auto sset = [&](int i, float r, float g, float b) {
        state.color_swatches[i] = { r, g, b, true };
    };
    sset(0,  1.f,   0.f,   0.f);   // Red
    sset(1,  0.f,   1.f,   0.f);   // Green
    sset(2,  0.f,   0.f,   1.f);   // Blue
    sset(3,  0.f,   1.f,   1.f);   // Cyan
    sset(4,  1.f,   0.f,   1.f);   // Magenta
    sset(5,  1.f,   1.f,   0.f);   // Yellow
    sset(6,  1.f,   1.f,   1.f);   // White
    sset(7,  0.f,   0.f,   0.f);   // Black
    sset(8,  1.f,   0.5f,  0.f);   // Orange
    sset(9,  0.5f,  1.f,   0.f);   // Lime
    sset(10, 0.f,   1.f,   0.5f);  // Spring
    sset(11, 0.f,   0.5f,  1.f);   // Sky
    sset(12, 0.5f,  0.f,   1.f);   // Violet
    sset(13, 1.f,   0.f,   0.5f);  // Rose
    sset(14, 1.f,   0.9f,  0.7f);  // Warm White
    sset(15, 0.7f,  0.9f,  1.f);   // Cool White
    // Slots 16-31 remain unset (user-recordable)

    loading_status_   = "Loading generators...";
    loading_progress_ = 0.80f;
    pump_loading_frame();

    // Default keybinds
    using KB = UIState::KeybindEntry;
    state.keybinds = {
        { "Draw Line",         "draw_line",      static_cast<int>(ImGuiKey_G) },
        { "Draw Dot",          "draw_dot",       static_cast<int>(ImGuiKey_R) },
        { "Draw Bezier",       "draw_bezier",    static_cast<int>(ImGuiKey_Y) },
        { "Draw Arc",          "draw_arc",       static_cast<int>(ImGuiKey_P) },
        { "Draw Circle",       "draw_circle",    static_cast<int>(ImGuiKey_L) },
        { "Select",            "select",         static_cast<int>(ImGuiKey_S) },
        { "Snap-to-grid",      "snap_grid",      static_cast<int>(ImGuiKey_C) },
        { "View: Overview",    "view_overview",  static_cast<int>(ImGuiKey_O) },
        { "View: QuickShow",   "view_quickshow", static_cast<int>(ImGuiKey_Q) },
        { "View: LivePRO",     "view_livepro",   static_cast<int>(ImGuiKey_U) },
        { "View: 3D",          "view_3d",        static_cast<int>(ImGuiKey_B) },
        { "View: Timeline",    "view_timeline",  static_cast<int>(ImGuiKey_D) },
        { "View: Cue Library", "view_cue_lib",   static_cast<int>(ImGuiKey_T) },
    };

    // Initial state sync before first frame
    sync_state_from_engine();

    loading_status_   = "Wiring UI...";
    loading_progress_ = 0.90f;
    pump_loading_frame();

    // Wire callbacks and shortcuts
    wire_callbacks();
    wire_shortcuts();

    // Build command palette entries
    palette.build_entries(state, shortcuts);

    loading_status_   = "Ready.";
    loading_progress_ = 1.00f;
    pump_loading_frame();

    running = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Frame
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::run_frame() {
    // ── Pull live state from engine ────────────────────────────────────────────
    sync_state_from_engine();

    // ── Window title: "IDHMFIS — <show name> [*]" ────────────────────────────
    {
        static std::string s_last_title;
        std::string title = "IDHMFIS v" IDHMFIS_VERSION " \xe2\x80\x94 ";
        title += state.project_name.empty() ? "New Show" : state.project_name;
        if (state.project_dirty) title += " *";
        if (title != s_last_title) {
            SDL_SetWindowTitle(window, title.c_str());
            s_last_title = title;
        }
    }

    // ── Auto-save ──────────────────────────────────────────────────────────────
    if (state.project_dirty)
        auto_save_.notify_dirty();
    if (project && auto_save_.is_due()) {
        // Flush engine-owned data into the project snapshot before writing
        project->full_cue_list = engine->read_full_cue_list();
        for (auto& pb : project->playbacks)
            pb.cuelist = engine->read_playback_cuelist(pb.id);
        // Flush UI-only fields that live in UIState / LayoutContext, not in project
        project->bpm_tap_key           = state.bpm_tap_key;
        project->emergency_shutoff_key = state.emergency_shutoff_key;
        for (auto& pb : project->playbacks) {
            int idx = pb.id;
            if (idx >= 0 && idx < UIState::kMaxPlaybacks) {
                pb.config.keyboard_go_key              = state.pb_conf[idx].keyboard_key;
                pb.config.fade_on_first_trigger        = state.pb_conf[idx].fade_on_first_trigger;
                pb.config.first_trigger_fade_s         = state.pb_conf[idx].first_trigger_fade_s;
                pb.config.remember_cuelist_position    = state.pb_conf[idx].remember_cuelist_position;
                pb.config.output_stream_ids            = state.pb_conf[idx].output_stream_ids;
            }
        }
        project->color_palette         = state.color_palette;
        project->position_palette      = state.position_palette;
        for (int i = 0; i < UIState::kNumSwatches; ++i) {
            project->color_swatches[i].r    = state.color_swatches[i].r;
            project->color_swatches[i].g    = state.color_swatches[i].g;
            project->color_swatches[i].b    = state.color_swatches[i].b;
            project->color_swatches[i].used = state.color_swatches[i].used;
        }
        // Flush safety blackout config
        {
            const auto& src = state.safety_blackout;
            auto& dst = project->safety_blackout;
            dst.enabled        = src.enabled;
            dst.borders.left   = src.borders.left;
            dst.borders.right  = src.borders.right;
            dst.borders.top    = src.borders.top;
            dst.borders.bottom = src.borders.bottom;
            dst.borders.tilt   = src.borders.tilt;
            dst.zones.clear();
            for (const auto& z : src.zones) {
                Project::SafetyBlackoutConfig::BlockZone pz;
                pz.enabled   = z.enabled;
                pz.name      = z.name;
                pz.cx        = z.cx;
                pz.cy        = z.cy;
                pz.hw        = z.hw;
                pz.hh        = z.hh;
                pz.angle_deg = z.angle_deg;
                dst.zones.push_back(pz);
            }
        }
        // Flush network config
        {
            const auto& src = state.net_config;
            auto& dst = project->net_config;
            dst.iface_auto          = src.iface_auto;
            dst.iface_index         = src.iface_index;
            dst.artnet_enabled      = src.artnet_enabled;
            dst.artnet_auto         = src.artnet_auto;
            dst.artnet_universe     = src.artnet_universe;
            dst.artnet_net          = src.artnet_net;
            dst.artnet_subnet       = src.artnet_subnet;
            dst.artnet_listen_ip    = src.artnet_listen_ip;
            dst.artnet_port         = src.artnet_port;
            dst.artnet_merge_htp    = src.artnet_merge_htp;
            dst.artnet_merge_mode   = src.artnet_merge_mode;
            dst.artnet_priority     = src.artnet_priority;
            dst.sacn_enabled        = src.sacn_enabled;
            dst.sacn_auto           = src.sacn_auto;
            dst.sacn_universe       = src.sacn_universe;
            dst.sacn_priority       = src.sacn_priority;
            dst.sacn_multicast_ip   = src.sacn_multicast_ip;
            dst.sacn_port           = src.sacn_port;
            dst.sacn_per_universe   = src.sacn_per_universe;
            dst.osc_in_enabled      = src.osc_in_enabled;
            dst.osc_in_auto         = src.osc_in_auto;
            dst.osc_in_port         = src.osc_in_port;
            dst.osc_in_ip           = src.osc_in_ip;
            dst.osc_out_enabled     = src.osc_out_enabled;
            dst.osc_out_auto        = src.osc_out_auto;
            dst.osc_out_ip          = src.osc_out_ip;
            dst.osc_out_port        = src.osc_out_port;
            dst.osc_prefix          = src.osc_prefix;
            dst.citp_enabled        = src.citp_enabled;
            dst.citp_auto           = src.citp_auto;
            dst.citp_tcp_port       = src.citp_tcp_port;
            dst.citp_multicast_group= src.citp_multicast_group;
            dst.citp_multicast_port = src.citp_multicast_port;
            dst.citp_source_name    = src.citp_source_name;
            dst.citp_respond_capture= src.citp_respond_capture;
            dst.idn_enabled         = src.idn_enabled;
            dst.idn_auto            = src.idn_auto;
            dst.idn_broadcast_addr  = src.idn_broadcast_addr;
            dst.idn_port            = src.idn_port;
            dst.idn_channel         = src.idn_channel;
            dst.cls_enabled         = src.cls_enabled;
            dst.cls_auto            = src.cls_auto;
            dst.cls_tcp_port        = src.cls_tcp_port;
            dst.cls_udp_port        = src.cls_udp_port;
            dst.cls_udp_broadcast   = src.cls_udp_broadcast;
            dst.etherdream_enabled  = src.etherdream_enabled;
            dst.etherdream_auto     = src.etherdream_auto;
            dst.etherdream_timeout  = src.etherdream_timeout;
            dst.etherdream_preferred_ip = src.etherdream_preferred_ip;
            dst.ndi_net_auto        = src.ndi_net_auto;
            dst.ndi_net_source_name = src.ndi_net_source_name;
            dst.ndi_net_bandwidth   = src.ndi_net_bandwidth;
            dst.ndi_net_fps         = src.ndi_net_fps;
        }
        // Flush output config
        {
            const auto& src = state.output_config;
            auto& dst = project->output_config;
            dst.ndi_enabled                = src.ndi_enabled;
            dst.ndi_name                   = src.ndi_name;
            dst.ndi_width                  = src.ndi_width;
            dst.ndi_height                 = src.ndi_height;
            dst.ndi_fps                    = src.ndi_fps;
            dst.ndi_fps_N                  = src.ndi_fps_N;
            dst.ndi_fps_D                  = src.ndi_fps_D;
            dst.ndi_clock_video            = src.ndi_clock_video;
            dst.beam_radius                = src.beam_radius;
            dst.glow_radius                = src.glow_radius;
            dst.glow_alpha                 = src.glow_alpha;
            dst.hdmi_window_enabled        = src.hdmi_window_enabled;
            dst.dac_enabled                = src.dac_enabled;
            dst.idn_stream_enabled         = src.idn_stream_enabled;
            dst.virtual_camera_enabled     = src.virtual_camera_enabled;
            dst.stream_type_dac_enabled    = src.stream_type_dac_enabled;
            dst.stream_type_ndi_enabled    = src.stream_type_ndi_enabled;
            dst.stream_type_artnet_enabled = src.stream_type_artnet_enabled;
            dst.stream_type_idn_enabled    = src.stream_type_idn_enabled;
        }
        // Flush 3D preview settings
        {
            const auto& src = state.preview_3d;
            auto& dst = project->preview_3d;
            dst.scan_mode       = src.scan_mode;
            dst.scan_speed      = src.scan_speed;
            dst.trail_pct       = src.trail_pct;
            dst.beam_brightness = src.beam_brightness;
            dst.haze_alpha      = src.haze_alpha;
            dst.wall_glow_px    = src.wall_glow_px;
            dst.beam_width_px   = src.beam_width_px;
        }
        // Flush laser placements
        {
            project->laser_placements.clear();
            for (const auto& lp : state.laser_placements) {
                Project::LaserPlacement3D plp;
                plp.stream_id = lp.stream_id;
                plp.pos_x     = lp.pos_x;
                plp.pos_y     = lp.pos_y;
                plp.pos_z     = lp.pos_z;
                plp.yaw       = lp.yaw;
                plp.pitch     = lp.pitch;
                project->laser_placements.push_back(plp);
            }
        }
        // Flush color input mode
        project->color_input_mode = static_cast<int>(state.color_input_mode);
        // Flush Otaniemi config
        {
            const auto& src = state.otaniemi;
            auto& dst = project->otaniemi;
            dst.enabled          = src.enabled;
            dst.line_thickness   = src.line_thickness;
            dst.auto_fill_shapes = src.auto_fill_shapes;
            dst.brightness_boost = src.brightness_boost;
            dst.glow_radius      = src.glow_radius;
        }
        project->active_stream_ids = state.active_stream_ids;
        // Flush ui_layout state so reopening the show restores the view
        project->ui_layout.active_view             = static_cast<int>(layout_ctx.active_view);
        project->ui_layout.show_layout_initialised = layout_ctx.show_layout_initialised;
        project->ui_layout.dockspace_initialised   = layout_ctx.dockspace_initialised;
        // Flush output patch from UIState back into the project document
        {
            project->output_patch.clear();
            project->output_patch.reserve(state.patched_outputs.size());
            for (const auto& po : state.patched_outputs)
                project->output_patch.push_back(po.config);
            project->output_patch_next_id = layout_ctx.patch_next_id;
        }
        // Flush output groups
        {
            project->output_groups.clear();
            project->output_groups.reserve(state.output_groups.size());
            for (const auto& g : state.output_groups) {
                Project::OutputGroup pg;
                pg.id           = g.id;
                pg.name         = g.name;
                pg.member_ids   = g.member_ids;
                pg.mirrored_ids = g.mirrored_ids;
                project->output_groups.push_back(std::move(pg));
            }
            project->output_group_next_id = state.output_group_next_id;
        }
    }
    if (project) {
        bool was_due = auto_save_.is_due();
        auto_save_.tick(*project, state.project_path);
        if (was_due)
            log::debug("autosave: triggered for \"%s\"", state.project_path.c_str());
    }

    // DAC connection status (will come from real subsystems later)
    state.dac_connected = false;

#if defined(_WIN32)
    UINT frame_idx = dx12.frame_index;
    dx12_wait_for_frame(dx12, frame_idx);
    dx12.cmd_allocs[frame_idx]->Reset();
    dx12.cmd_list->Reset(dx12.cmd_allocs[frame_idx], nullptr);

    // Transition backbuffer to render target
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = dx12.backbufs[frame_idx];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dx12.cmd_list->ResourceBarrier(1, &barrier);

    const float clear_color[4] = { kClearColor.x, kClearColor.y, kClearColor.z, kClearColor.w };
    dx12.cmd_list->ClearRenderTargetView(dx12.rtv_handles[frame_idx], clear_color, 0, nullptr);
    dx12.cmd_list->OMSetRenderTargets(1, &dx12.rtv_handles[frame_idx], FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { dx12.srv_heap };
    dx12.cmd_list->SetDescriptorHeaps(1, heaps);
#endif

    // New frame
    ImGui_ImplSDL3_NewFrame();
#if defined(_WIN32)
    ImGui_ImplDX12_NewFrame();
#else
    // ImGui_ImplVulkan_NewFrame()
#endif
    ImGui::NewFrame();

    // Fullscreen toggle
    if (want_fullscreen) {
        want_fullscreen = false;
        auto flags = SDL_GetWindowFlags(window);
        if (flags & SDL_WINDOW_FULLSCREEN)
            SDL_SetWindowFullscreen(window, false);
        else
            SDL_SetWindowFullscreen(window, true);
    }

    // ── Deferred show-file load ───────────────────────────────────────────────
    // The overlay was presented last frame; now perform the blocking load.
    if (!is_loading_show && !pending_load_path_.empty()) {
        std::string load_path = std::move(pending_load_path_);
        pending_load_path_.clear();
        do_load_project(load_path);
    }

    // ── Show-file loading overlay ─────────────────────────────────────────────
    // Rendered on top of everything else while a show file is being parsed.
    // The flag is cleared at the END of this block so the overlay shows for
    // exactly one frame (enough to prevent the user seeing OS cmd flashes and
    // to provide visual feedback before the blocking load begins next frame).
    if (is_loading_show) {
        ImGuiIO& lio = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
        ImGui::SetNextWindowSize(lio.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.f); // background cleared by clear_color
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin("##loading_show_overlay", nullptr,
            ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoSavedSettings);

        // Dark semi-transparent fill matching the Obsidian Laser theme (#0D0F12)
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(
            ImVec2(0.f, 0.f), lio.DisplaySize,
            IM_COL32(13, 15, 18, 220));

        // Animated spinner: cycles through | / - \ at ~5 Hz
        static const char* kSpinFrames[] = { "|", "/", "-", "\\" };
        int spin_idx = static_cast<int>(ImGui::GetTime() * 5.0) & 3;

        // Centered "Loading Show..." text in laser-cyan (#00E5FF)
        {
            const char* label = "Loading Show...";
            char full[32]{};
            std::snprintf(full, sizeof(full), "%s %s", label, kSpinFrames[spin_idx]);

            ImGui::SetWindowFontScale(1.6f);
            ImVec2 ts = ImGui::CalcTextSize(full);
            ImVec2 pos(
                (lio.DisplaySize.x - ts.x) * 0.5f,
                (lio.DisplaySize.y - ts.y) * 0.5f);
            dl->AddText(ImGui::GetFont(),
                        ImGui::GetFontSize() * 1.6f,
                        pos,
                        IM_COL32(0, 229, 255, 255),
                        full);
            ImGui::SetWindowFontScale(1.0f);
        }

        ImGui::End();
        ImGui::PopStyleVar();

        // Render and present this overlay frame, then clear the flag so the
        // actual load can proceed on the next Impl call-site.
        ImGui::Render();
#if defined(_WIN32)
        {
            UINT li_idx = dx12.frame_index;
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12.cmd_list);
            D3D12_RESOURCE_BARRIER li_barrier{};
            li_barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            li_barrier.Transition.pResource   = dx12.backbufs[li_idx];
            li_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            li_barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            li_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            dx12.cmd_list->ResourceBarrier(1, &li_barrier);
            dx12.cmd_list->Close();
            ID3D12CommandList* li_lists[] = { dx12.cmd_list };
            dx12.cmd_queue->ExecuteCommandLists(1, li_lists);
            dx12.swapchain->Present(1, 0);
            UINT64 li_fence_val = ++dx12.fence_vals[li_idx];
            dx12.cmd_queue->Signal(dx12.fence, li_fence_val);
            dx12.fence_vals[li_idx] = li_fence_val;
            dx12.frame_index = dx12.swapchain->GetCurrentBackBufferIndex();
        }
#endif
        is_loading_show = false;
        return; // skip main-UI rendering this frame
    }

    // ── Output patch applying overlay ─────────────────────────────────────────
    // Shown every frame while background DacManager::stop() threads are running.
    if (dacs_stopping_.load(std::memory_order_relaxed) > 0) {
        ImGuiIO& pio = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
        ImGui::SetNextWindowSize(pio.DisplaySize, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin("##patch_overlay", nullptr,
            ImGuiWindowFlags_NoTitleBar   | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoMove       | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoSavedSettings);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(0.f, 0.f), pio.DisplaySize, IM_COL32(13, 15, 18, 220));

        static const char* kSpin[] = { "|", "/", "-", "\\" };
        int spin_idx = static_cast<int>(ImGui::GetTime() * 5.0) & 3;
        char full[48]{};
        std::snprintf(full, sizeof(full), "Applying patch... %s", kSpin[spin_idx]);

        ImGui::SetWindowFontScale(1.6f);
        ImVec2 ts  = ImGui::CalcTextSize(full);
        ImVec2 pos((pio.DisplaySize.x - ts.x) * 0.5f,
                   (pio.DisplaySize.y - ts.y) * 0.5f);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.6f, pos,
                    IM_COL32(0, 229, 255, 255), full);
        ImGui::SetWindowFontScale(1.0f);

        ImGui::End();
        ImGui::PopStyleVar();

        ImGui::Render();
#if defined(_WIN32)
        {
            UINT pi_idx = dx12.frame_index;
            ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12.cmd_list);
            D3D12_RESOURCE_BARRIER pi_barrier{};
            pi_barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            pi_barrier.Transition.pResource   = dx12.backbufs[pi_idx];
            pi_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            pi_barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            pi_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            dx12.cmd_list->ResourceBarrier(1, &pi_barrier);
            dx12.cmd_list->Close();
            ID3D12CommandList* pi_lists[] = { dx12.cmd_list };
            dx12.cmd_queue->ExecuteCommandLists(1, pi_lists);
            dx12.swapchain->Present(1, 0);
            UINT64 pi_fence_val = ++dx12.fence_vals[pi_idx];
            dx12.cmd_queue->Signal(dx12.fence, pi_fence_val);
            dx12.fence_vals[pi_idx] = pi_fence_val;
            dx12.frame_index = dx12.swapchain->GetCurrentBackBufferIndex();
        }
#endif
        return;
    }

    // ── EULA / Safety gate ────────────────────────────────────────────────────
    // Must be accepted before any laser output or main UI is shown.
    if (!safety_eula_accepted) {
        // Darken the background
        ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::SetNextWindowBgAlpha(0.85f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin("##eula_bg", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoInputs  | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
        ImGui::PopStyleVar();

        // Centered dialog
        ImGuiIO& _io = ImGui::GetIO();
        float dlg_w = std::min(760.f, _io.DisplaySize.x - 40.f);
        ImGui::SetNextWindowPos(
            ImVec2(_io.DisplaySize.x * 0.5f, _io.DisplaySize.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(dlg_w, 0.f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.98f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg,  ImVec4(0.08f, 0.08f, 0.10f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Border,    ImVec4(0.8f,  0.25f, 0.1f,  1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(22.f, 18.f));
        ImGui::Begin("##eula_dlg", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove  |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);

        // ── Header
        ImGui::SetWindowFontScale(1.25f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.35f, 0.1f, 1.f));
        ImGui::TextUnformatted("\xe2\x9a\xa0 LASER SAFETY WARNING");
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── English block
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.85f, 1.0f, 1.f));
        ImGui::TextUnformatted("English");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.f));
        ImGui::TextWrapped(
            "This software controls Class 3B and Class 4 laser equipment capable of causing "
            "permanent eye injury, skin burns, and fire \xe2\x80\x94 including from reflections "
            "and at long distances. By continuing, you agree to the End User License Agreement "
            "and confirm that you understand the risks of operating laser equipment.");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // ── Finnish block
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.85f, 1.0f, 1.f));
        ImGui::TextUnformatted("Suomi");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.92f, 0.92f, 1.f));
        ImGui::TextWrapped(
            "T\xc3\xa4m\xc3\xa4 ohjelmisto ohjaa luokan 3B ja 4 laserlaitteita, jotka voivat "
            "aiheuttaa pysyv\xc3\xa4n silmav\xc3\xa4urion, palovammoja ja tulipalon \xe2\x80\x94 "
            "my\xc3\xb6s heijastusten kautta ja pitkilta et\xc3\xa4isyyksilt\xc3\xa4. "
            "Jatkamalla hyv\xc3\xa4ksyt k\xc3\xa4ytt\xc3\xb6oikeussopimuksen (EULA) ja vahvistat "
            "ymm\xc3\xa4rt\xc3\xa4v\xc3\xa4si laserlaitteen k\xc3\xa4ytt\xc3\xb6\xc3\xb6n liittyv\xc3\xa4t riskit.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Bright caution line
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.88f, 0.1f, 1.f));
        ImGui::TextWrapped(
            "ONLY CLICK CONTINUE IF YOU ARE USING A VIDEO PROJECTOR AND NOT A LASER, OR IF "
            "YOU HAVE THE PROPER TRAINING TO CONTINUE \xe2\x80\x94 DO NOT RISK BLINDNESS. "
            "IF YOU'RE UNSURE, STOP NOW.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── EULA hyperlink
        ImGui::PushStyleColor(ImGuiCol_Text,        ImVec4(0.35f, 0.75f, 1.f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_Button,       ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.f, 0.f, 0.f, 0.f));
        if (ImGui::SmallButton("View full EULA / Lue koko EULA")) {
#if defined(_WIN32)
            namespace fs = std::filesystem;
            fs::path eula = fs::weakly_canonical(
                fs::path(win32_get_exe_dir()) / ".." / ".." / "EULA_Laser_Control_Software.pdf");
            ShellExecuteA(nullptr, "open", eula.string().c_str(), nullptr, nullptr, SW_SHOW);
#endif
        }
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("EULA_Laser_Control_Software.pdf");
        ImGui::Spacing();

        // ── Acceptance checkbox
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1.f));
        ImGui::Checkbox(
            "I have read and accept the EULA and the safety warning above.\n"
            "Olen lukenut ja hyv\xc3\xa4ksy\xc3\xa4n EULAn sek\xc3\xa4 yll\xc3\xa4 olevan "
            "turvallisuusvaroituksen.",
            &safety_eula_checkbox);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Buttons
        float btn_w = 175.f;
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f,  0.1f,  0.1f,  1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f,  0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f,  0.2f,  0.2f,  1.f));
        if (ImGui::Button("Cancel / Peruuta", ImVec2(btn_w, 0.f)))
            std::terminate();
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.f, 12.f);

        if (!safety_eula_checkbox) ImGui::BeginDisabled();
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f,  0.45f, 0.1f,  1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.65f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f,  0.8f,  0.2f,  1.f));
        if (ImGui::Button("Continue / Jatka", ImVec2(btn_w, 0.f))) {
            safety_eula_accepted = true;
            engine->send(cmd::SetOutputEnable{true});
            state.output_enabled = true;
            // Persist acceptance so the dialog is not shown again.
#if defined(_WIN32)
            {
                char appdata[MAX_PATH]{};
                if (GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH) > 0) {
                    // Ensure directory exists.
                    std::string dir  = std::string(appdata) + "\\IDHMFIS";
                    std::string flag = dir + "\\eula_accepted";
                    CreateDirectoryA(dir.c_str(), nullptr);
                    FILE* f = nullptr;
                    if (fopen_s(&f, flag.c_str(), "w") == 0 && f) {
                        fputs("1", f); fclose(f);
                    }
                }
            }
#endif
        }
        ImGui::PopStyleColor(3);
        if (!safety_eula_checkbox) ImGui::EndDisabled();

        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    } else {
        // ── Startup dialog ────────────────────────────────────────────────────
        if (show_startup_dialog) {
            ImGui::OpenPopup("##startup_dialog");
            ImGui::SetNextWindowPos(
                ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                       ImGui::GetIO().DisplaySize.y * 0.5f),
                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.f, 24.f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg,  ImVec4(0.07f, 0.08f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg,   ImVec4(0.07f, 0.08f, 0.10f, 1.f));
            ImGui::PushStyleColor(ImGuiCol_Border,    ImVec4(0.25f, 0.45f, 0.70f, 1.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.f);
            if (ImGui::BeginPopupModal("##startup_dialog", nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {

                ImGui::SetWindowFontScale(1.5f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.75f, 1.f, 1.f));
                ImGui::TextUnformatted("IDHMFIS");
                ImGui::PopStyleColor();
                ImGui::SetWindowFontScale(1.0f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.65f, 0.65f, 0.65f, 1.f));
                ImGui::TextUnformatted("I Don't Have Money For ILDA Software");
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Detect recent file
                std::string recent_path;
                std::string recent_name;
                {
                    namespace fs = std::filesystem;
                    std::error_code ec;
                    auto rf = RecentFiles::load();
                    for (const auto& e : rf.entries()) {
                        if (fs::exists(e.path, ec)) {
                            recent_path = e.path;
                            recent_name = e.name.empty() ? e.path : e.name;
                            break;
                        }
                    }
                }

                float btn_w = 340.f;

                // Continue button (only if there is a valid recent file)
                if (!recent_path.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.95f, 0.85f, 1.f));
                    ImGui::TextUnformatted("Recent show:");
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.85f, 0.4f, 1.f));
                    ImGui::TextUnformatted(recent_name.c_str());
                    ImGui::PopStyleColor(2);
                    ImGui::Spacing();

                    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f,  0.40f, 0.1f,  1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.60f, 0.15f, 1.f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f,  0.75f, 0.2f,  1.f));
                    std::string cont_label = "Continue: " + recent_name;
                    if (ImGui::Button(cont_label.c_str(), ImVec2(btn_w, 0.f))) {
                        pending_load_path_ = recent_path;
                        is_loading_show    = true;
                        ImGui::CloseCurrentPopup();
                        show_startup_dialog = false;
                    }
                    ImGui::PopStyleColor(3);
                    ImGui::Spacing();
                }

                // New Show button
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.20f, 0.25f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.30f, 0.38f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.40f, 0.50f, 1.f));
                if (ImGui::Button("New Show", ImVec2(btn_w, 0.f))) {
                    project = std::make_shared<Project>();
                    project->name = "Untitled";
                    engine->send(cmd::LoadProject{ project });
                    state.cues.clear();
                    state.timeline_cues.clear();
                    state.project_name  = "Untitled";
                    state.project_path  = "";
                    state.project_dirty = false;
                    ImGui::CloseCurrentPopup();
                    show_startup_dialog = false;
                }
                ImGui::PopStyleColor(3);
                ImGui::Spacing();

                // Open Show button
                ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.18f, 0.20f, 0.25f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.30f, 0.38f, 1.f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.38f, 0.40f, 0.50f, 1.f));
                if (ImGui::Button("Open Show...", ImVec2(btn_w, 0.f))) {
#if defined(_WIN32)
                    SDL_PropertiesID dlg_props = SDL_GetWindowProperties(window);
                    HWND hwnd_dlg = (HWND)SDL_GetPointerProperty(dlg_props,
                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
                    std::string open_path = win32_open_file_dialog(hwnd_dlg);
                    if (!open_path.empty()) {
                        pending_load_path_ = open_path;
                        is_loading_show    = true;
                        ImGui::CloseCurrentPopup();
                        show_startup_dialog = false;
                    }
#endif
                }
                ImGui::PopStyleColor(3);

                ImGui::EndPopup();
            }
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(2);
        }

        if (!show_startup_dialog) {
        // Main layout
        layout_draw(state, layout_ctx, layout_cbs, palette, shortcuts);

        // Poll UI-owned state changes and send engine commands
        {
            static float last_master = 1.f;
            if (std::abs(state.master_intensity - last_master) > 0.001f) {
                engine->send(cmd::SetMasterIntensity{ state.master_intensity });
                last_master = state.master_intensity;
            }

            // Sync programmer (frame editor) objects to engine every frame
            {
                bool prog_active = !layout_ctx.frame_editor.objects.empty();
                bool prog_blind  = layout_ctx.frame_editor_blind;
                KeyframeLayer kf;
                kf.objects       = layout_ctx.frame_editor.objects;
                kf.symmetry_mode = static_cast<int>(layout_ctx.frame_editor.symmetry);
                cmd::SetProgrammerFrame spcmd;
                spcmd.objects          = kf;
                spcmd.global_layer     = layout_ctx.programmer_global;
                spcmd.fx_layer         = layout_ctx.programmer_fx_layer;
                spcmd.blind            = prog_blind;
                spcmd.active           = prog_active;
                engine->send(spcmd);
            }
        }

        // ── Emergency Shutoff key handling ────────────────────────────────────
        // Activate via user-bound key (toggle).
        // Deactivate via Ctrl+Alt+Enter (always works regardless of bound key).
        if (!state.emergency_key_capturing) {
            if (state.emergency_shutoff_key != 0) {
                if (!state.emergency_shutoff_active &&
                    ImGui::IsKeyPressed(static_cast<ImGuiKey>(state.emergency_shutoff_key),
                                        /*repeat=*/false)) {
                    state.emergency_shutoff_active = true;
                    engine->send(cmd::SetEmergencyShutoff{ true });
                    // Clear programmer — use ClearProgrammer (explicit clear, ChamSys rule)
                    engine->send(cmd::ClearProgrammer{});
                    layout_ctx.frame_editor.objects.clear();
                    // Zero all active playback intensities
                    for (int i = 0; i < UIState::kMaxPlaybacks; ++i) {
                        if (state.snap.playbacks[i].active)
                            engine->send(cmd::SetPlaybackIntensity{
                                state.snap.playbacks[i].id, 0.f });
                    }
                }
            }

            // Ctrl+Alt+Enter always deactivates emergency shutoff
            if (state.emergency_shutoff_active) {
                if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl)
                    && ImGui::IsKeyDown(ImGuiKey_LeftAlt)
                    && ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false)) {
                    state.emergency_shutoff_active = false;
                    engine->send(cmd::SetEmergencyShutoff{ false });
                    state.output_enabled = true;
                    engine->send(cmd::SetOutputEnable{ true });
                }
            }
        }

        // Emergency Shutoff overlay is rendered by layout.cpp (##eso_overlay window),
        // which is drawn last and correctly blocks all mouse input.
        } // end if (!show_startup_dialog)
    } // end EULA gate

    // ── Consume quit flag from layout (File > Quit menu item) ────────────────
    if (layout_ctx.quit_requested) {
        layout_ctx.quit_requested = false;
        quit_requested = true;
    }

    // ── Quit confirmation dialog ──────────────────────────────────────────────
    if (quit_requested) {
        ImGui::OpenPopup("Quit IDHMFIS?");
        quit_requested = false;
    }
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(340.f, 0.f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("Quit IDHMFIS?", nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Are you sure you want to quit?");
        if (state.project_dirty)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.1f, 1.f), "You have unsaved changes.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        float btn_w = 90.f;
        // Save & Quit
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.1f, 0.45f, 0.1f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.65f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.2f, 0.8f, 0.2f, 1.f));
        if (ImGui::Button("Save & Quit", ImVec2(btn_w, 0.f))) {
            if (!state.project_path.empty() && project)
                project->save(state.project_path);
            ImGui::CloseCurrentPopup();
            running = false;
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.f, 8.f);
        // Quit without saving
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.5f, 0.1f, 0.1f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.15f, 0.15f, 1.f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.9f, 0.2f, 0.2f, 1.f));
        if (ImGui::Button("Quit", ImVec2(btn_w, 0.f))) {
            ImGui::CloseCurrentPopup();
            running = false;
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine(0.f, 8.f);
        if (ImGui::Button("Cancel", ImVec2(btn_w, 0.f)))
            ImGui::CloseCurrentPopup();

        ImGui::Spacing();
        ImGui::EndPopup();
    }

    // ImGui render
    ImGui::Render();

#if defined(_WIN32)
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), dx12.cmd_list);

    // Transition backbuffer back to present
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    dx12.cmd_list->ResourceBarrier(1, &barrier);
    dx12.cmd_list->Close();

    ID3D12CommandList* lists[] = { dx12.cmd_list };
    dx12.cmd_queue->ExecuteCommandLists(1, lists);

    dx12.swapchain->Present(1, 0);  // vsync on

    // Signal fence for this frame
    UINT64 fence_val = ++dx12.fence_vals[frame_idx];
    dx12.cmd_queue->Signal(dx12.fence, fence_val);
    dx12.fence_vals[frame_idx] = fence_val;
    dx12.frame_index = dx12.swapchain->GetCurrentBackBufferIndex();
#else
    // Vulkan: vkQueueSubmit / present
#endif

    // Multi-viewport rendering (viewports outside main window)
    ImGuiIO& io = ImGui::GetIO();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shutdown
// ─────────────────────────────────────────────────────────────────────────────
void Application::Impl::shutdown() {
    log::info("shutdown: beginning clean shutdown");
    auto_save_.on_clean_shutdown(state.project_path);

    // Stop engine threads before GPU teardown
    thumbnailer.stop();

    // Stop all laser DacManagers (blocks briefly to join their threads — acceptable here)
    for (auto& [/*id*/ ignored, mgr] : laser_managers_)
        mgr->stop();
    laser_managers_.clear();

    if (watchdog) { watchdog->stop(); watchdog.reset(); }
    if (engine)   { log::info("engine: stopping"); engine->stop(); engine.reset(); }
    log::info("audio: shutting down");
    audio.shutdown();
    log::info("shutdown: complete");

#if defined(_WIN32)
    dx12_wait_all(dx12);
    ImGui_ImplDX12_Shutdown();
#else
    // ImGui_ImplVulkan_Shutdown()
#endif
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

#if defined(_WIN32)
    dx12_cleanup(dx12);
#endif

    // Destroy patch-managed HDMI windows
    for (auto& [/*id*/ ignored, w] : hdmi_windows_) {
        if (w) SDL_DestroyWindow(w);
    }
    hdmi_windows_.clear();

    if (hdmi_window_) { SDL_DestroyWindow(hdmi_window_); hdmi_window_ = nullptr; }
    if (window)       { SDL_DestroyWindow(window);       window       = nullptr; }
    SDL_Quit();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Application PIMPL interface
// ─────────────────────────────────────────────────────────────────────────────
Application::Application()  : impl_(std::make_unique<Impl>()) {}
Application::~Application() = default;

int Application::run() {
    if (!impl_->init()) return 1;

    Uint64 last_tick = SDL_GetTicks();
    const float target_ms = impl_->target_frame_ms;

    while (impl_->running) {
        // --- Event processing -------------------------------------------------
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) {
                if (!impl_->safety_eula_accepted)
                    std::terminate();   // closed before accepting EULA
                impl_->quit_requested = true;
            }
            if (event.type == SDL_EVENT_WINDOW_RESIZED) {
#if defined(_WIN32)
                // Only resize the D3D12 swap chain when the *main* window changes.
                if (impl_->window &&
                    event.window.windowID == SDL_GetWindowID(impl_->window)) {
                    dx12_resize(impl_->dx12, event.window.data1, event.window.data2);
                }
#endif
            }
            // Keep legacy HDMI toggle window responsive; close destroys it.
            if (impl_->hdmi_window_) {
                SDL_WindowID hdmi_id = SDL_GetWindowID(impl_->hdmi_window_);
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                    event.window.windowID == hdmi_id) {
                    impl_->state.output_config.hdmi_window_enabled = false;
                    SDL_DestroyWindow(impl_->hdmi_window_);
                    impl_->hdmi_window_ = nullptr;
                }
            }
            // Handle close requests for patch-managed HDMI windows.
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                for (auto it = impl_->hdmi_windows_.begin();
                     it != impl_->hdmi_windows_.end(); ++it) {
                    if (it->second &&
                        event.window.windowID == SDL_GetWindowID(it->second)) {
                        SDL_DestroyWindow(it->second);
                        impl_->hdmi_windows_.erase(it);
                        break;
                    }
                }
            }
        }

        // --- Frame time throttle (target 120 fps) ----------------------------
        Uint64 now       = SDL_GetTicks();
        float  elapsed   = (float)(now - last_tick);
        if (elapsed < target_ms) {
            SDL_Delay((Uint32)(target_ms - elapsed));
        }
        last_tick = SDL_GetTicks();

        // --- Render -----------------------------------------------------------
        impl_->run_frame();
    }

    impl_->shutdown();
    return 0;
}

} // namespace idhmfis
