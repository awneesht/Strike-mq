#include "storage/sharded_log_map.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
using namespace strike;
using namespace strike::storage;

static const std::string kTestDir = "/tmp/strikemq_test_sharded";

static void cleanup_test_dir() {
    std::error_code ec;
    std::filesystem::remove_all(kTestDir, ec);
}

void test_find_empty() {
    std::cout << "test_find_empty... ";
    cleanup_test_dir();
    ShardedLogMap<8> map;
    TopicPartition tp{"test-topic", 0};
    assert(map.find(tp) == nullptr);
    std::cout << "PASS\n";
}

void test_get_or_create() {
    std::cout << "test_get_or_create... ";
    cleanup_test_dir();
    ShardedLogMap<8> map;
    TopicPartition tp{"test-topic", 0};
    int new_topic_count = 0;

    auto& log = map.get_or_create(tp, kTestDir, 1048576,
        [&](const TopicPartition&) { new_topic_count++; });

    assert(new_topic_count == 1);
    assert(log.next_offset() == 0);

    // Should find it now
    auto* found = map.find(tp);
    assert(found != nullptr);
    assert(found == &log);

    // Second get_or_create should not call on_new_topic
    auto& log2 = map.get_or_create(tp, kTestDir, 1048576,
        [&](const TopicPartition&) { new_topic_count++; });
    assert(new_topic_count == 1);
    assert(&log2 == &log);

    cleanup_test_dir();
    std::cout << "PASS\n";
}

void test_multiple_partitions() {
    std::cout << "test_multiple_partitions... ";
    cleanup_test_dir();
    ShardedLogMap<8> map;
    std::vector<std::string> created_topics;

    for (int i = 0; i < 16; i++) {
        TopicPartition tp{"topic-" + std::to_string(i), i % 4};
        map.get_or_create(tp, kTestDir, 1048576,
            [&](const TopicPartition& t) { created_topics.push_back(t.topic); });
    }

    assert(created_topics.size() == 16);

    // Verify all are findable
    for (int i = 0; i < 16; i++) {
        TopicPartition tp{"topic-" + std::to_string(i), i % 4};
        assert(map.find(tp) != nullptr);
    }

    // Non-existent should return nullptr
    TopicPartition missing{"nonexistent", 0};
    assert(map.find(missing) == nullptr);

    cleanup_test_dir();
    std::cout << "PASS\n";
}

void test_for_each() {
    std::cout << "test_for_each... ";
    cleanup_test_dir();
    ShardedLogMap<8> map;

    for (int i = 0; i < 8; i++) {
        TopicPartition tp{"topic-" + std::to_string(i), 0};
        map.get_or_create(tp, kTestDir, 1048576, [](const TopicPartition&) {});
    }

    int count = 0;
    map.for_each([&](const TopicPartition&, PartitionLog&) { count++; });
    assert(count == 8);

    cleanup_test_dir();
    std::cout << "PASS\n";
}

void test_erase() {
    std::cout << "test_erase... ";
    cleanup_test_dir();
    ShardedLogMap<8> map;

    TopicPartition tp1{"topic-a", 0};
    TopicPartition tp2{"topic-b", 0};
    map.get_or_create(tp1, kTestDir, 1048576, [](const TopicPartition&) {});
    map.get_or_create(tp2, kTestDir, 1048576, [](const TopicPartition&) {});

    assert(map.find(tp1) != nullptr);
    assert(map.erase(tp1));
    assert(map.find(tp1) == nullptr);
    assert(map.find(tp2) != nullptr);

    // Erase non-existent
    assert(!map.erase(tp1));

    cleanup_test_dir();
    std::cout << "PASS\n";
}

void test_erase_if() {
    std::cout << "test_erase_if... ";
    cleanup_test_dir();
    ShardedLogMap<8> map;

    for (int i = 0; i < 10; i++) {
        TopicPartition tp{"topic-x", i};
        map.get_or_create(tp, kTestDir, 1048576, [](const TopicPartition&) {});
    }
    // Also add a different topic
    TopicPartition other{"topic-y", 0};
    map.get_or_create(other, kTestDir, 1048576, [](const TopicPartition&) {});

    auto erased = map.erase_if([](const TopicPartition& tp, PartitionLog&) {
        return tp.topic == "topic-x";
    });
    assert(erased.size() == 10);

    // topic-y should still exist
    assert(map.find(other) != nullptr);

    int remaining = 0;
    map.for_each([&](const TopicPartition&, PartitionLog&) { remaining++; });
    assert(remaining == 1);

    cleanup_test_dir();
    std::cout << "PASS\n";
}

void test_concurrent_get_or_create() {
    std::cout << "test_concurrent_get_or_create... ";
    cleanup_test_dir();
    ShardedLogMap<64> map;
    constexpr int kThreads = 8;
    constexpr int kTopicsPerThread = 50;
    std::atomic<int> created{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kTopicsPerThread; i++) {
                TopicPartition tp{"concurrent-" + std::to_string(t) + "-" + std::to_string(i), 0};
                map.get_or_create(tp, kTestDir, 1048576,
                    [&](const TopicPartition&) { created.fetch_add(1); });
            }
        });
    }
    for (auto& t : threads) t.join();

    assert(created.load() == kThreads * kTopicsPerThread);

    // Verify all are findable
    int total = 0;
    map.for_each([&](const TopicPartition&, PartitionLog&) { total++; });
    assert(total == kThreads * kTopicsPerThread);

    cleanup_test_dir();
    std::cout << "PASS (" << kThreads << " threads x " << kTopicsPerThread << " topics)\n";
}

void test_concurrent_find_and_create() {
    std::cout << "test_concurrent_find_and_create... ";
    cleanup_test_dir();
    ShardedLogMap<64> map;

    // Pre-create some topics
    for (int i = 0; i < 100; i++) {
        TopicPartition tp{"shared-" + std::to_string(i), 0};
        map.get_or_create(tp, kTestDir, 1048576, [](const TopicPartition&) {});
    }

    constexpr int kThreads = 8;
    constexpr int kOps = 10000;
    std::atomic<int> found{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&] {
            for (int i = 0; i < kOps; i++) {
                TopicPartition tp{"shared-" + std::to_string(i % 100), 0};
                auto* log = map.find(tp);
                if (log) found.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) t.join();

    assert(found.load() == kThreads * kOps);

    cleanup_test_dir();
    std::cout << "PASS (" << kThreads << " threads x " << kOps << " finds)\n";
}

int main() {
    std::cout << "═══════════════════════════════════════\n"
              << "  StrikeMQ Sharded Log Map Tests\n"
              << "═══════════════════════════════════════\n\n";

    test_find_empty();
    test_get_or_create();
    test_multiple_partitions();
    test_for_each();
    test_erase();
    test_erase_if();
    test_concurrent_get_or_create();
    test_concurrent_find_and_create();

    std::cout << "\nAll tests passed!\n";
    cleanup_test_dir();
    return 0;
}
