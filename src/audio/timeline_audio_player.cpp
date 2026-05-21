// timeline_audio_player.cpp — miniaudio-backed timeline audio playback.
// MINIAUDIO_IMPLEMENTATION must be defined in exactly one translation unit.

#define MINIAUDIO_IMPLEMENTATION
#ifdef _MSC_VER
#   pragma warning(push, 0)   // suppress all warnings from the third-party header
#endif
#include "miniaudio.h"
#ifdef _MSC_VER
#   pragma warning(pop)
#endif

#include "timeline_audio_player.h"
#include "../core/logger.h"

#include <cstring>
#ifdef _WIN32
#   include <windows.h>
#endif

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  utf8_to_wstring — Windows: convert a UTF-8 std::string to std::wstring so
//  that miniaudio's _w APIs can open paths that contain non-ANSI characters.
//  On Windows the char* overloads go through fopen_s / CP_ACP which will fail
//  for any path that the file dialog returns as UTF-8 but CP_ACP cannot encode.
// ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
static std::wstring utf8_to_wstring(const std::string& utf8)
{
    if (utf8.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0,
                                     utf8.c_str(), static_cast<int>(utf8.size()),
                                     nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0,
                        utf8.c_str(), static_cast<int>(utf8.size()),
                        &result[0], needed);
    return result;
}
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Internal state
// ─────────────────────────────────────────────────────────────────────────────
struct TimelineAudioPlayer::Impl {
    ma_engine engine;
    ma_sound  sound;
    bool      engine_ok  = false;
    bool      sound_ok   = false;

    Impl()
    {
        ma_engine_config cfg = ma_engine_config_init();
        engine_ok = (ma_engine_init(&cfg, &engine) == MA_SUCCESS);
    }

    ~Impl()
    {
        if (sound_ok) {
            ma_sound_uninit(&sound);
            sound_ok = false;
        }
        if (engine_ok) {
            ma_engine_uninit(&engine);
            engine_ok = false;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────
TimelineAudioPlayer::TimelineAudioPlayer()
    : impl_(new Impl())
{
}

TimelineAudioPlayer::~TimelineAudioPlayer()
{
    delete impl_;
}

// ─────────────────────────────────────────────────────────────────────────────
//  load
// ─────────────────────────────────────────────────────────────────────────────
bool TimelineAudioPlayer::load(const std::string& path)
{
    if (!impl_ || !impl_->engine_ok) return false;

    // Unload previous sound first
    unload();

    // MA_SOUND_FLAG_DECODE: decode to PCM upfront so seeking is instant.
    // MA_SOUND_FLAG_NO_SPATIALIZATION: 2-D stereo, no 3-D processing.
    // On Windows, use the wide-string API so UTF-8 paths (returned by
    // GetOpenFileNameW + WideCharToMultiByte) survive the round-trip through
    // CP_ACP that the char* overload would use internally via fopen_s.
#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(path);
    ma_result r = ma_sound_init_from_file_w(
        &impl_->engine,
        wpath.c_str(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr, nullptr,
        &impl_->sound);
#else
    ma_result r = ma_sound_init_from_file(
        &impl_->engine,
        path.c_str(),
        MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION,
        nullptr, nullptr,
        &impl_->sound);
#endif

    impl_->sound_ok = (r == MA_SUCCESS);
    return impl_->sound_ok;
}

// ─────────────────────────────────────────────────────────────────────────────
//  unload
// ─────────────────────────────────────────────────────────────────────────────
void TimelineAudioPlayer::unload()
{
    if (!impl_) return;
    if (impl_->sound_ok) {
        ma_sound_uninit(&impl_->sound);
        // Zero the struct so we can safely re-init into it later.
        std::memset(&impl_->sound, 0, sizeof(impl_->sound));
        impl_->sound_ok = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  play_from
// ─────────────────────────────────────────────────────────────────────────────
void TimelineAudioPlayer::play_from(double seconds)
{
    if (!impl_ || !impl_->sound_ok) return;

    // Convert seconds to PCM frames using the engine's sample rate.
    ma_uint32 sr = ma_engine_get_sample_rate(&impl_->engine);
    if (sr == 0) sr = 44100;
    ma_uint64 frame = static_cast<ma_uint64>(seconds * static_cast<double>(sr));

    ma_sound_seek_to_pcm_frame(&impl_->sound, frame);
    ma_sound_start(&impl_->sound);
}

// ─────────────────────────────────────────────────────────────────────────────
//  pause
// ─────────────────────────────────────────────────────────────────────────────
void TimelineAudioPlayer::pause()
{
    if (!impl_ || !impl_->sound_ok) return;
    ma_sound_stop(&impl_->sound);
}

// ─────────────────────────────────────────────────────────────────────────────
//  stop
// ─────────────────────────────────────────────────────────────────────────────
void TimelineAudioPlayer::stop()
{
    if (!impl_ || !impl_->sound_ok) return;
    ma_sound_stop(&impl_->sound);
    ma_sound_seek_to_pcm_frame(&impl_->sound, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  set_volume
// ─────────────────────────────────────────────────────────────────────────────
void TimelineAudioPlayer::set_volume(float v)
{
    if (!impl_ || !impl_->sound_ok) return;
    ma_sound_set_volume(&impl_->sound, v);
}

// ─────────────────────────────────────────────────────────────────────────────
//  is_playing
// ─────────────────────────────────────────────────────────────────────────────
bool TimelineAudioPlayer::is_playing() const
{
    if (!impl_ || !impl_->sound_ok) return false;
    return ma_sound_is_playing(&impl_->sound) == MA_TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
//  position_seconds
// ─────────────────────────────────────────────────────────────────────────────
double TimelineAudioPlayer::position_seconds() const
{
    if (!impl_ || !impl_->sound_ok) return 0.0;
    float pos_s = 0.f;
    ma_sound_get_cursor_in_seconds(&impl_->sound, &pos_s);
    return static_cast<double>(pos_s);
}

// ─────────────────────────────────────────────────────────────────────────────
//  audio_compute_peaks — decode any audio format and build a peak envelope.
//  Downsamples to 22050 Hz mono f32 for format-agnostic, fast decoding.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<float> audio_compute_peaks(const std::string& path)
{
    constexpr ma_uint32 kRate   = 22050;
    constexpr int       kBucket = 100;   // samples per peak bucket

    log::info("audio_compute_peaks: decoding '%s'", path.c_str());

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, kRate);
    ma_decoder        dec{};

    // On Windows the path string is UTF-8 (produced by WideCharToMultiByte
    // from GetOpenFileNameW).  miniaudio's char* overload uses fopen_s which
    // goes through CP_ACP — not UTF-8 — and will silently fail on any path
    // that CP_ACP cannot encode (or on any system without the "Use Unicode
    // UTF-8 for worldwide language support" option enabled).  Use the _w
    // variant with a proper UTF-16 conversion instead.
#ifdef _WIN32
    std::wstring wpath = utf8_to_wstring(path);
    ma_result init_result = ma_decoder_init_file_w(wpath.c_str(), &cfg, &dec);
#else
    ma_result init_result = ma_decoder_init_file(path.c_str(), &cfg, &dec);
#endif

    if (init_result != MA_SUCCESS) {
        log::warn("audio_compute_peaks: ma_decoder_init_file failed (ma_result=%d) for '%s'",
                  static_cast<int>(init_result), path.c_str());
        return {};
    }

    std::vector<float> peaks;
    peaks.reserve(4096);

    float   bucket_max = 0.f;
    int     bucket_cnt = 0;
    float   chunk[4096]{};

    for (;;) {
        ma_uint64 frames_read = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, chunk, 4096, &frames_read);
        for (ma_uint64 i = 0; i < frames_read; ++i) {
            float s = chunk[i] < 0.f ? -chunk[i] : chunk[i];
            if (s > bucket_max) bucket_max = s;
            if (++bucket_cnt >= kBucket) {
                peaks.push_back(bucket_max);
                bucket_max = 0.f;
                bucket_cnt = 0;
            }
        }
        if (r != MA_SUCCESS || frames_read < 4096) break;
    }
    if (bucket_cnt > 0) peaks.push_back(bucket_max);

    ma_decoder_uninit(&dec);
    log::info("audio_compute_peaks: produced %zu peaks for '%s'",
              peaks.size(), path.c_str());
    return peaks;
}

} // namespace idhmfis
