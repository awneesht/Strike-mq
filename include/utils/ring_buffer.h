#pragma once
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

namespace strike {

static constexpr size_t kCacheLineSize = 64;
#define STRIKE_CACHE_ALIGNED alignas(strike::kCacheLineSize)

template <typename T, size_t Capacity>
class SPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static_assert(Capacity >= 2);

public:
    SPSCRingBuffer() : head_(0), tail_(0) {
        for (size_t i = 0; i < Capacity; ++i) new (&storage_[i]) T{};
    }

    [[nodiscard]] bool try_push(const T& item) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t next = head + 1;
        if (next - tail_cached_ > Capacity) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (next - tail_cached_ > Capacity) return false;
        }
        storage_[head & kMask] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(T&& item) noexcept {
        const uint64_t head = head_.load(std::memory_order_relaxed);
        const uint64_t next = head + 1;
        if (next - tail_cached_ > Capacity) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (next - tail_cached_ > Capacity) return false;
        }
        storage_[head & kMask] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (head_cached_ == tail) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if (head_cached_ == tail) return std::nullopt;
        }
        T item = std::move(storage_[tail & kMask]);
        tail_.store(tail + 1, std::memory_order_release);
        return item;
    }

    size_t try_pop_batch(T* output, size_t max_count) noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        head_cached_ = head_.load(std::memory_order_acquire);
        const size_t available = static_cast<size_t>(head_cached_ - tail);
        const size_t count = std::min(available, max_count);
        for (size_t i = 0; i < count; ++i)
            output[i] = std::move(storage_[(tail + i) & kMask]);
        if (count > 0) tail_.store(tail + count, std::memory_order_release);
        return count;
    }

    [[nodiscard]] size_t size_approx() const noexcept {
        return head_.load(std::memory_order_relaxed) - tail_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }
    [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }
    [[nodiscard]] double fill_ratio() const noexcept {
        return static_cast<double>(size_approx()) / Capacity;
    }

private:
    static constexpr uint64_t kMask = Capacity - 1;
    STRIKE_CACHE_ALIGNED std::atomic<uint64_t> head_;
    STRIKE_CACHE_ALIGNED uint64_t tail_cached_ = 0;
    STRIKE_CACHE_ALIGNED std::atomic<uint64_t> tail_;
    STRIKE_CACHE_ALIGNED uint64_t head_cached_ = 0;
    STRIKE_CACHE_ALIGNED T storage_[Capacity];
};

template <typename T, size_t Capacity>
class MPSCRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0);
public:
    MPSCRingBuffer() : head_(0), tail_(0) {}

    [[nodiscard]] bool try_push(const T& item) noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);
        while (true) {
            if (head - tail_.load(std::memory_order_acquire) >= Capacity) return false;
            if (head_.compare_exchange_weak(head, head + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
                storage_[head & kMask] = item;
                committed_[head & kMask].store(true, std::memory_order_release);
                return true;
            }
        }
    }

    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const uint64_t tail = tail_.load(std::memory_order_relaxed);
        if (tail >= head_.load(std::memory_order_acquire)) return std::nullopt;
        if (!committed_[tail & kMask].load(std::memory_order_acquire)) return std::nullopt;
        T item = std::move(storage_[tail & kMask]);
        committed_[tail & kMask].store(false, std::memory_order_relaxed);
        tail_.store(tail + 1, std::memory_order_release);
        return item;
    }

private:
    static constexpr uint64_t kMask = Capacity - 1;
    STRIKE_CACHE_ALIGNED std::atomic<uint64_t> head_;
    STRIKE_CACHE_ALIGNED std::atomic<uint64_t> tail_;
    STRIKE_CACHE_ALIGNED T storage_[Capacity];
    STRIKE_CACHE_ALIGNED std::atomic<bool> committed_[Capacity] = {};
};

} // namespace strike
