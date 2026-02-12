#pragma once
#include "core/types.h"
#include "storage/partition_log.h"
#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace strike {
namespace storage {

/// Sharded map from TopicPartition -> PartitionLog.
/// Replaces global logs_mu + unordered_map with N cache-line-aligned shards
/// for reduced lock contention under concurrent Produce/Fetch/ListOffsets.
template <size_t NumShards = 64>
class ShardedLogMap {
    static_assert((NumShards & (NumShards - 1)) == 0, "NumShards must be power of 2");

public:
    ShardedLogMap() = default;

    // Non-copyable, non-movable (contains mutexes)
    ShardedLogMap(const ShardedLogMap&) = delete;
    ShardedLogMap& operator=(const ShardedLogMap&) = delete;

    /// Look up an existing partition log. Returns nullptr if not found.
    PartitionLog* find(const TopicPartition& tp) {
        auto& shard = shard_for(tp);
        std::lock_guard<std::mutex> lock(shard.mu);
        auto it = shard.map.find(tp);
        return it != shard.map.end() ? it->second.get() : nullptr;
    }

    /// Get existing or create new partition log.
    /// on_new_topic is called (under shard lock) for newly created logs.
    template <typename F>
    PartitionLog& get_or_create(const TopicPartition& tp,
                                const std::string& log_dir,
                                uint64_t segment_size,
                                F&& on_new_topic) {
        auto& shard = shard_for(tp);
        std::lock_guard<std::mutex> lock(shard.mu);
        auto it = shard.map.find(tp);
        if (it != shard.map.end()) return *it->second;

        auto log = std::make_unique<PartitionLog>(tp, log_dir, segment_size);
        auto& ref = *log;
        shard.map.emplace(tp, std::move(log));
        on_new_topic(tp);
        return ref;
    }

    /// Iterate all logs. Locks each shard sequentially.
    /// fn signature: void(const TopicPartition&, PartitionLog&)
    template <typename F>
    void for_each(F&& fn) {
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mu);
            for (auto& [tp, log] : shard.map) {
                fn(tp, *log);
            }
        }
    }

    /// Iterate all logs (const version).
    template <typename F>
    void for_each(F&& fn) const {
        for (const auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mu);
            for (const auto& [tp, log] : shard.map) {
                fn(tp, static_cast<const PartitionLog&>(*log));
            }
        }
    }

    /// Erase a specific topic-partition. Returns true if found and erased.
    bool erase(const TopicPartition& tp) {
        auto& shard = shard_for(tp);
        std::lock_guard<std::mutex> lock(shard.mu);
        return shard.map.erase(tp) > 0;
    }

    /// Erase all partitions matching a predicate.
    /// pred signature: bool(const TopicPartition&, PartitionLog&)
    /// Returns the list of erased TopicPartitions.
    template <typename Pred>
    std::vector<TopicPartition> erase_if(Pred&& pred) {
        std::vector<TopicPartition> erased;
        for (auto& shard : shards_) {
            std::lock_guard<std::mutex> lock(shard.mu);
            for (auto it = shard.map.begin(); it != shard.map.end(); ) {
                if (pred(it->first, *it->second)) {
                    erased.push_back(it->first);
                    it = shard.map.erase(it);
                } else {
                    ++it;
                }
            }
        }
        return erased;
    }

private:
    static constexpr size_t kShardMask = NumShards - 1;

    struct alignas(64) Shard {
        mutable std::mutex mu;
        std::unordered_map<TopicPartition,
                           std::unique_ptr<PartitionLog>,
                           TopicPartitionHash> map;
    };

    Shard& shard_for(const TopicPartition& tp) {
        return shards_[TopicPartitionHash{}(tp) & kShardMask];
    }

    const Shard& shard_for(const TopicPartition& tp) const {
        return shards_[TopicPartitionHash{}(tp) & kShardMask];
    }

    std::array<Shard, NumShards> shards_;
};

} // namespace storage
} // namespace strike
