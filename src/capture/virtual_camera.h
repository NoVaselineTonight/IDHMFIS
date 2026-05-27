#pragma once
// virtual_camera.h — VirtualCamera (shared-memory producer)
//
// Renders IDHMFIS laser frames into a named shared memory segment
// (Local\IDHMFISCameraFrame) that IDHMFISCapture.dll reads and delivers
// to Windows Media Foundation consumers (Capture 2024, etc.).
//
// Architecture:
//   IDHMFIS.exe (this class)  →  SHM  →  IDHMFISCapture.dll (IMFMediaSource)
//                                              ↓
//                                     MF consumer (Capture 2024)
//
// This class handles:
//   - Creating / mapping the named SHM
//   - Rasterising PointBuffer into BGR24 and writing to SHM
//   - Registering IDHMFISCapture.dll in HKCU (InprocServer32 for COM)
//   - Creating an MF virtual camera via MFCreateVirtualCamera (Win11+)

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

#include "camera_shm.h"
#include "core/types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace idhmfis {

class VirtualCamera {
public:
    static constexpr int  kWidth        = kCamWidth;
    static constexpr int  kHeight       = kCamHeight;
    static constexpr int  kFPS          = kCamFPS;
    static constexpr const char* kFriendlyName = kCamFriendlyName;

    VirtualCamera();
    ~VirtualCamera();

    VirtualCamera(const VirtualCamera&)            = delete;
    VirtualCamera& operator=(const VirtualCamera&) = delete;
    VirtualCamera(VirtualCamera&&)                 = delete;
    VirtualCamera& operator=(VirtualCamera&&)      = delete;

    // Set stream index before open().  Index 0 = primary ("IDHMFIS Laser Preview"),
    // index N>0 = "IDHMFIS Laser Preview N+1" / indexed SHM name.
    void set_stream_index(int idx) { stream_index_ = idx; }

    // Create shared memory, register DLL in HKCU, return true on success.
    [[nodiscard]] bool open();

    // Close shared memory handles, remove HKCU entries.
    void close();

    [[nodiscard]] bool is_open() const noexcept { return open_.load(); }

    // Rasterise pts → BGR24 → write to shared memory.
    // Thread-safe; may be called from the DAC output thread.
    void push_frame(const PointBuffer& pts);

private:
    // Rasteriser
    void rasterise(const PointBuffer& pts);
    void draw_segment(int x0, int y0, int x1, int y1,
                      uint8_t r, uint8_t g, uint8_t b, int thickness);
    void blend_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b, float alpha);
    static int ilda_to_px(int16_t v, int dim) noexcept;

    // Registry helpers
    bool write_registry();
    void remove_registry();

    // State
    std::atomic<bool>    open_{ false };
    bool                 registry_written_{ false };
    int                  stream_index_{ 0 };

    // Local rasterise buffer (BGR24, kWidth*kHeight*3 bytes, bottom-up).
    std::vector<uint8_t> rgb_;
    std::mutex           push_mutex_;

    // Shared memory (producer side)
    HANDLE      shm_handle_{ nullptr };
    HANDLE      cam_mutex_{ nullptr };
    CameraShm*  shm_view_{ nullptr };

    // MF virtual camera handle (Win11+, stored as void* to avoid COM header deps)
    void*       mf_vcam_{ nullptr };

    void start_mf_virtual_camera();
    void stop_mf_virtual_camera();

    // Rate-limiter: cap rasterize to 30fps so the DAC output thread
    // isn't stalled by heavy image work on every engine tick.
    using Clock = std::chrono::steady_clock;
    Clock::time_point    last_push_{ Clock::time_point::min() };
};

} // namespace idhmfis

#else // ── Non-Windows stub ──────────────────────────────────────────────────

#include "core/types.h"
namespace idhmfis {
class VirtualCamera {
public:
    [[nodiscard]] bool open()  { return false; }
    void               close() {}
    [[nodiscard]] bool is_open() const noexcept { return false; }
    void               push_frame(const PointBuffer&) {}
};
} // namespace idhmfis

#endif // _WIN32
