// virtual_camera.cpp — VirtualCamera implementation (SHM producer)
//
// This file handles the IDHMFIS.exe side:
//   1. Create / map the named shared memory (Local\IDHMFISCameraFrame)
//   2. Rasterise laser PointBuffer into BGR24 on each push_frame() call
//   3. Write the BGR24 frame into the SHM under the named mutex
//   4. Register IDHMFISCapture.dll in HKCU so Capture 2024 discovers it
//
// The actual DirectShow COM server lives in IDHMFISCapture.dll (idhmfis_camera.cpp).

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>    // StringFromGUID2, CoCreateGuid
#include <mfapi.h>      // MFStartup, MFShutdown

#include "virtual_camera.h"
#include "camera_shm.h"
#include "core/logger.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
//  Fixed CLSID for our DirectShow filter.
//  Must match CLSID_IdhmfisCapture in idhmfis_camera.cpp.
//  {CA1D0001-DAC0-1DA0-BEEF-DEADBEEF0001}
// ─────────────────────────────────────────────────────────────────────────────
namespace {

static const GUID kIdhmfisCaptureCLSID = {
    0xCA1D0001u, 0xDAC0u, 0x1DA0u,
    { 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 }
};

// {860BB310-5D01-11D0-BD3B-00A0C911CE86}  CLSID_VideoInputDeviceCategory
static const GUID kVideoInputDeviceCat = {
    0x860BB310u, 0x5D01u, 0x11D0u,
    { 0xBD, 0x3B, 0x00, 0xA0, 0xC9, 0x11, 0xCE, 0x86 }
};

static std::wstring guid_to_wstring(const GUID& g) {
    WCHAR buf[40]{};
    StringFromGUID2(g, buf, static_cast<int>(std::size(buf)));
    return buf;
}

static std::wstring to_wstring(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

} // anonymous namespace

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / Destructor
// ─────────────────────────────────────────────────────────────────────────────
VirtualCamera::VirtualCamera() {
    rgb_.assign(static_cast<size_t>(kWidth * kHeight * 3), 0u);
}

VirtualCamera::~VirtualCamera() {
    close();
}

// ─────────────────────────────────────────────────────────────────────────────
//  open
// ─────────────────────────────────────────────────────────────────────────────
bool VirtualCamera::open() {
    if (open_.load()) return true;

    // Build indexed names for multi-stream support.
    // Stream 0: canonical names from camera_shm.h constants.
    // Stream N>0: append N as suffix so each instance has unique SHM/mutex names.
    const std::wstring shm_name = (stream_index_ == 0)
        ? std::wstring(kCamShmName)
        : std::wstring(kCamShmName) + std::to_wstring(stream_index_);
    const std::wstring mutex_name = (stream_index_ == 0)
        ? std::wstring(kCamMutexName)
        : std::wstring(kCamMutexName) + std::to_wstring(stream_index_);

    // Create named shared memory.
    shm_handle_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(CameraShm)),
        shm_name.c_str());

    if (!shm_handle_) {
        log::error("VirtualCamera[%d]: CreateFileMapping failed (%lu)", stream_index_, GetLastError());
        return false;
    }

    shm_view_ = static_cast<CameraShm*>(
        MapViewOfFile(shm_handle_, FILE_MAP_WRITE, 0, 0, sizeof(CameraShm)));

    if (!shm_view_) {
        log::error("VirtualCamera[%d]: MapViewOfFile failed (%lu)", stream_index_, GetLastError());
        CloseHandle(shm_handle_);
        shm_handle_ = nullptr;
        return false;
    }

    std::memset(shm_view_, 0, sizeof(CameraShm));

    // Create named mutex for SHM synchronisation.
    cam_mutex_ = CreateMutexW(nullptr, FALSE, mutex_name.c_str());
    if (!cam_mutex_) {
        log::warn("VirtualCamera[%d]: CreateMutex failed (%lu) — continuing without mutex",
                  stream_index_, GetLastError());
    }

    // COM/registry registration only for primary stream — DirectShow has one CLSID.
    if (stream_index_ == 0) {
        if (!write_registry()) {
            log::warn("VirtualCamera: registry write failed (Capture may not enumerate it)");
        }
    }

    open_.store(true);

    // Start MF virtual camera (Win11+) for primary stream so the device appears
    // in Windows camera / video-capture enumerators.
    if (stream_index_ == 0) {
        start_mf_virtual_camera();
    }

    log::info("VirtualCamera[%d]: opened (SHM+DirectShow path)", stream_index_);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  close
// ─────────────────────────────────────────────────────────────────────────────
void VirtualCamera::close() {
    if (!open_.load()) return;
    open_.store(false);

    // Stop MF virtual camera before unmapping SHM
    stop_mf_virtual_camera();

    if (registry_written_) {
        remove_registry();
        registry_written_ = false;
    }

    if (shm_view_) {
        UnmapViewOfFile(shm_view_);
        shm_view_ = nullptr;
    }
    if (shm_handle_) {
        CloseHandle(shm_handle_);
        shm_handle_ = nullptr;
    }
    if (cam_mutex_) {
        CloseHandle(cam_mutex_);
        cam_mutex_ = nullptr;
    }

    log::info("VirtualCamera: closed");
}

// ─────────────────────────────────────────────────────────────────────────────
//  push_frame — rasterise pts and write into SHM
// ─────────────────────────────────────────────────────────────────────────────
void VirtualCamera::push_frame(const PointBuffer& pts) {
    if (!open_.load() || !shm_view_) return;

    // Rate-limit to 30fps. The output thread may call this at the full engine
    // rate (up to 1kHz); rasterizing 1920×1080 on every tick stalls the DAC.
    {
        auto now = Clock::now();
        using namespace std::chrono_literals;
        if (now - last_push_ < 33ms) return;
        last_push_ = now;
    }

    std::lock_guard<std::mutex> lk(push_mutex_);

    // Rasterise into local buffer.
    rasterise(pts);

    // Write to SHM under named mutex so the DLL reader doesn't get a torn frame.
    // BUG #26 fix: sequence counter must be incremented INSIDE the mutex so the
    // consumer cannot observe the incremented counter before the payload is written.
    // Previously the increment was outside the mutex, creating a write-ordering race
    // where the DLL could read a new sequence number but still see old frame data.
    if (cam_mutex_) {
        WaitForSingleObject(cam_mutex_, INFINITE);
        std::memcpy(shm_view_->data, rgb_.data(), static_cast<size_t>(kCamFrameBytes));
        // Increment inside the lock, after payload is fully written.
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm_view_->sequence));
        ReleaseMutex(cam_mutex_);
    } else {
        // No mutex available (open() warned about this) — best-effort write.
        std::memcpy(shm_view_->data, rgb_.data(), static_cast<size_t>(kCamFrameBytes));
        InterlockedIncrement(reinterpret_cast<volatile LONG*>(&shm_view_->sequence));
    }

}

// ─────────────────────────────────────────────────────────────────────────────
//  rasterise — render PointBuffer into rgb_[] (bottom-up BGR24)
// ─────────────────────────────────────────────────────────────────────────────
void VirtualCamera::rasterise(const PointBuffer& pts) {
    std::fill(rgb_.begin(), rgb_.end(), uint8_t{ 0 });

    if (pts.empty()) return;

    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const LaserPoint& a = pts[i];
        const LaserPoint& b = pts[i + 1];

        if (a.blanked || b.blanked) continue;

        int x0 = ilda_to_px(a.x, kWidth);
        int y0 = ilda_to_px(a.y, kHeight);
        int x1 = ilda_to_px(b.x, kWidth);
        int y1 = ilda_to_px(b.y, kHeight);

        draw_segment(x0, y0, x1, y1, b.r, b.g, b.b, 2);
    }

    for (const auto& p : pts) {
        if (p.blanked) continue;
        int px = ilda_to_px(p.x, kWidth);
        int py = ilda_to_px(p.y, kHeight);
        blend_pixel(px, py, p.r, p.g, p.b, 1.0f);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ilda_to_px
// ─────────────────────────────────────────────────────────────────────────────
int VirtualCamera::ilda_to_px(int16_t v, int dim) noexcept {
    float nv  = static_cast<float>(v) / 32767.0f;
    float px  = (nv + 1.0f) * 0.5f * static_cast<float>(dim - 1);
    int   pxi = static_cast<int>(px + 0.5f);
    return std::clamp(pxi, 0, dim - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
//  blend_pixel — additive blend, clamped
// ─────────────────────────────────────────────────────────────────────────────
void VirtualCamera::blend_pixel(int x, int y,
                                 uint8_t r, uint8_t g, uint8_t b,
                                 float alpha) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    int idx = (y * kWidth + x) * 3;
    auto blend_ch = [&](uint8_t base, uint8_t ch) -> uint8_t {
        int v = static_cast<int>(base) +
                static_cast<int>(static_cast<float>(ch) * alpha);
        return static_cast<uint8_t>(std::min(v, 255));
    };
    rgb_[static_cast<size_t>(idx + 0)] = blend_ch(rgb_[static_cast<size_t>(idx + 0)], b);
    rgb_[static_cast<size_t>(idx + 1)] = blend_ch(rgb_[static_cast<size_t>(idx + 1)], g);
    rgb_[static_cast<size_t>(idx + 2)] = blend_ch(rgb_[static_cast<size_t>(idx + 2)], r);
}

// ─────────────────────────────────────────────────────────────────────────────
//  draw_segment — Bresenham with square thickness
// ─────────────────────────────────────────────────────────────────────────────
void VirtualCamera::draw_segment(int x0, int y0, int x1, int y1,
                                  uint8_t r, uint8_t g, uint8_t b,
                                  int thickness) {
    int dx  = std::abs(x1 - x0);
    int dy  = std::abs(y1 - y0);
    int sx  = (x0 < x1) ? 1 : -1;
    int sy  = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int t   = std::max(1, thickness / 2);

    while (true) {
        for (int oy = -t; oy <= t; ++oy)
            for (int ox = -t; ox <= t; ++ox)
                blend_pixel(x0 + ox, y0 + oy, r, g, b, 1.0f);

        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  write_registry — register IDHMFISCapture.dll in HKCU (no admin needed)
//
//  HKCU\Software\Classes\CLSID\{CA1D0001-...}\
//    (default)          = "IDHMFIS Laser Preview"
//    InprocServer32\(default) = "<exedir>\IDHMFISCapture.dll"
//    InprocServer32\ThreadingModel = "Both"
//  HKCU\Software\Classes\CLSID\{VideoInputDeviceCat}\Instance\{CA1D0001-...}
//    FriendlyName = "IDHMFIS Laser Preview"
//    CLSID        = "{CA1D0001-...}"
// ─────────────────────────────────────────────────────────────────────────────
bool VirtualCamera::write_registry() {
    std::wstring clsid_str = guid_to_wstring(kIdhmfisCaptureCLSID);
    std::wstring cat_str   = guid_to_wstring(kVideoInputDeviceCat);
    std::wstring wname     = to_wstring(kFriendlyName);

    // Build DLL path: same directory as this EXE.
    WCHAR exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    WCHAR* last_sep = std::wcsrchr(exe_path, L'\\');
    if (last_sep) last_sep[1] = L'\0';
    std::wstring dll_path = std::wstring(exe_path) + L"IDHMFISCapture.dll";

    // Verify the DLL actually exists at that path.
    DWORD attr = GetFileAttributesW(dll_path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        log::error("VirtualCamera: IDHMFISCapture.dll not found at '%ls' (err %lu) — virtual camera won't work",
                   dll_path.c_str(), GetLastError());
        // Still write the registry so future launches find it once the DLL exists.
    } else {
        log::info("VirtualCamera: DLL found at '%ls'", dll_path.c_str());
    }

    // Try HKLM first (MFCreateVirtualCamera's camera service runs as LocalSystem
    // and needs HKLM to find the COM server).  Falls back to HKCU if no elevation.
    auto write_clsid_keys = [&](HKEY root, const wchar_t* root_name) -> bool {
        std::wstring base_key = L"SOFTWARE\\Classes\\CLSID\\" + clsid_str;

        // root\...\CLSID\{our-clsid}
        {
            HKEY hk = nullptr;
            LONG rc = RegCreateKeyExW(root, base_key.c_str(),
                                0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr);
            if (rc != ERROR_SUCCESS) {
                log::warn("VirtualCamera: RegCreateKeyEx(%ls\\%ls) failed rc=%ld — skipping",
                          root_name, base_key.c_str(), rc);
                return false;
            }
            RegSetValueExW(hk, nullptr, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(wname.c_str()),
                           static_cast<DWORD>((wname.size() + 1) * sizeof(WCHAR)));
            RegCloseKey(hk);
        }

        // root\...\CLSID\{our-clsid}\InprocServer32
        {
            std::wstring sub = base_key + L"\\InprocServer32";
            HKEY hk = nullptr;
            LONG rc = RegCreateKeyExW(root, sub.c_str(),
                                0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr);
            if (rc != ERROR_SUCCESS) {
                log::warn("VirtualCamera: RegCreateKeyEx(%ls InprocServer32) failed rc=%ld",
                          root_name, rc);
                return false;
            }
            RegSetValueExW(hk, nullptr, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(dll_path.c_str()),
                           static_cast<DWORD>((dll_path.size() + 1) * sizeof(WCHAR)));
            const std::wstring threading = L"Both";
            RegSetValueExW(hk, L"ThreadingModel", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(threading.c_str()),
                           static_cast<DWORD>((threading.size() + 1) * sizeof(WCHAR)));
            RegFlushKey(hk);
            RegCloseKey(hk);
        }
        log::info("VirtualCamera: COM server registered in %ls\\%ls", root_name, base_key.c_str());
        return true;
    };

    bool hklm_ok = write_clsid_keys(HKEY_LOCAL_MACHINE, L"HKLM");
    bool hkcu_ok = write_clsid_keys(HKEY_CURRENT_USER,  L"HKCU");
    if (!hklm_ok && !hkcu_ok) {
        log::error("VirtualCamera: registry write failed in both HKLM and HKCU");
        return false;
    }

    // 3. HKCU\...\CLSID\{VideoInputDeviceCat}\Instance\{our-clsid}
    //    Also write FilterData binary blob — required for DirectShow hosts (OBS, VLC).
    {
        std::wstring inst_key = L"SOFTWARE\\Classes\\CLSID\\" + cat_str +
                                L"\\Instance\\" + clsid_str;
        HKEY hk = nullptr;
        LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, inst_key.c_str(),
                            0, nullptr, 0, KEY_SET_VALUE, nullptr, &hk, nullptr);
        if (rc != ERROR_SUCCESS) {
            log::error("VirtualCamera: RegCreateKeyEx(Instance) failed rc=%ld", rc);
            return false;
        }
        RegSetValueExW(hk, L"FriendlyName", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(wname.c_str()),
                       static_cast<DWORD>((wname.size() + 1) * sizeof(WCHAR)));
        RegSetValueExW(hk, L"CLSID", 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(clsid_str.c_str()),
                       static_cast<DWORD>((clsid_str.size() + 1) * sizeof(WCHAR)));

        // FilterData: REGFILTER2 binary blob for IFilterMapper enumeration.
        static const BYTE kFilterData[] = {
            0x02,0x00,0x00,0x00,  // version=2
            0x00,0x00,0x20,0x00,  // merit=0x00200000
            0x01,0x00,0x00,0x00,  // cPins=1
            0x00,0x00,0x00,0x00,  // reserved
            0x08,0x00,0x00,0x00,  // dwFlags=REG_PINFLAG_B_OUTPUT
            0x01,0x00,0x00,0x00,  // cInstances=1
            0x01,0x00,0x00,0x00,  // nMediaTypes=1
            0x00,0x00,0x00,0x00,  // reserved
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // lpMediaType=NULL (64-bit)
            0x00,0x00,0x00,0x00,  // cMediaTypesWorking=0
            0x00,0x00,0x00,0x00,  // reserved
            0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,  // lpMediaTypeWorking=NULL (64-bit)
            // MEDIATYPE_Video GUID (16 bytes):
            0x76,0x69,0x64,0x73,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71,
            // MEDIASUBTYPE_RGB24 GUID (16 bytes):
            0x7D,0xEB,0x36,0xE4,0x4F,0x52,0xCE,0x11,0x9F,0x53,0x00,0x20,0xAF,0x0B,0xA7,0x70,
        };
        RegSetValueExW(hk, L"FilterData", 0, REG_BINARY,
                       kFilterData, static_cast<DWORD>(sizeof(kFilterData)));
        RegCloseKey(hk);
    }

    registry_written_ = true;
    log::info("VirtualCamera: COM registration complete — HKLM=%s HKCU=%s — CLSID %ls → %ls",
              hklm_ok ? "OK" : "denied", hkcu_ok ? "OK" : "failed",
              clsid_str.c_str(), dll_path.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  remove_registry
// ─────────────────────────────────────────────────────────────────────────────
void VirtualCamera::remove_registry() {
    std::wstring clsid_str = guid_to_wstring(kIdhmfisCaptureCLSID);
    std::wstring cat_str   = guid_to_wstring(kVideoInputDeviceCat);

    std::wstring inst_key  = L"SOFTWARE\\Classes\\CLSID\\" + cat_str + L"\\Instance\\" + clsid_str;
    std::wstring base_key  = L"SOFTWARE\\Classes\\CLSID\\" + clsid_str;

    RegDeleteTreeW(HKEY_CURRENT_USER,  inst_key.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER,  base_key.c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, base_key.c_str()); // may fail without elevation, ignore

    log::info("VirtualCamera: unregistered CLSID %ls", clsid_str.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
//  start_mf_virtual_camera — register as MF virtual camera via Win11 API
//
//  MFCreateVirtualCamera is loaded at runtime from mfsensorgroup.dll so the
//  binary still runs on Windows 10 (where the API doesn't exist); it just
//  won't appear in MFEnumDeviceSources there.
//
//  IMFVirtualCamera vtable (from public Win11 SDK):
//    slot 0: QueryInterface, 1: AddRef, 2: Release
//    slot 3: Start(IMFAttributes*), 4: Stop(), 5: Shutdown()
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Minimal vtable view covering the methods we actually call.
struct IMinVirtualCamera : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Start(IUnknown* pConfig) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};

typedef HRESULT (WINAPI *PFN_MFStartup)(ULONG version, DWORD dwFlags);
typedef HRESULT (WINAPI *PFN_MFCreateVirtualCamera)(
    int type, int lifetime, int access,
    LPCWSTR pszFriendlyName,
    REFCLSID clsid,
    const GUID* pCategories,
    DWORD dwCategoryCount,
    IUnknown** ppVirtualCamera);

} // anonymous namespace

void VirtualCamera::start_mf_virtual_camera() {
    std::wstring clsid_str = guid_to_wstring(kIdhmfisCaptureCLSID);

    // Verify the InprocServer32 registry key is actually readable before calling
    // MFCreateVirtualCamera (which needs it to instantiate the COM server).
    {
        std::wstring check_key = L"Software\\Classes\\CLSID\\" + clsid_str + L"\\InprocServer32";
        HKEY hk = nullptr;
        LONG rc = RegOpenKeyExW(HKEY_CURRENT_USER, check_key.c_str(), 0, KEY_READ, &hk);
        if (rc != ERROR_SUCCESS) {
            log::error("VirtualCamera: InprocServer32 not found in HKCU (rc=%ld key=%ls) "
                       "— MFCreateVirtualCamera will fail with CO_E_CLASSSTRING",
                       rc, check_key.c_str());
        } else {
            WCHAR val[MAX_PATH]{};
            DWORD valSz = sizeof(val);
            DWORD type  = 0;
            RegQueryValueExW(hk, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(val), &valSz);
            RegCloseKey(hk);
            log::info("VirtualCamera: InprocServer32 confirmed in registry → '%ls'", val);
        }
    }

    // MF must be initialized before calling MFCreateVirtualCamera.
    HMODULE hMFPlat = GetModuleHandleW(L"mfplat.dll");
    if (!hMFPlat) hMFPlat = LoadLibraryW(L"mfplat.dll");
    if (hMFPlat) {
        auto pfnStartup = reinterpret_cast<PFN_MFStartup>(
            GetProcAddress(hMFPlat, "MFStartup"));
        if (pfnStartup) {
            HRESULT hr = pfnStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
            log::info("VirtualCamera: MFStartup hr=0x%08X", hr);
        }
    }

    HMODULE hSG = LoadLibraryW(L"mfsensorgroup.dll");
    if (!hSG) {
        log::warn("VirtualCamera: mfsensorgroup.dll not found — MF virtual camera unavailable (Win10?)");
        return;
    }
    log::info("VirtualCamera: mfsensorgroup.dll loaded");

    auto pfnCreate = reinterpret_cast<PFN_MFCreateVirtualCamera>(
        GetProcAddress(hSG, "MFCreateVirtualCamera"));
    if (!pfnCreate) {
        log::warn("VirtualCamera: MFCreateVirtualCamera not exported from mfsensorgroup.dll");
        FreeLibrary(hSG);
        return;
    }

    static const GUID kClsid = {
        0xCA1D0001u, 0xDAC0u, 0x1DA0u,
        { 0xBE, 0xEF, 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01 }
    };
    std::wstring wfname = to_wstring(kFriendlyName);

    log::info("VirtualCamera: calling MFCreateVirtualCamera(type=0 lifetime=0 access=0 name='%s' clsid=%ls)",
              kFriendlyName, clsid_str.c_str());

    // MFCreateVirtualCamera + Start can BLOCK INDEFINITELY when a prior instance
    // left a stale virtual-camera registration (after a crash / hard-kill) or the
    // MF frame-server service is wedged.  This runs during "Starting outputs", so
    // a hang here FREEZES the whole app at startup.  The MF virtual camera is an
    // OPTIONAL sidecar (SHM + NDI still deliver the feed), so we time-box it:
    // run the call on a detached worker thread and give up after a few seconds.
    struct VCamCreateResult {
        std::mutex mtx;
        bool       done = false;
        HRESULT    hr   = E_FAIL;
        IUnknown*  vcam = nullptr;
    };
    auto res = std::make_shared<VCamCreateResult>();

    std::thread([pfnCreate, wfname, res]() {
        IUnknown* pv = nullptr;
        HRESULT   h  = pfnCreate(0, 0, 0, wfname.c_str(), kClsid, nullptr, 0, &pv);
        if (SUCCEEDED(h) && pv) {
            auto* pMinW   = reinterpret_cast<IMinVirtualCamera*>(pv);
            HRESULT sh    = pMinW->Start(nullptr);
            if (FAILED(sh)) { pv->Release(); pv = nullptr; h = sh; }
        }
        std::lock_guard<std::mutex> lk(res->mtx);
        res->hr = h; res->vcam = pv; res->done = true;
    }).detach();

    constexpr int kVCamTimeoutMs = 4000;
    bool      done  = false;
    HRESULT   hr    = E_FAIL;
    IUnknown* pVCam = nullptr;
    for (int waited = 0; waited < kVCamTimeoutMs; waited += 20) {
        {
            std::lock_guard<std::mutex> lk(res->mtx);
            if (res->done) { done = true; hr = res->hr; pVCam = res->vcam; break; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!done) {
        // The worker is still parked inside mfsensorgroup.dll, so do NOT FreeLibrary
        // (it is still executing in it) — leak the handle and continue startup.
        log::warn("VirtualCamera: MFCreateVirtualCamera did not return within %d ms — "
                  "skipping MF virtual camera (SHM + NDI feed unaffected). Likely a stale "
                  "virtual-camera registration from a prior crash/hard-kill.", kVCamTimeoutMs);
        return;
    }

    FreeLibrary(hSG); // COM holds a ref to the DLL; FreeLibrary is safe here

    if (FAILED(hr) || !pVCam) {
        log::error("VirtualCamera: MFCreateVirtualCamera FAILED hr=0x%08X%s", hr,
                   hr == 0x800401F3 ? " (CO_E_CLASSSTRING: CLSID not in registry or DLL missing)" :
                   hr == 0x80040154 ? " (REGDB_E_CLASSNOTREG: COM server not registered)" :
                   hr == 0x80070005 ? " (E_ACCESSDENIED: needs elevation for HKLM?)" : "");
        return;
    }

    mf_vcam_ = static_cast<void*>(pVCam);
    log::info("VirtualCamera: MF virtual camera started — '%s' visible to MFEnumDeviceSources",
              kFriendlyName);
}

void VirtualCamera::stop_mf_virtual_camera() {
    if (!mf_vcam_) return;

    auto* pMin = reinterpret_cast<IMinVirtualCamera*>(mf_vcam_);
    pMin->Stop();
    pMin->Shutdown();
    pMin->Release();
    mf_vcam_ = nullptr;

    log::info("VirtualCamera: MF virtual camera stopped");
}

} // namespace idhmfis

#endif // _WIN32
