#pragma once
// AudioAnalyzer — captures audio from WASAPI loopback (Windows) or file,
// runs a real-time FFT via KissFFT, detects BPM via autocorrelation,
// and exposes a lock-free AudioSnapshot for consumption by the show engine.

#include "../core/types.h"
#include <string>
#include <atomic>
#include <thread>
#include <vector>
#include <memory>
#include <mutex>
#include <array>

// KissFFT forward declare (included in .cpp only to keep build times low)
struct kiss_fft_state;
using kiss_fft_cfg = kiss_fft_state*;

namespace idhmfis {

class AudioAnalyzer {
public:
    enum class InputMode {
        WasapiLoopback,
        AsioDevice,
        File,
        NdiAudio
    };

    AudioAnalyzer();
    ~AudioAnalyzer();

    // Initialise and start capture.
    // device_id: for WASAPI/ASIO the endpoint ID; for File the path.
    bool init(InputMode mode, const std::string& device_id = "");
    void shutdown();

    // Called by show engine on every 1 ms tick.
    // Pulls freshly captured samples and re-runs analysis if a new hop is ready.
    void tick();

    // Thread-safe, lock-free read of the current audio analysis state.
    AudioSnapshot snapshot() const;

private:
    // ── Capture threads ───────────────────────────────────────────────────
    void wasapi_loop();
    void file_loop();

    // ── Signal processing ─────────────────────────────────────────────────
    // Feed one hop of samples through the FFT pipeline.
    void process_fft(const float* samples, int n);

    // Update BPM estimate from onset strength signal.
    void update_bpm(float onset_strength);

    // Compute band energies from FFT magnitude spectrum.
    // BUG #3 fix: returns raw (unsmoothed) band RMS values via out parameters
    // instead of writing directly to current_snap_ under a separate lock.
    void compute_bands(const float* mag, int n_bins, float sample_rate,
                       float& out_sub, float& out_mid, float& out_high);

    // ── Internal state ────────────────────────────────────────────────────
    static constexpr int kFftSize = 2048;
    static constexpr int kHopSize =  512;
    static constexpr int kBpmHistLen = 256; // autocorrelation window

    InputMode   mode_    = InputMode::WasapiLoopback;
    std::string device_id_;
    float       sample_rate_ = 48000.f;

    // Ring buffer for incoming PCM samples (mono float)
    static constexpr int kRingSize = 32768;
    std::array<float, kRingSize> ring_{};
    std::atomic<int> ring_write_{0};
    int              ring_read_  = 0;

    // FFT window (Hann)
    std::array<float, kFftSize> window_{};

    // KissFFT
    kiss_fft_cfg fft_cfg_ = nullptr;

    // BPM autocorrelation history of onset strength
    std::array<float, kBpmHistLen> onset_history_{};
    int onset_write_ = 0;
    float prev_energy_ = 0.f;

    // Beat detection simple threshold
    int   frames_since_beat_ = 0;
    float beat_threshold_    = 0.15f;

    // Published snapshot — written under snap_mtx_, read via snapshot()
    mutable std::mutex  snap_mtx_;
    AudioSnapshot       current_snap_;

    // Capture thread
    std::atomic<bool>   capture_running_{false};
    std::thread         capture_thread_;

    // Silence detector: suppress processing when input is silent
    float rms_smooth_ = 0.f;

    int bpm_skip_counter_ = 0;
};

} // namespace idhmfis
