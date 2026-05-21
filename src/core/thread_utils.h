#pragma once
// Thread priority and affinity helpers. Platform-specific.

#include <thread>
#include <string>
#include <stdexcept>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#elif defined(__APPLE__)
#  include <pthread.h>
#  include <mach/mach.h>
#  include <mach/thread_policy.h>
#endif

namespace idhmfis {

enum class ThreadPriority {
    Normal,
    AboveNormal,
    High,
    Realtime   // Use for DAC output thread only
};

inline void set_thread_priority(std::thread& t, ThreadPriority p) {
#ifdef _WIN32
    HANDLE h = t.native_handle();
    int wp = THREAD_PRIORITY_NORMAL;
    switch (p) {
        case ThreadPriority::AboveNormal: wp = THREAD_PRIORITY_ABOVE_NORMAL; break;
        case ThreadPriority::High:        wp = THREAD_PRIORITY_HIGHEST;      break;
        case ThreadPriority::Realtime:    wp = THREAD_PRIORITY_TIME_CRITICAL; break;
        default: break;
    }
    SetThreadPriority(h, wp);
#elif defined(__APPLE__)
    int policy = SCHED_OTHER;
    struct sched_param sp{};
    switch (p) {
        case ThreadPriority::High:
        case ThreadPriority::Realtime:
            policy = SCHED_RR;
            sp.sched_priority = sched_get_priority_max(SCHED_RR);
            break;
        default: break;
    }
    pthread_setschedparam(t.native_handle(), policy, &sp);
#endif
}

inline void set_thread_priority_current(ThreadPriority p) {
#ifdef _WIN32
    int wp = THREAD_PRIORITY_NORMAL;
    switch (p) {
        case ThreadPriority::AboveNormal: wp = THREAD_PRIORITY_ABOVE_NORMAL;  break;
        case ThreadPriority::High:        wp = THREAD_PRIORITY_HIGHEST;       break;
        case ThreadPriority::Realtime:    wp = THREAD_PRIORITY_TIME_CRITICAL; break;
        default: break;
    }
    SetThreadPriority(GetCurrentThread(), wp);
#elif defined(__APPLE__)
    int policy = SCHED_OTHER;
    struct sched_param sp{};
    if (p == ThreadPriority::High || p == ThreadPriority::Realtime) {
        policy = SCHED_RR;
        sp.sched_priority = sched_get_priority_max(SCHED_RR);
    }
    pthread_setschedparam(pthread_self(), policy, &sp);
#endif
}

inline void set_thread_name(const std::string& name) {
#ifdef _WIN32
    // SetThreadDescription requires Windows 10 1607+
    using FnType = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto fn = (FnType)GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                             "SetThreadDescription");
    if (fn) {
        std::wstring wname(name.begin(), name.end());
        fn(GetCurrentThread(), wname.c_str());
    }
#elif defined(__APPLE__)
    pthread_setname_np(name.substr(0, 15).c_str());
#endif
}

// Spin-wait until the clock ticks past the given absolute time (nanoseconds).
// Used in the DAC output thread for sample-accurate scheduling.
inline void spin_until_ns(int64_t target_ns) {
    using Clock = std::chrono::steady_clock;
    auto target = Clock::time_point(std::chrono::nanoseconds(target_ns));
    // Yield-sleep until close, then spin
    while (Clock::now() < target - std::chrono::microseconds(200))
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    while (Clock::now() < target) { /* tight spin */ }
}

} // namespace idhmfis
