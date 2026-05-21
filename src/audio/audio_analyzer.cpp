// audio_analyzer.cpp — WASAPI loopback capture + KissFFT + BPM tracker.
//
// On Windows, WASAPI shared-mode loopback capture is used: we capture the
// system render endpoint's output mix, giving us whatever the host is playing
// with zero additional configuration.
//
// FFT: KissFFT with a 2048-point Hann-windowed real FFT, hop of 512 samples.
// BPM: autocorrelation on onset-strength signal over a ~5-second history buffer.

#include "audio_analyzer.h"
#include "../core/logger.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <numeric>

// ── KissFFT ──────────────────────────────────────────────────────────────────
// We use the "real" variant: kiss_fftr.
// Provide a local implementation stub so the file compiles even without the
// external kissfft package; the real package will override at link time.
#if __has_include(<kissfft/kiss_fftr.h>)
#  include <kissfft/kiss_fftr.h>
#  define HAVE_KISSFFT 1
#elif __has_include(<kiss_fftr.h>)
#  include <kiss_fftr.h>
#  define HAVE_KISSFFT 1
#else
// Stub — produces a zero spectrum. Replace with real kissfft at build time.
#  define HAVE_KISSFFT 0
struct kiss_fft_cpx { float r, i; };
using kiss_fftr_cfg = void*;
inline kiss_fftr_cfg kiss_fftr_alloc(int, int, void*, size_t*) { return nullptr; }
inline void kiss_fftr(kiss_fftr_cfg, const float*, kiss_fft_cpx*) {}
inline void kiss_fft_free(void*) {}
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#  define NOMINMAX
#  endif
#  include <windows.h>
#  include <mmdeviceapi.h>
#  include <audioclient.h>
#  include <functiondiscoverykeys_devpkey.h>
#  pragma comment(lib, "ole32.lib")
#endif

#include <thread>
#include <chrono>
#include <mutex>

static constexpr float kPi = 3.14159265358979323846f;

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
AudioAnalyzer::AudioAnalyzer()
{
    // Build Hann window
    for (int i = 0; i < kFftSize; ++i)
        window_[i] = 0.5f * (1.f - std::cos(2.f * kPi * i / (kFftSize - 1)));

#if HAVE_KISSFFT
    fft_cfg_ = reinterpret_cast<kiss_fft_state*>(
        kiss_fftr_alloc(kFftSize, 0, nullptr, nullptr));
#endif

    current_snap_.bpm = 120.f;
}

AudioAnalyzer::~AudioAnalyzer()
{
    shutdown();
#if HAVE_KISSFFT
    if (fft_cfg_) kiss_fft_free(fft_cfg_);
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  init
// ─────────────────────────────────────────────────────────────────────────────
bool AudioAnalyzer::init(InputMode mode, const std::string& device_id)
{
    mode_      = mode;
    device_id_ = device_id;

    capture_running_.store(true, std::memory_order_release);

    switch (mode_)
    {
        case InputMode::WasapiLoopback:
            capture_thread_ = std::thread(&AudioAnalyzer::wasapi_loop, this);
            break;
        case InputMode::File:
            capture_thread_ = std::thread(&AudioAnalyzer::file_loop, this);
            break;
        default:
            // ASIO / NDI stubs: just run silence
            capture_thread_ = std::thread([this] {
                while (capture_running_.load(std::memory_order_acquire))
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            });
            break;
    }

    log::info("AudioAnalyzer: init mode=%d device='%s'",
              static_cast<int>(mode_), device_id_.c_str());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  shutdown
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::shutdown()
{
    capture_running_.store(false, std::memory_order_release);
    if (capture_thread_.joinable())
        capture_thread_.join();
    log::info("AudioAnalyzer: shutdown");
}

// ─────────────────────────────────────────────────────────────────────────────
//  tick — called by the engine at 1000 Hz; drains the ring buffer
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::tick()
{
    // Drain new samples from the ring buffer in hop-sized chunks.
    while (true)
    {
        int wpos = ring_write_.load(std::memory_order_acquire);
        int available = (wpos - ring_read_ + kRingSize) % kRingSize;
        if (available < kHopSize) break;

        // Copy kFftSize samples into a windowed buffer (with wrap-around).
        // We need kFftSize samples centered on the hop position.
        // For simplicity, always use the most recent kFftSize samples up to
        // the current read cursor + kHopSize.
        float buf[kFftSize]{};
        int start = (ring_read_ - (kFftSize - kHopSize) + kRingSize) % kRingSize;
        for (int i = 0; i < kFftSize; ++i)
            buf[i] = ring_[(start + i) % kRingSize] * window_[i];

        process_fft(buf, kFftSize);
        ring_read_ = (ring_read_ + kHopSize) % kRingSize;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  snapshot — thread-safe read of current analysis state
// ─────────────────────────────────────────────────────────────────────────────
AudioSnapshot AudioAnalyzer::snapshot() const
{
    std::lock_guard<std::mutex> lk(snap_mtx_);
    return current_snap_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  process_fft — windowed FFT + band energies
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::process_fft(const float* samples, int n)
{
    // Compute RMS and peak of the raw window
    float sum_sq = 0.f, peak = 0.f;
    for (int i = 0; i < n; ++i)
    {
        float s = samples[i];
        sum_sq += s * s;
        float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    float rms = std::sqrt(sum_sq / n);

    // Smooth RMS
    rms_smooth_ = rms_smooth_ * 0.85f + rms * 0.15f;

    // FFT magnitude spectrum
    static constexpr int kNBins = kFftSize / 2 + 1;
    float mag[kNBins]{};

#if HAVE_KISSFFT
    if (fft_cfg_)
    {
        kiss_fft_cpx cx[kNBins];
        kiss_fftr(reinterpret_cast<kiss_fftr_cfg>(fft_cfg_), samples, cx);
        for (int i = 0; i < kNBins; ++i)
            mag[i] = std::sqrt(cx[i].r * cx[i].r + cx[i].i * cx[i].i) / kFftSize;
    }
#else
    // Fallback: synthesise a crude spectrum from the time-domain RMS
    for (int i = 0; i < kNBins; ++i)
        mag[i] = rms * std::exp(-static_cast<float>(i) / 64.f);
#endif

    compute_bands(mag, kNBins, sample_rate_);

    // Onset strength: ratio of current energy to previous
    float onset = std::max(0.f, rms - prev_energy_);
    prev_energy_ = rms;
    update_bpm(onset);

    // Beat detection: simple threshold with refractory period
    bool beat_now = false;
    ++frames_since_beat_;
    // Minimum refractory: ~250 ms at 48 kHz, hop 512 → ~23 hops per second
    // ~250ms = ~5.75 hops → use 6
    if (onset > beat_threshold_ && frames_since_beat_ > 6)
    {
        beat_now = true;
        frames_since_beat_ = 0;
        // Adaptive threshold
        beat_threshold_ = beat_threshold_ * 0.9f + onset * 0.1f;
    }

    // Publish snapshot
    AudioSnapshot snap;
    snap.rms      = rms_smooth_;
    snap.peak     = peak;
    snap.beat_now = beat_now;

    // Fill FFT bins (downsample kNBins → kFFTBins)
    {
        static constexpr float kScale = 4.f; // perceptual normalisation
        for (int i = 0; i < AudioSnapshot::kFFTBins; ++i)
        {
            int src = static_cast<int>(
                static_cast<float>(i) / AudioSnapshot::kFFTBins * kNBins);
            snap.fft[i] = std::min(mag[src] * kScale, 1.f);
        }
    }

    {
        std::lock_guard<std::mutex> lk(snap_mtx_);
        // Preserve smoothed bands
        snap.sub_band  = current_snap_.sub_band;
        snap.mid_band  = current_snap_.mid_band;
        snap.high_band = current_snap_.high_band;
        snap.bpm       = current_snap_.bpm;
        current_snap_  = snap;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  compute_bands — compute sub/mid/high energy from FFT magnitudes
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::compute_bands(const float* mag, int n_bins, float sr)
{
    // Frequency resolution: sr / kFftSize Hz per bin
    float bin_hz = sr / kFftSize;

    float sub  = 0.f, mid = 0.f, high = 0.f;
    int   ns   = 0,   nm  = 0,   nh   = 0;

    for (int i = 0; i < n_bins; ++i)
    {
        float freq = i * bin_hz;
        float m    = mag[i];
        if (freq < 200.f)              { sub  += m * m; ++ns; }
        else if (freq < 2000.f)        { mid  += m * m; ++nm; }
        else if (freq < 20000.f)       { high += m * m; ++nh; }
    }

    float sub_rms  = ns  ? std::sqrt(sub  / ns)  : 0.f;
    float mid_rms  = nm  ? std::sqrt(mid  / nm)  : 0.f;
    float high_rms = nh  ? std::sqrt(high / nh)  : 0.f;

    // Smooth
    std::lock_guard<std::mutex> lk(snap_mtx_);
    current_snap_.sub_band  = current_snap_.sub_band  * 0.8f + sub_rms  * 0.2f;
    current_snap_.mid_band  = current_snap_.mid_band  * 0.8f + mid_rms  * 0.2f;
    current_snap_.high_band = current_snap_.high_band * 0.8f + high_rms * 0.2f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  update_bpm — autocorrelation BPM detection
//
//  Strategy: maintain a circular history of onset strengths.
//  Compute autocorrelation of the onset signal at lag τ ∈ [hops_at_40bpm,
//  hops_at_200bpm]. The lag with maximum correlation corresponds to the
//  beat period. Convert to BPM.
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::update_bpm(float onset)
{
    onset_history_[onset_write_] = onset;
    onset_write_ = (onset_write_ + 1) % kBpmHistLen;

    // Run BPM estimate every 64 hops (~1.4 s at 48 kHz/512 hop)
    if (++bpm_skip_counter_ < 64) return;
    bpm_skip_counter_ = 0;

    // Hop rate = sample_rate_ / kHopSize
    float hop_rate = sample_rate_ / kHopSize;

    // BPM range: 40–200 bpm
    int min_lag = static_cast<int>(hop_rate * 60.f / 200.f);
    int max_lag = static_cast<int>(hop_rate * 60.f /  40.f);
    max_lag = std::min(max_lag, kBpmHistLen / 2);
    min_lag = std::max(min_lag, 1);

    float best_corr = -1.f;
    int   best_lag  = static_cast<int>(hop_rate * 60.f / 120.f);

    for (int lag = min_lag; lag <= max_lag; ++lag)
    {
        float corr = 0.f;
        for (int i = 0; i < kBpmHistLen - lag; ++i)
        {
            int ia = (onset_write_ + i)       % kBpmHistLen;
            int ib = (onset_write_ + i + lag) % kBpmHistLen;
            corr += onset_history_[ia] * onset_history_[ib];
        }
        if (corr > best_corr) { best_corr = corr; best_lag = lag; }
    }

    if (best_lag > 0)
    {
        float period_s  = best_lag / hop_rate;
        float raw_bpm   = 60.f / period_s;
        // Double/halve into 60–180 range
        while (raw_bpm > 180.f) raw_bpm *= 0.5f;
        while (raw_bpm <  60.f) raw_bpm *= 2.f;

        std::lock_guard<std::mutex> lk(snap_mtx_);
        // Heavy smoothing — BPM should not jump wildly
        current_snap_.bpm = current_snap_.bpm * 0.95f + raw_bpm * 0.05f;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  wasapi_loop — Windows WASAPI loopback capture
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::wasapi_loop()
{
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice*           device     = nullptr;
    IAudioClient*        client     = nullptr;
    IAudioCaptureClient* capture    = nullptr;

    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));

    if (FAILED(hr)) { log::error("AudioAnalyzer: WASAPI enumerator failed hr=0x%08X", hr); goto cleanup_wasapi; }

    // Use the default render endpoint for loopback capture
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { log::error("AudioAnalyzer: GetDefaultAudioEndpoint failed hr=0x%08X", hr); goto cleanup_wasapi; }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    if (FAILED(hr)) { log::error("AudioAnalyzer: Activate IAudioClient failed hr=0x%08X", hr); goto cleanup_wasapi; }

    {
        WAVEFORMATEX* fmt = nullptr;
        hr = client->GetMixFormat(&fmt);
        if (FAILED(hr)) { log::error("AudioAnalyzer: GetMixFormat failed"); goto cleanup_wasapi; }

        sample_rate_ = static_cast<float>(fmt->nSamplesPerSec);
        int channels = fmt->nChannels;

        // Initialise loopback capture (AUDCLNT_STREAMFLAGS_LOOPBACK)
        hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK,
            0, 0, fmt, nullptr);

        CoTaskMemFree(fmt);
        if (FAILED(hr)) { log::error("AudioAnalyzer: Initialize failed hr=0x%08X", hr); goto cleanup_wasapi; }

        hr = client->GetService(__uuidof(IAudioCaptureClient),
                                reinterpret_cast<void**>(&capture));
        if (FAILED(hr)) { log::error("AudioAnalyzer: GetService failed hr=0x%08X", hr); goto cleanup_wasapi; }

        hr = client->Start();
        if (FAILED(hr)) { log::error("AudioAnalyzer: Start failed hr=0x%08X", hr); goto cleanup_wasapi; }

        log::info("AudioAnalyzer: WASAPI loopback started sr=%.0f ch=%d",
                  sample_rate_, channels);

        while (capture_running_.load(std::memory_order_acquire))
        {
            UINT32 pkt = 0;
            hr = capture->GetNextPacketSize(&pkt);
            if (FAILED(hr)) break; // Device lost — exit loop and clean up

            while (pkt > 0)
            {
                BYTE*  data   = nullptr;
                UINT32 frames = 0;
                DWORD  flags  = 0;
                hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr))
                {
                    // Convert interleaved float32 to mono and push to ring buffer.
                    // Most WASAPI loopback formats are float32.
                    const float* src = reinterpret_cast<const float*>(data);
                    for (UINT32 f = 0; f < frames; ++f)
                    {
                        float s = 0.f;
                        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        {
                            for (int c = 0; c < channels; ++c)
                                s += src[f * channels + c];
                            s /= channels;
                        }
                        int w = ring_write_.load(std::memory_order_relaxed);
                        ring_[w] = s;
                        ring_write_.store((w + 1) % kRingSize, std::memory_order_release);
                    }
                    capture->ReleaseBuffer(frames);
                }
                hr = capture->GetNextPacketSize(&pkt);
                if (FAILED(hr)) { pkt = 0; break; } // Device lost
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        client->Stop();
    }

cleanup_wasapi:
    if (capture)   capture->Release();
    if (client)    client->Release();
    if (device)    device->Release();
    if (enumerator) enumerator->Release();
    CoUninitialize();
#else
    // Non-Windows: run silent
    while (capture_running_.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
//  file_loop — read a raw PCM float file and loop it
// ─────────────────────────────────────────────────────────────────────────────
void AudioAnalyzer::file_loop()
{
    // Open the file as a raw binary stream of 32-bit float mono samples.
    std::vector<float> file_data;
    {
        FILE* f = std::fopen(device_id_.c_str(), "rb");
        if (!f)
        {
            log::error("AudioAnalyzer: cannot open file '%s'", device_id_.c_str());
            return;
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        if (sz > 0)
        {
            file_data.resize(sz / sizeof(float));
            fread(file_data.data(), sizeof(float), file_data.size(), f);
        }
        fclose(f);
    }

    if (file_data.empty())
    {
        log::warn("AudioAnalyzer: file '%s' empty or unreadable", device_id_.c_str());
        return;
    }

    log::info("AudioAnalyzer: file loop, %zu samples", file_data.size());

    size_t pos = 0;
    while (capture_running_.load(std::memory_order_acquire))
    {
        // Simulate realtime: push kHopSize samples every hop_period ms
        float hop_ms = (kHopSize / sample_rate_) * 1000.f;

        for (int i = 0; i < kHopSize; ++i)
        {
            float s = file_data[pos % file_data.size()];
            ++pos;
            int w = ring_write_.load(std::memory_order_relaxed);
            ring_[w] = s;
            ring_write_.store((w + 1) % kRingSize, std::memory_order_release);
        }

        std::this_thread::sleep_for(
            std::chrono::microseconds(static_cast<int>(hop_ms * 1000.f)));
    }
}

} // namespace idhmfis
