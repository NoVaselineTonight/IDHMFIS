#pragma once
// Lock-free single-producer/single-consumer queue.
// Wraps readerwriterqueue by Cameron314.
// All render-bus-critical paths use this; never use mutex queues there.

#include <readerwriterqueue.h>
#include <concurrentqueue.h>
#include <chrono>
#include <optional>
#include <cstddef>

namespace idhmfis {

template<typename T, size_t Capacity = 256>
class SpscQueue {
public:
    explicit SpscQueue(size_t cap = Capacity)
        : q_(cap) {}

    // Producer side (single thread only)
    bool try_push(const T& item) { return q_.try_enqueue(item); }
    bool try_push(T&& item)      { return q_.try_enqueue(std::move(item)); }

    // L-1: the original push() busy-spun forever on a full queue.  On the real-
    // time engine thread that is a priority-inversion deadlock if the consumer
    // stalls: the high-priority engine spins, starving the consumer which would
    // drain the queue.  push_timeout() spins for at most timeout_us microseconds
    // and returns false on expiry so the caller can drop or log and continue.
    bool push_timeout(const T& item,
                      std::chrono::microseconds timeout_us = std::chrono::microseconds{500})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout_us;
        while (!q_.try_enqueue(item)) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
        }
        return true;
    }
    bool push_timeout(T&& item,
                      std::chrono::microseconds timeout_us = std::chrono::microseconds{500})
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout_us;
        while (!q_.try_enqueue(std::move(item))) {
            if (std::chrono::steady_clock::now() >= deadline)
                return false;
        }
        return true;
    }

    // Legacy blocking push — kept for callers not on the RT thread.
    // Do NOT use on the engine thread; use push_timeout() there.
    void push(const T& item)     { while (!q_.try_enqueue(item)) {} }
    void push(T&& item)          { while (!q_.try_enqueue(std::move(item))) {} }

    // Consumer side (single thread only)
    std::optional<T> try_pop() {
        T item;
        if (q_.try_dequeue(item)) return item;
        return std::nullopt;
    }

    bool try_pop(T& out) { return q_.try_dequeue(out); }

    size_t size_approx() const { return q_.size_approx(); }

private:
    moodycamel::ReaderWriterQueue<T> q_;
};

// Multi-producer, single-consumer variant for input aggregation
template<typename T>
class MpscQueue {
public:
    explicit MpscQueue(size_t cap = 1024) : q_(cap) {}

    bool try_push(const T& item) { return q_.try_enqueue(item); }
    bool try_push(T&& item)      { return q_.try_enqueue(std::move(item)); }

    bool try_pop(T& out)         { return q_.try_dequeue(out); }
    size_t size_approx() const   { return q_.size_approx(); }

private:
    moodycamel::ConcurrentQueue<T> q_;
};

} // namespace idhmfis
