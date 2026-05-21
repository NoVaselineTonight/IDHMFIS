#pragma once
// Non-hot-path structured logger. Never call this from the render bus.
// Logs to file and stderr. Rotated at 10 MB. User can disable via env.
// Developer session log: opt-in, written to Logs/session.log next to the
// executable, truncated each launch. Controlled by enable_dev_logging() /
// disable_dev_logging() — wired to the Advanced Settings checkbox.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <array>
#include <atomic>
#include <mutex>
#include <chrono>
#include <filesystem>

namespace idhmfis::log {

enum class Level : uint8_t { Trace=0, Debug, Info, Warn, Error, Fatal };

namespace detail {

inline const char* level_str(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
    }
    return "?????";
}

struct Logger {
    std::mutex      mtx;
    FILE*           file      = nullptr;
    Level           min_level = Level::Info;
    std::atomic_bool enabled  = true;
    long            file_size = 0;
    std::string     log_path;
    static constexpr long kMaxBytes = 10 * 1024 * 1024;

    // ── Developer session log (opt-in, separate file) ─────────────────────────
    FILE*             dev_file            = nullptr;  // Logs/session.log, truncated each launch
    std::string       dev_path;
    std::atomic<bool> dev_logging_active_ {false};

    void open(const std::string& path) {
        log_path = path;
        file = std::fopen(path.c_str(), "a");
    }

    // Opens Logs/session.log relative to the exe directory, writes a header,
    // and starts routing all log() calls to it as well.
    void open_dev(const std::string& path) {
        std::lock_guard<std::mutex> lk(mtx);
        dev_path = path;
#if defined(_MSC_VER) || defined(_WIN32)
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "w");
        dev_file = f;
#else
        dev_file = std::fopen(path.c_str(), "w");
#endif
        if (!dev_file) return;

        dev_logging_active_.store(true, std::memory_order_relaxed);

        // Session header
        using Clock = std::chrono::system_clock;
        auto now = Clock::now();
        auto t   = Clock::to_time_t(now);
        std::tm tm_val{};
#if defined(_MSC_VER) || defined(_WIN32)
        localtime_s(&tm_val, &t);
#else
        localtime_r(&t, &tm_val);
#endif
        char ts[64];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_val);

        const char* build_config =
#if defined(NDEBUG)
            "Release";
#else
            "Debug";
#endif

        std::fputs(
            "===========================================\n"
            "IDHMFIS Developer Log\n", dev_file);
        std::fprintf(dev_file, "Session started: %s\n", ts);
#if defined(_WIN32)
        std::fputs("Platform: Windows\n", dev_file);
#elif defined(__APPLE__)
        std::fputs("Platform: macOS\n", dev_file);
#else
        std::fputs("Platform: Linux\n", dev_file);
#endif
        std::fprintf(dev_file, "Build: %s\n", build_config);
        std::fputs(
            "===========================================\n\n", dev_file);
        std::fflush(dev_file);
    }

    void close_dev() {
        std::lock_guard<std::mutex> lk(mtx);
        if (!dev_file) return;
        dev_logging_active_.store(false, std::memory_order_relaxed);
        std::fputs("\n[session end]\n", dev_file);
        std::fflush(dev_file);
        std::fclose(dev_file);
        dev_file = nullptr;
    }

    void write(Level lvl, const char* msg) {
        if (!enabled || lvl < min_level) return;
        using Clock = std::chrono::system_clock;
        auto now = std::chrono::system_clock::now();
        auto t   = std::chrono::system_clock::to_time_t(now);
        std::tm tm_val{};
#if defined(_MSC_VER) || defined(_WIN32)
        localtime_s(&tm_val, &t);
#else
        localtime_r(&t, &tm_val);
#endif
        // Include milliseconds for dev log
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch()) % 1000;
        char ts[32];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_val);

        char line[1024];
        int n = std::snprintf(line, sizeof(line), "[%s.%03d] [%s] %s\n",
                              ts, (int)ms.count(), level_str(lvl), msg);

        std::lock_guard<std::mutex> lk(mtx);
        if (lvl >= Level::Warn) std::fputs(line, stderr);
        if (file) {
            std::fwrite(line, 1, n, file);
            std::fflush(file);
            file_size += n;
            if (file_size > kMaxBytes) rotate();
        }
        if (dev_file) {
            std::fwrite(line, 1, n, dev_file);
            std::fflush(dev_file);
        }
    }

    void rotate() {
        std::fclose(file);
        std::string old = log_path + ".1";
        std::rename(log_path.c_str(), old.c_str());
        file = std::fopen(log_path.c_str(), "w");
        file_size = 0;
    }

    ~Logger() {
        if (file)     std::fclose(file);
        if (dev_file) std::fclose(dev_file);
    }
};

inline Logger& instance() {
    static Logger g;
    return g;
}

} // namespace detail

inline void init(const std::string& log_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(log_dir);
    detail::instance().open(log_dir + "/idhmfis.log");
}

inline void set_level(Level l)  { detail::instance().min_level = l; }
inline void set_enabled(bool e) { detail::instance().enabled = e; }

// ── Developer session log API ─────────────────────────────────────────────────
// Call enable_dev_logging() with the full path to Logs/session.log.
// The file is created / truncated, a session header is written, and all
// subsequent log() calls are mirrored there until disable_dev_logging().
inline void enable_dev_logging(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    detail::instance().open_dev(path);
}

inline void disable_dev_logging() {
    detail::instance().close_dev();
}

inline bool is_dev_logging_enabled() {
    return detail::instance().dev_logging_active_.load(std::memory_order_relaxed);
}

template<typename... Args>
inline void write(Level lvl, const char* fmt, Args&&... args) {
    char buf[1024];
    std::snprintf(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
    detail::instance().write(lvl, buf);
}

template<typename... Args> inline void trace(const char* f, Args&&... a) { write(Level::Trace,f,a...); }
template<typename... Args> inline void debug(const char* f, Args&&... a) { write(Level::Debug,f,a...); }
template<typename... Args> inline void info (const char* f, Args&&... a) { write(Level::Info ,f,a...); }
template<typename... Args> inline void warn (const char* f, Args&&... a) { write(Level::Warn ,f,a...); }
template<typename... Args> inline void error(const char* f, Args&&... a) { write(Level::Error,f,a...); }
template<typename... Args> inline void fatal(const char* f, Args&&... a) { write(Level::Fatal,f,a...); }

} // namespace idhmfis::log
