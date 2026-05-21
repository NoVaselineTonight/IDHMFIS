#pragma once
// High-resolution wall-clock timer. Nanosecond resolution on all platforms.

#include <chrono>
#include <cstdint>

namespace idhmfis {

class HRTimer {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    HRTimer() : start_(Clock::now()) {}

    void reset() { start_ = Clock::now(); }

    double elapsed_s()  const { return elapsed_ns() * 1e-9; }
    double elapsed_ms() const { return elapsed_ns() * 1e-6; }
    int64_t elapsed_us() const { return elapsed_ns() / 1000; }
    int64_t elapsed_ns() const {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - start_).count();
    }

    static double now_s() {
        static const TimePoint epoch = Clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - epoch).count() * 1e-9;
    }

private:
    TimePoint start_;
};

// Rolling average for latency tracking
template<int N = 64>
class RollingStats {
public:
    void push(double v) {
        sum_ -= buf_[idx_];
        buf_[idx_] = v;
        sum_ += v;
        idx_ = (idx_ + 1) % N;
        if (count_ < N) ++count_;
        if (v > p99_approx_) p99_approx_ = v;
        else p99_approx_ = p99_approx_ * 0.999 + v * 0.001;
    }
    double mean()    const { return count_ ? sum_ / count_ : 0.0; }
    double p99()     const { return p99_approx_; }
    int    count()   const { return count_; }
    void   clear()         { buf_ = {}; sum_ = 0; idx_ = 0; count_ = 0; p99_approx_ = 0; }

private:
    std::array<double, N> buf_{};
    double sum_        = 0.0;
    double p99_approx_ = 0.0;
    int    idx_        = 0;
    int    count_      = 0;
};

} // namespace idhmfis
