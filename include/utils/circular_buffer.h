#pragma once
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace strike {

/// Fixed-capacity circular buffer for I/O. Replaces std::vector<uint8_t>
/// for connection read/write buffers — eliminates erase/realloc overhead.
///
/// Uses power-of-2 capacity with monotonic head/tail counters and & mask
/// wrapping. All operations are O(1).
class FixedCircularBuffer {
public:
    /// Construct with given capacity (rounded up to next power of 2).
    /// Allocates aligned heap memory.
    explicit FixedCircularBuffer(size_t min_capacity = 131072 /* 128KB */)
        : capacity_(next_power_of_2(min_capacity))
        , mask_(capacity_ - 1)
        , head_(0)
        , tail_(0)
    {
        // Page-align for io_uring registered buffers when capacity >= 4096;
        // otherwise use cache-line alignment. std::aligned_alloc requires
        // size to be a multiple of alignment.
        size_t align = (capacity_ >= 4096) ? 4096 : 64;
        size_t alloc_size = (capacity_ + align - 1) & ~(align - 1);
        data_ = static_cast<uint8_t*>(std::aligned_alloc(align, alloc_size));
        if (!data_) throw std::bad_alloc();
        owned_ = true;
    }

    /// Construct over externally-owned memory (no allocation/deallocation).
    FixedCircularBuffer(uint8_t* buf, size_t capacity)
        : data_(buf)
        , capacity_(capacity)
        , mask_(capacity - 1)
        , head_(0)
        , tail_(0)
        , owned_(false)
    {
        assert((capacity & (capacity - 1)) == 0 && "Capacity must be power of 2");
    }

    ~FixedCircularBuffer() {
        if (owned_ && data_) std::free(data_);
    }

    // Move-only
    FixedCircularBuffer(FixedCircularBuffer&& o) noexcept
        : data_(o.data_), capacity_(o.capacity_), mask_(o.mask_),
          head_(o.head_), tail_(o.tail_), owned_(o.owned_)
    {
        o.data_ = nullptr;
        o.owned_ = false;
        o.head_ = o.tail_ = 0;
    }

    FixedCircularBuffer& operator=(FixedCircularBuffer&& o) noexcept {
        if (this != &o) {
            if (owned_ && data_) std::free(data_);
            data_ = o.data_; capacity_ = o.capacity_; mask_ = o.mask_;
            head_ = o.head_; tail_ = o.tail_; owned_ = o.owned_;
            o.data_ = nullptr; o.owned_ = false; o.head_ = o.tail_ = 0;
        }
        return *this;
    }

    FixedCircularBuffer(const FixedCircularBuffer&) = delete;
    FixedCircularBuffer& operator=(const FixedCircularBuffer&) = delete;

    // ─── Write side (producer) ───

    /// Returns pointer and contiguous length available for writing (e.g., recv target).
    /// After writing n bytes, call advance_head(n).
    std::pair<uint8_t*, size_t> write_head() noexcept {
        size_t pos = head_ & mask_;
        size_t contig = capacity_ - pos; // bytes until wrap
        size_t avail = capacity_ - size(); // total free space
        return {data_ + pos, std::min(contig, avail)};
    }

    /// Advance write position after writing n bytes.
    void advance_head(size_t n) noexcept {
        head_ += n;
    }

    /// Copy data into the buffer (handles wrap-around).
    size_t write(const uint8_t* src, size_t len) noexcept {
        size_t avail = capacity_ - size();
        size_t to_write = std::min(len, avail);
        if (to_write == 0) return 0;

        size_t pos = head_ & mask_;
        size_t first = std::min(to_write, capacity_ - pos);
        std::memcpy(data_ + pos, src, first);
        if (to_write > first) {
            std::memcpy(data_, src + first, to_write - first);
        }
        head_ += to_write;
        return to_write;
    }

    // ─── Read side (consumer) ───

    /// Returns pointer and contiguous length available for reading (e.g., send source).
    /// After consuming n bytes, call advance_tail(n).
    std::pair<const uint8_t*, size_t> read_head() const noexcept {
        size_t pos = tail_ & mask_;
        size_t contig = capacity_ - pos;
        size_t avail = size();
        return {data_ + pos, std::min(contig, avail)};
    }

    /// Advance read position after consuming n bytes.
    void advance_tail(size_t n) noexcept {
        tail_ += n;
    }

    /// Peek at readable data starting at logical offset from tail.
    /// Copies up to `len` bytes into `dst`, handling wrap-around.
    /// Returns number of bytes copied.
    size_t peek(uint8_t* dst, size_t len) const noexcept {
        size_t avail = size();
        size_t to_copy = std::min(len, avail);
        if (to_copy == 0) return 0;

        size_t pos = tail_ & mask_;
        size_t first = std::min(to_copy, capacity_ - pos);
        std::memcpy(dst, data_ + pos, first);
        if (to_copy > first) {
            std::memcpy(dst + first, data_, to_copy - first);
        }
        return to_copy;
    }

    /// Read and consume up to `len` bytes into `dst`.
    size_t read(uint8_t* dst, size_t len) noexcept {
        size_t n = peek(dst, len);
        tail_ += n;
        return n;
    }

    // ─── State queries ───

    [[nodiscard]] size_t size() const noexcept { return head_ - tail_; }
    [[nodiscard]] size_t free_space() const noexcept { return capacity_ - size(); }
    [[nodiscard]] bool empty() const noexcept { return head_ == tail_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint8_t* data() noexcept { return data_; }
    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }

    void reset() noexcept { head_ = tail_ = 0; }

private:
    static size_t next_power_of_2(size_t v) {
        if (v == 0) return 1;
        v--;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16; v |= v >> 32;
        return v + 1;
    }

    uint8_t* data_ = nullptr;
    size_t capacity_;
    size_t mask_;
    size_t head_;    // monotonic write counter
    size_t tail_;    // monotonic read counter
    bool owned_ = false;
};

} // namespace strike
