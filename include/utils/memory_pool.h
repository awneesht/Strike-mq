#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>

namespace strike {

struct BlockHeader { BlockHeader* next; };

class HugePagePool {
public:
    HugePagePool(size_t block_size, size_t num_blocks)
        : block_size_(align_up(std::max(block_size, sizeof(BlockHeader)), 64))
        , num_blocks_(num_blocks)
        , total_size_(block_size_ * num_blocks)
        , allocated_count_(0)
    {
        const size_t page_size = 4096;
        const size_t aligned_size = align_up(total_size_, page_size);

#ifdef STRIKE_PLATFORM_LINUX
        base_ = static_cast<uint8_t*>(mmap(nullptr, aligned_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0));
        if (base_ == MAP_FAILED) {
            base_ = static_cast<uint8_t*>(mmap(nullptr, aligned_size,
                PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));
            using_huge_pages_ = false;
        } else {
            using_huge_pages_ = true;
        }
#else
        base_ = static_cast<uint8_t*>(mmap(nullptr, aligned_size,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        using_huge_pages_ = false;
#endif
        if (base_ == MAP_FAILED)
            throw std::runtime_error("Failed to allocate memory pool");

        mmap_size_ = aligned_size;

        for (size_t i = 0; i < aligned_size; i += 4096)
            static_cast<volatile uint8_t*>(base_)[i] = 0;

        freelist_ = nullptr;
        for (size_t i = 0; i < num_blocks; ++i) {
            auto* hdr = reinterpret_cast<BlockHeader*>(base_ + i * block_size_);
            hdr->next = freelist_;
            freelist_ = hdr;
        }
    }

    ~HugePagePool() {
        if (base_ && base_ != MAP_FAILED) munmap(base_, mmap_size_);
    }

    HugePagePool(const HugePagePool&) = delete;
    HugePagePool& operator=(const HugePagePool&) = delete;

    [[nodiscard]] void* allocate() noexcept {
        if (!freelist_) return nullptr;
        BlockHeader* block = freelist_;
        freelist_ = block->next;
        ++allocated_count_;
        return static_cast<void*>(block);
    }

    void deallocate(void* ptr) noexcept {
        if (!ptr) return;
        auto* block = static_cast<BlockHeader*>(ptr);
        block->next = freelist_;
        freelist_ = block;
        --allocated_count_;
    }

    [[nodiscard]] uint8_t* get_buffer() noexcept {
        return static_cast<uint8_t*>(allocate());
    }

    [[nodiscard]] size_t block_size() const noexcept { return block_size_; }
    [[nodiscard]] size_t num_blocks() const noexcept { return num_blocks_; }
    [[nodiscard]] size_t allocated() const noexcept { return allocated_count_; }
    [[nodiscard]] size_t available() const noexcept { return num_blocks_ - allocated_count_; }
    [[nodiscard]] bool using_huge_pages() const noexcept { return using_huge_pages_; }
    [[nodiscard]] bool owns(const void* ptr) const noexcept {
        const auto* p = static_cast<const uint8_t*>(ptr);
        return p >= base_ && p < base_ + total_size_;
    }

private:
    static constexpr size_t align_up(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }
    uint8_t* base_ = nullptr;
    size_t mmap_size_ = 0;
    size_t block_size_;
    size_t num_blocks_;
    size_t total_size_;
    size_t allocated_count_;
    bool using_huge_pages_ = false;
    BlockHeader* freelist_ = nullptr;
};

class PoolBuffer {
public:
    PoolBuffer() = default;
    PoolBuffer(HugePagePool* pool, size_t size)
        : pool_(pool), data_(pool->get_buffer()), size_(size) {}
    ~PoolBuffer() { if (pool_ && data_) pool_->deallocate(data_); }

    PoolBuffer(PoolBuffer&& o) noexcept : pool_(o.pool_), data_(o.data_), size_(o.size_) {
        o.pool_ = nullptr; o.data_ = nullptr; o.size_ = 0;
    }
    PoolBuffer& operator=(PoolBuffer&& o) noexcept {
        if (this != &o) {
            if (pool_ && data_) pool_->deallocate(data_);
            pool_ = o.pool_; data_ = o.data_; size_ = o.size_;
            o.pool_ = nullptr; o.data_ = nullptr; o.size_ = 0;
        }
        return *this;
    }
    PoolBuffer(const PoolBuffer&) = delete;
    PoolBuffer& operator=(const PoolBuffer&) = delete;

    [[nodiscard]] uint8_t* data() noexcept { return data_; }
    [[nodiscard]] const uint8_t* data() const noexcept { return data_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

private:
    HugePagePool* pool_ = nullptr;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
};

} // namespace strike
