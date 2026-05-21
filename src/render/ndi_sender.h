#pragma once
// ndi_sender.h — NDI video/audio output sender.
// Dynamically loads the NDI runtime library at runtime.
// If NDI is not installed, init() returns false and the app continues without NDI.

#include <atomic>
#include <cstdint>
#include <string>

namespace idhmfis {

// ---------------------------------------------------------------------------
// Forward declarations of NDI SDK types (avoids including NDI headers).
// We load the DLL dynamically so we never link against NDI directly.
// These match the NDI SDK 5 / 6 C ABI exactly.
// ---------------------------------------------------------------------------

// NDI video frame (v2 — works with both SDK 5 and 6)
// Field types must match NDIlib_video_frame_v2_t exactly (NDI SDK C ABI).
// frame_rate_N / frame_rate_D are int in the SDK, not float — mismatching
// these would corrupt the bit-pattern interpretation on the NDI side.
struct NdiVideoFrameV2 {
    int                xres                = 0;
    int                yres                = 0;
    int                FourCC              = 0;  // NDIlib_FourCC_video_type_e
    int                frame_rate_N        = 0;  // e.g. 60 (numerator)
    int                frame_rate_D        = 1;  // e.g.  1 (denominator)
    float              picture_aspect_ratio= 0.f;
    int                frame_format_type   = 1;  // NDIlib_frame_format_type_progressive
    int64_t            timecode            = 0;
    uint8_t*           p_data              = nullptr;
    int                line_stride_in_bytes= 0;
    const char*        p_metadata          = nullptr;
    int64_t            timestamp           = 0;
};

// NDI audio frame v3
struct NdiAudioFrameV3 {
    int     sample_rate    = 48000;
    int     no_channels    = 2;
    int     no_samples     = 0;
    int64_t timecode       = 0;
    int     FourCC         = 0;    // NDIlib_FourCC_audio_type_FLTP
    float*  p_data         = nullptr;
    int     channel_stride_in_bytes = 0;
    const char* p_metadata = nullptr;
    int64_t timestamp      = 0;
};

// NDI send create descriptor
struct NdiSendCreateV2 {
    const char* p_ndi_name      = nullptr;
    const char* p_groups        = nullptr;
    bool        clock_video     = false;
    bool        clock_audio     = false;
};

// ---------------------------------------------------------------------------
// Function pointer typedefs (NDI SDK C ABI)
// ---------------------------------------------------------------------------
using FnNdiInitialize    = bool  (*)();
using FnNdiDestroy       = void  (*)();
using FnNdiSendCreate    = void* (*)(const NdiSendCreateV2*);
using FnNdiSendDestroy   = void  (*)(void*);
using FnNdiSendVideoV2   = void  (*)(void*, const NdiVideoFrameV2*);
using FnNdiSendAudioV3   = void  (*)(void*, const NdiAudioFrameV3*);
using FnNdiSendConnections = int (*)(void*, uint32_t);

// ---------------------------------------------------------------------------
// NdiSender
// ---------------------------------------------------------------------------
class NdiSender {
public:
    NdiSender();
    ~NdiSender();

    // Initialize the NDI sender.
    // source_name: the name shown in NDI Studio Monitor (e.g. "IDHMFIS")
    // fps_N/fps_D: rational frame rate (e.g. 60000/1001 for 59.94, 60/1 for 60)
    // bgra: true = BGRA (32-bit RGBA), false = UYVY (YCbCr 4:2:2, 2 bytes/pixel)
    // clock_video: true = NDI paces sends internally (stable, +1 frame latency).
    //              false = caller paces sends (lower latency, drops visible on engine hiccup).
    // Returns false if NDI runtime is not installed or cannot be initialised.
    bool init(const std::string& source_name, int width, int height,
              int fps_N, int fps_D, bool bgra = false, bool clock_video = true);

    // Poll receiver connection count without sending a frame.
    // Call from the UI/snapshot thread when no frames are being sent so
    // the NDI status indicator does not show stale connected/disconnected state.
    void poll_connections();

    // Send a BGRA frame.
    // data must point to width * height * 4 bytes (BGRA, row-major, no padding).
    // timecode_100ns: wall-clock timecode in 100-nanosecond units.
    // Non-blocking: if the send queue is full the frame is silently dropped.
    void send_bgra(const uint8_t* data, int width, int height, int64_t timecode_100ns);

    // Send a UYVY frame.
    // data must point to width * height * 2 bytes.
    void send_uyvy(const uint8_t* data, int width, int height, int64_t timecode_100ns);

    // Send a floating-point audio frame alongside video.
    // samples: MUST be planar FLTP format (channel 0 contiguous, then channel 1).
    //          Interleaved stereo is NOT accepted — de-interleave before calling.
    // num_channels: 1 or 2.
    // sample_rate: e.g. 48000.
    // timecode_100ns: wall-clock timecode matching the accompanying video frame.
    void send_audio(const float* samples, int num_samples,
                    int num_channels, int sample_rate,
                    int64_t timecode_100ns = 0);

    bool        is_connected()    const;
    bool        is_initialized()  const { return have_ndi_; }  // true if DLL loaded & sender created
    int         num_connections() const;
    std::string source_name()    const { return source_name_; }

    void shutdown();

private:
    bool try_load_ndi_lib();
    bool try_load_from_path(const std::string& path);
    void unload_ndi_lib();

    std::string source_name_;
    bool        bgra_mode_    = false;
    bool        clock_video_  = true;  // NDI-clocked (true) vs. app-clocked (false)
    int         width_        = 1920;
    int         height_       = 1080;
    int         fps_N_        = 60;   // frame rate numerator
    int         fps_D_        = 1;    // frame rate denominator

    // NDI instance (opaque pointer)
    void* ndi_send_ = nullptr;

    // Loaded DLL handle (HMODULE on Windows, void* on POSIX)
#ifdef _WIN32
    void* lib_handle_ = nullptr;
#else
    void* lib_handle_ = nullptr;
#endif

    bool have_ndi_ = false;

    // Function pointers (populated after successful DLL load)
    FnNdiInitialize     fn_init_       = nullptr;
    FnNdiDestroy        fn_destroy_    = nullptr;
    FnNdiSendCreate     fn_send_create_= nullptr;
    FnNdiSendDestroy    fn_send_destroy_= nullptr;
    FnNdiSendVideoV2    fn_send_video_ = nullptr;
    FnNdiSendAudioV3    fn_send_audio_ = nullptr;
    FnNdiSendConnections fn_connections_= nullptr;

    std::atomic<int> connections_{0};
};

} // namespace idhmfis
