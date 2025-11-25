#pragma once
#include <atomic>
#include <cstddef>
#include <vector>

namespace quant {

constexpr std::size_t Q_CACHELINE_SIZE = 64;

template<typename T>
// Single-producer single-consumer bounded ring buffer.
// - Capacity rounded to next power-of-two; wrap via mask.
// - Contract: exactly 1 producer thread calling push, exactly 1 consumer thread calling pop.
// - Uses acquire/release order to avoid locks and minimize latency.
class SPSCQueue {
public:
    // capacity will be rounded up to the next power-of-two for masking.
    explicit SPSCQueue(std::size_t capacity) {
        std::size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        capacity_ = cap;
        mask_ = capacity_ - 1;
        buffer_.resize(capacity_);
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Producer-only. Returns false if queue is full (caller decides drop/backpressure policy).
    bool push(const T& item) {
        auto head = head_.load(std::memory_order_relaxed);
        auto tail = tail_cached_;
        if (((head + 1) & mask_) == tail) {
            tail = tail_cached_ = tail_.load(std::memory_order_acquire);
            if (((head + 1) & mask_) == tail) {
                return false; // full
            }
        }
        buffer_[head] = item;
        head_.store((head + 1) & mask_, std::memory_order_release);
        return true;
    }

    // Consumer-only. Returns false if queue is empty.
    bool pop(T& item) {
        auto tail = tail_.load(std::memory_order_relaxed);
        auto head = head_cached_;
        if (tail == head) {
            head = head_cached_ = head_.load(std::memory_order_acquire);
            if (tail == head) {
                return false; // empty
            }
        }
        item = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    // Approximate size (non-atomic snapshot); suitable for telemetry.
    std::size_t approx_size() const {
        auto head = head_.load(std::memory_order_acquire);
        auto tail = tail_.load(std::memory_order_acquire);
        return (head + capacity_ - tail) & mask_;
    }

    // Configured capacity (rounded to power-of-two).
    std::size_t capacity() const { return capacity_; }

private:
    std::size_t capacity_{0};
    std::size_t mask_{0};
    std::vector<T> buffer_;

    alignas(Q_CACHELINE_SIZE) std::atomic<std::size_t> head_;
    mutable std::size_t head_cached_ = 0;

    alignas(Q_CACHELINE_SIZE) std::atomic<std::size_t> tail_;
    mutable std::size_t tail_cached_ = 0;
};

} // namespace quant
