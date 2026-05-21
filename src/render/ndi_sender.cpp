// ndi_sender.cpp — NDI sender implementation with dynamic DLL loading.
// Searches for NDI 6 SDK first, then NDI 5, then common installation paths.
// Never hard-links against NDI — the app must work without it installed.

#include "ndi_sender.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>
#include "../core/logger.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace idhmfis {

// ---------------------------------------------------------------------------
// NDI FourCC constants (from NDIlib.h — reproduced to avoid SDK dependency)
// ---------------------------------------------------------------------------
static constexpr int kNdiFourCC_UYVY  = 0x59565955; // 'UYVY'
static constexpr int kNdiFourCC_BGRA  = 0x41524742; // 'BGRA'
static constexpr int kNdiFourCC_FLTP  = 0x50544C46; // 'FLTP' — planar float audio

// NDI function name strings (same across SDK 5 and 6)
static constexpr const char* kFnInit         = "NDIlib_initialize";
static constexpr const char* kFnDestroy      = "NDIlib_destroy";
static constexpr const char* kFnSendCreate   = "NDIlib_send_create";
static constexpr const char* kFnSendDestroy  = "NDIlib_send_destroy";
static constexpr const char* kFnSendVideoV2  = "NDIlib_send_send_video_v2";
static constexpr const char* kFnSendAudioV3  = "NDIlib_send_send_audio_v3";
static constexpr const char* kFnConnections  = "NDIlib_send_get_no_connections";

// ---------------------------------------------------------------------------
// Platform-specific DLL helpers
// ---------------------------------------------------------------------------
#ifdef _WIN32
static void* platform_load_lib(const std::string& path) {
    return static_cast<void*>(LoadLibraryA(path.c_str()));
}
static void platform_free_lib(void* handle) {
    if (handle) FreeLibrary(static_cast<HMODULE>(handle));
}
static void* platform_get_proc(void* handle, const char* name) {
    return reinterpret_cast<void*>(
        GetProcAddress(static_cast<HMODULE>(handle), name));
}
static std::string platform_get_env(const char* var) {
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(var, buf, sizeof(buf));
    return (n > 0 && n < sizeof(buf)) ? std::string(buf, n) : std::string{};
}
#else
static void* platform_load_lib(const std::string& path) {
    return dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
}
static void platform_free_lib(void* handle) {
    if (handle) dlclose(handle);
}
static void* platform_get_proc(void* handle, const char* name) {
    return dlsym(handle, name);
}
static std::string platform_get_env(const char* var) {
    const char* v = getenv(var);
    return v ? std::string(v) : std::string{};
}
#endif

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
NdiSender::NdiSender()  = default;
NdiSender::~NdiSender() { shutdown(); }

// ---------------------------------------------------------------------------
// try_load_from_path — attempt to load a specific library file
// ---------------------------------------------------------------------------
bool NdiSender::try_load_from_path(const std::string& path) {
    if (path.empty()) return false;

    void* h = platform_load_lib(path);
    if (!h) return false;

    // Resolve all required symbols
    auto resolve = [&](const char* name) -> void* {
        void* sym = platform_get_proc(h, name);
        if (!sym) {
            log::warn("NDI: symbol '%s' not found in %s", name, path.c_str());
        }
        return sym;
    };

    fn_init_        = reinterpret_cast<FnNdiInitialize>    (resolve(kFnInit));
    fn_destroy_     = reinterpret_cast<FnNdiDestroy>       (resolve(kFnDestroy));
    fn_send_create_ = reinterpret_cast<FnNdiSendCreate>    (resolve(kFnSendCreate));
    fn_send_destroy_= reinterpret_cast<FnNdiSendDestroy>   (resolve(kFnSendDestroy));
    fn_send_video_  = reinterpret_cast<FnNdiSendVideoV2>   (resolve(kFnSendVideoV2));
    fn_send_audio_  = reinterpret_cast<FnNdiSendAudioV3>   (resolve(kFnSendAudioV3));
    fn_connections_ = reinterpret_cast<FnNdiSendConnections>(resolve(kFnConnections));

    if (!fn_init_ || !fn_destroy_ || !fn_send_create_ ||
        !fn_send_destroy_ || !fn_send_video_ || !fn_connections_) {
        platform_free_lib(h);
        fn_init_        = nullptr;
        fn_destroy_     = nullptr;
        fn_send_create_ = nullptr;
        fn_send_destroy_= nullptr;
        fn_send_video_  = nullptr;
        fn_send_audio_  = nullptr;
        fn_connections_ = nullptr;
        return false;
    }

    lib_handle_ = h;
    log::info("NDI: loaded runtime from %s", path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
// try_load_ndi_lib — search candidate paths in priority order
// ---------------------------------------------------------------------------
bool NdiSender::try_load_ndi_lib() {
    // Build the candidate path list
    std::vector<std::string> candidates;

    // Helper: add candidate from an env-var dir prefix
    auto add_from_env = [&](const char* var, const char* suffix) {
        std::string dir = platform_get_env(var);
        if (!dir.empty()) candidates.push_back(dir + suffix);
    };

    // 1. NDI 6 Tools runtime env var (set by NDI Tools installer — most common)
    add_from_env("NDI_RUNTIME_DIR",   "\\Processing.NDI.Lib.x64.dll");
    add_from_env("NDI_RUNTIME_DIR",   "\\ndi.dll");
    // 2. Older NDI 5 Tools env var
    add_from_env("NDIRUNTIME",        "\\Processing.NDI.Lib.x64.dll");
    // 3. Developer SDK env var
    add_from_env("NDI_SDK_DIR",       "\\Bin\\x64\\Processing.NDI.Lib.x64.dll");
    add_from_env("NDI_SDK_DIR",       "\\Bin\\x64\\ndi.dll");

#ifdef _WIN32
    // 4. NDI 6 Tools default install paths
    candidates.push_back("C:\\Program Files\\NDI\\NDI 6 Tools\\Processing.NDI.Lib.x64.dll");
    candidates.push_back("C:\\Program Files\\NDI\\NDI 6 Tools\\Bin\\x64\\Processing.NDI.Lib.x64.dll");

    // 5. NDI 6 SDK default install paths
    candidates.push_back("C:\\Program Files\\NDI\\NDI 6 SDK\\Bin\\x64\\Processing.NDI.Lib.x64.dll");
    candidates.push_back("C:\\Program Files (x86)\\NDI\\NDI 6 SDK\\Bin\\x64\\Processing.NDI.Lib.x64.dll");

    // 6. NDI 5 SDK and Tools
    candidates.push_back("C:\\Program Files\\NDI\\NDI 5 SDK\\Bin\\x64\\Processing.NDI.Lib.x64.dll");
    candidates.push_back("C:\\Program Files (x86)\\NDI\\NDI 5 SDK\\Bin\\x64\\Processing.NDI.Lib.x64.dll");
    candidates.push_back("C:\\Program Files\\NDI\\NDI 5 Tools\\Processing.NDI.Lib.x64.dll");

    // 7. NDI Runtime redistributable (ships with NDI Studio Monitor, Studio Monitor, etc.)
    candidates.push_back("C:\\Program Files\\NDI\\NDI 6 Runtime\\x64\\Processing.NDI.Lib.x64.dll");
    candidates.push_back("C:\\Program Files\\NDI\\NDI 5 Runtime\\x64\\Processing.NDI.Lib.x64.dll");

    // 8. Vizrt NDI (post-acquisition naming)
    candidates.push_back("C:\\Program Files\\Vizrt\\NDI\\Processing.NDI.Lib.x64.dll");
    candidates.push_back("C:\\Program Files\\Vizrt\\NDI SDK\\Bin\\x64\\Processing.NDI.Lib.x64.dll");

    // 9. System PATH / system32 — DLL may be registered globally by installer
    candidates.push_back("Processing.NDI.Lib.x64.dll");
    candidates.push_back("ndi.dll");

    // 10. Registry: HKLM\SOFTWARE\NDI (NDI 6) and HKLM\SOFTWARE\NewTek\NDI (NDI 5)
    {
        static const char* kRegKeys[] = {
            "SOFTWARE\\NDI",
            "SOFTWARE\\NewTek\\NDI",
            "SOFTWARE\\Vizrt\\NDI"
        };
        for (const char* regkey : kRegKeys) {
            HKEY hkey = nullptr;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, regkey,
                              0, KEY_READ, &hkey) == ERROR_SUCCESS) {
                char buf[MAX_PATH]{};
                DWORD buf_size = sizeof(buf);
                DWORD type = REG_SZ;
                static const char* kValueNames[] = {
                    "NDI_RUNTIME_DIR", "NDI_SDK_DIR", "InstallDir", ""
                };
                for (const char* val : kValueNames) {
                    buf_size = sizeof(buf);
                    if (RegQueryValueExA(hkey, val, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(buf), &buf_size)
                        == ERROR_SUCCESS && buf[0]) {
                        std::string base(buf);
                        candidates.push_back(base + "\\Processing.NDI.Lib.x64.dll");
                        candidates.push_back(base + "\\Bin\\x64\\Processing.NDI.Lib.x64.dll");
                        candidates.push_back(base + "\\ndi.dll");
                        break;
                    }
                }
                RegCloseKey(hkey);
            }
        }
    }

    // 11. Known apps that bundle the NDI DLL (Capture, vMix, OBS, etc.)
    //     Check existence before adding so the candidate list stays tight.
    {
        static const char* kDll = "Processing.NDI.Lib.x64.dll";
        static const char* kKnown[] = {
            "C:\\Program Files\\Capture\\Capture 2026",
            "C:\\Program Files\\Capture\\Capture 2025",
            "C:\\Program Files\\Capture\\Capture 2024",
            "C:\\Program Files\\Capture",
            "C:\\Program Files\\vMix",
            "C:\\Program Files\\Wirecast\\Wirecast",
            "C:\\Program Files\\obs-studio\\bin\\64bit",
            "C:\\Program Files (x86)\\obs-studio\\bin\\64bit",
            nullptr
        };
        for (const char** d = kKnown; *d; ++d) {
            std::string p = std::string(*d) + "\\" + kDll;
            if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                candidates.push_back(std::move(p));
        }

        // Broad scan: 2 levels deep in Program Files — catches any app that
        // ships the NDI runtime in its own install directory (e.g. Capture).
        static const char* kRoots[] = {
            "C:\\Program Files",
            "C:\\Program Files (x86)",
            nullptr
        };
        for (const char** root = kRoots; *root; ++root) {
            std::string pat = std::string(*root) + "\\*";
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fd.cFileName[0] == '.') continue;
                std::string lvl1 = std::string(*root) + "\\" + fd.cFileName;
                {
                    std::string p = lvl1 + "\\" + kDll;
                    if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                        candidates.push_back(p);
                }
                WIN32_FIND_DATAA fd2;
                std::string pat2 = lvl1 + "\\*";
                HANDLE h2 = FindFirstFileA(pat2.c_str(), &fd2);
                if (h2 == INVALID_HANDLE_VALUE) continue;
                do {
                    if (!(fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                    if (fd2.cFileName[0] == '.') continue;
                    std::string p = lvl1 + "\\" + fd2.cFileName + "\\" + kDll;
                    if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES)
                        candidates.push_back(p);
                } while (FindNextFileA(h2, &fd2));
                FindClose(h2);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }

#elif defined(__APPLE__)
    candidates.push_back("/usr/local/lib/libndi.dylib");
    candidates.push_back("/opt/homebrew/lib/libndi.dylib");
    candidates.push_back("libndi.dylib");
#else
    candidates.push_back("/usr/lib/x86_64-linux-gnu/libndi.so.6");
    candidates.push_back("/usr/lib/x86_64-linux-gnu/libndi.so.5");
    candidates.push_back("/usr/local/lib/libndi.so.6");
    candidates.push_back("/usr/local/lib/libndi.so");
    candidates.push_back("libndi.so.6");
    candidates.push_back("libndi.so");
#endif

    for (const auto& path : candidates) {
        if (try_load_from_path(path)) return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool NdiSender::init(const std::string& source_name, int width, int height,
                     int fps_N, int fps_D, bool bgra, bool clock_video)
{
    source_name_   = source_name;
    width_         = width;
    height_        = height;
    fps_N_         = std::max(1, fps_N);
    fps_D_         = std::max(1, fps_D);
    bgra_mode_     = bgra;
    clock_video_   = clock_video;

    if (!try_load_ndi_lib()) {
        log::warn("NDI: runtime not found; NDI output disabled. Install NDI SDK or NDI Tools to enable.");
        return false;
    }

    if (!fn_init_()) {
        log::error("NDI: NDIlib_initialize() failed");
        unload_ndi_lib();
        return false;
    }

    NdiSendCreateV2 create_desc{};
    create_desc.p_ndi_name  = source_name_.c_str();
    create_desc.p_groups    = nullptr;
    create_desc.clock_video = clock_video_;
    create_desc.clock_audio = false;

    ndi_send_ = fn_send_create_(&create_desc);
    if (!ndi_send_) {
        log::error("NDI: NDIlib_send_create() failed");
        fn_destroy_();
        unload_ndi_lib();
        return false;
    }

    have_ndi_ = true;
    log::info("NDI: sender '%s' initialised (%dx%d @ %d/%dfps %s)", source_name_.c_str(), width, height, fps_N_, fps_D_, bgra ? "BGRA" : "UYVY");
    return true;
}

// ---------------------------------------------------------------------------
// send_bgra
// ---------------------------------------------------------------------------
void NdiSender::send_bgra(const uint8_t* data, int width, int height,
                           int64_t timecode_100ns)
{
    if (!have_ndi_ || !fn_send_video_) return;

    NdiVideoFrameV2 frame{};
    frame.xres                = width;
    frame.yres                = height;
    frame.FourCC              = kNdiFourCC_BGRA;
    frame.frame_rate_N        = fps_N_;
    frame.frame_rate_D        = fps_D_;
    frame.picture_aspect_ratio= static_cast<float>(width) / static_cast<float>(height);
    frame.frame_format_type   = 1; // progressive
    frame.timecode            = timecode_100ns;
    frame.p_data              = const_cast<uint8_t*>(data);
    frame.line_stride_in_bytes= width * 4;
    frame.timestamp           = timecode_100ns;

    fn_send_video_(ndi_send_, &frame);

    // Update connection count
    if (fn_connections_) {
        connections_.store(fn_connections_(ndi_send_, 0),
                           std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// send_uyvy
// ---------------------------------------------------------------------------
void NdiSender::send_uyvy(const uint8_t* data, int width, int height,
                           int64_t timecode_100ns)
{
    if (!have_ndi_ || !fn_send_video_) return;

    NdiVideoFrameV2 frame{};
    frame.xres                = width;
    frame.yres                = height;
    frame.FourCC              = kNdiFourCC_UYVY;
    frame.frame_rate_N        = fps_N_;
    frame.frame_rate_D        = fps_D_;
    frame.picture_aspect_ratio= static_cast<float>(width) / static_cast<float>(height);
    frame.frame_format_type   = 1;
    frame.timecode            = timecode_100ns;
    frame.p_data              = const_cast<uint8_t*>(data);
    frame.line_stride_in_bytes= width * 2; // UYVY = 2 bytes/pixel
    frame.timestamp           = timecode_100ns;

    fn_send_video_(ndi_send_, &frame);

    if (fn_connections_) {
        connections_.store(fn_connections_(ndi_send_, 0),
                           std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// send_audio
// ---------------------------------------------------------------------------
void NdiSender::send_audio(const float* samples, int num_samples,
                            int num_channels, int sample_rate,
                            int64_t timecode_100ns)
{
    if (!have_ndi_ || !fn_send_audio_) return;

    // NDI FLTP format is planar: all samples for ch0 contiguous, then ch1.
    // Callers MUST supply planar data. Interleaved stereo will produce
    // silent/corrupt right channel — de-interleave before calling.

    NdiAudioFrameV3 frame{};
    frame.sample_rate            = sample_rate;
    frame.no_channels            = num_channels;
    frame.no_samples             = num_samples;
    frame.timecode               = timecode_100ns;
    frame.FourCC                 = kNdiFourCC_FLTP;
    frame.p_data                 = const_cast<float*>(samples);
    frame.channel_stride_in_bytes= num_samples * sizeof(float);

    fn_send_audio_(ndi_send_, &frame);
}

// ---------------------------------------------------------------------------
// poll_connections — update connection count without sending a frame
// ---------------------------------------------------------------------------
void NdiSender::poll_connections() {
    if (!have_ndi_ || !fn_connections_ || !ndi_send_) return;
    connections_.store(fn_connections_(ndi_send_, 0), std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// is_connected / num_connections
// ---------------------------------------------------------------------------
bool NdiSender::is_connected() const {
    return connections_.load(std::memory_order_relaxed) > 0;
}

int NdiSender::num_connections() const {
    return connections_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// shutdown / unload
// ---------------------------------------------------------------------------
void NdiSender::unload_ndi_lib() {
    if (lib_handle_) {
        platform_free_lib(lib_handle_);
        lib_handle_    = nullptr;
    }
    fn_init_        = nullptr;
    fn_destroy_     = nullptr;
    fn_send_create_ = nullptr;
    fn_send_destroy_= nullptr;
    fn_send_video_  = nullptr;
    fn_send_audio_  = nullptr;
    fn_connections_ = nullptr;
}

void NdiSender::shutdown() {
    if (!have_ndi_) return;

    if (ndi_send_ && fn_send_destroy_) {
        fn_send_destroy_(ndi_send_);
        ndi_send_ = nullptr;
    }
    if (fn_destroy_) {
        fn_destroy_();
    }

    have_ndi_ = false;
    unload_ndi_lib();
    log::info("NDI: sender '%s' shut down", source_name_.c_str());
}

} // namespace idhmfis
