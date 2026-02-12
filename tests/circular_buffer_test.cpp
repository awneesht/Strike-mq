#include "utils/circular_buffer.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace strike;

void test_basic_write_read() {
    std::cout << "test_basic_write_read... ";
    FixedCircularBuffer buf(64);
    assert(buf.empty());
    assert(buf.size() == 0);
    assert(buf.capacity() == 64);

    uint8_t data[] = {1, 2, 3, 4, 5};
    size_t written = buf.write(data, 5);
    assert(written == 5);
    assert(buf.size() == 5);
    assert(!buf.empty());

    uint8_t out[5];
    size_t n = buf.read(out, 5);
    assert(n == 5);
    assert(std::memcmp(data, out, 5) == 0);
    assert(buf.empty());
    std::cout << "PASS\n";
}

void test_write_head_advance() {
    std::cout << "test_write_head_advance... ";
    FixedCircularBuffer buf(64);

    auto [ptr, avail] = buf.write_head();
    assert(avail == 64);
    std::memcpy(ptr, "hello", 5);
    buf.advance_head(5);
    assert(buf.size() == 5);

    auto [rptr, ravail] = buf.read_head();
    assert(ravail == 5);
    assert(std::memcmp(rptr, "hello", 5) == 0);
    buf.advance_tail(5);
    assert(buf.empty());
    std::cout << "PASS\n";
}

void test_wrap_around() {
    std::cout << "test_wrap_around... ";
    FixedCircularBuffer buf(16); // 16-byte ring

    // Fill 12 bytes
    uint8_t fill[12];
    for (int i = 0; i < 12; i++) fill[i] = static_cast<uint8_t>(i);
    buf.write(fill, 12);
    assert(buf.size() == 12);

    // Consume 10 bytes — tail is now at 10
    uint8_t drain[10];
    buf.read(drain, 10);
    assert(buf.size() == 2);
    for (int i = 0; i < 10; i++) assert(drain[i] == static_cast<uint8_t>(i));

    // Write 10 more bytes — this wraps around
    uint8_t more[10];
    for (int i = 0; i < 10; i++) more[i] = static_cast<uint8_t>(100 + i);
    size_t written = buf.write(more, 10);
    assert(written == 10);
    assert(buf.size() == 12);

    // Read remaining 2 old bytes + 10 new bytes
    uint8_t out[12];
    size_t n = buf.peek(out, 12);
    assert(n == 12);
    assert(out[0] == 10 && out[1] == 11);
    for (int i = 0; i < 10; i++) assert(out[2 + i] == static_cast<uint8_t>(100 + i));
    std::cout << "PASS\n";
}

void test_full_buffer() {
    std::cout << "test_full_buffer... ";
    FixedCircularBuffer buf(16);
    uint8_t data[16];
    for (int i = 0; i < 16; i++) data[i] = static_cast<uint8_t>(i);

    size_t written = buf.write(data, 16);
    assert(written == 16);
    assert(buf.size() == 16);
    assert(buf.free_space() == 0);

    // Attempt to write more — should write 0
    uint8_t extra = 0xFF;
    written = buf.write(&extra, 1);
    assert(written == 0);

    // Read all
    uint8_t out[16];
    size_t n = buf.read(out, 16);
    assert(n == 16);
    assert(std::memcmp(data, out, 16) == 0);
    assert(buf.empty());
    std::cout << "PASS\n";
}

void test_contiguous_read_at_boundary() {
    std::cout << "test_contiguous_read_at_boundary... ";
    FixedCircularBuffer buf(16);

    // Write 14 bytes, consume 14 — tail at 14
    uint8_t tmp[14];
    buf.write(tmp, 14);
    buf.read(tmp, 14);
    assert(buf.empty());

    // Now write 8 bytes — wraps around at pos 14
    uint8_t data[] = {10, 20, 30, 40, 50, 60, 70, 80};
    buf.write(data, 8);
    assert(buf.size() == 8);

    // First contiguous read should only give 2 bytes (14..16)
    auto [ptr, contig] = buf.read_head();
    assert(contig == 2);
    assert(ptr[0] == 10 && ptr[1] == 20);

    // But peek can get all 8
    uint8_t out[8];
    size_t n = buf.peek(out, 8);
    assert(n == 8);
    assert(std::memcmp(out, data, 8) == 0);
    std::cout << "PASS\n";
}

void test_power_of_2_rounding() {
    std::cout << "test_power_of_2_rounding... ";
    FixedCircularBuffer buf(100); // should round up to 128
    assert(buf.capacity() == 128);

    FixedCircularBuffer buf2(1); // minimum
    assert(buf2.capacity() == 1);

    FixedCircularBuffer buf3(256); // exact power
    assert(buf3.capacity() == 256);
    std::cout << "PASS\n";
}

void test_reset() {
    std::cout << "test_reset... ";
    FixedCircularBuffer buf(64);
    uint8_t data[] = {1, 2, 3};
    buf.write(data, 3);
    assert(!buf.empty());
    buf.reset();
    assert(buf.empty());
    assert(buf.size() == 0);
    std::cout << "PASS\n";
}

void test_move_semantics() {
    std::cout << "test_move_semantics... ";
    FixedCircularBuffer buf1(64);
    uint8_t data[] = {1, 2, 3, 4, 5};
    buf1.write(data, 5);

    FixedCircularBuffer buf2(std::move(buf1));
    assert(buf2.size() == 5);

    uint8_t out[5];
    buf2.read(out, 5);
    assert(std::memcmp(data, out, 5) == 0);
    std::cout << "PASS\n";
}

void test_large_throughput() {
    std::cout << "test_large_throughput... ";
    FixedCircularBuffer buf(131072); // 128KB
    constexpr size_t kChunkSize = 4096;
    constexpr int kIterations = 1000;

    uint8_t chunk[kChunkSize];
    for (size_t i = 0; i < kChunkSize; i++) chunk[i] = static_cast<uint8_t>(i & 0xFF);

    for (int i = 0; i < kIterations; i++) {
        // Write a chunk
        auto [wptr, wavail] = buf.write_head();
        size_t to_write = std::min(wavail, kChunkSize);
        if (to_write > 0) {
            std::memcpy(wptr, chunk, to_write);
            buf.advance_head(to_write);
        }

        // Read a chunk
        auto [rptr, ravail] = buf.read_head();
        if (ravail > 0) {
            buf.advance_tail(ravail);
        }
    }
    std::cout << "PASS (" << kIterations << " iterations)\n";
}

int main() {
    std::cout << "═══════════════════════════════════════\n"
              << "  StrikeMQ Circular Buffer Tests\n"
              << "═══════════════════════════════════════\n\n";
    test_basic_write_read();
    test_write_head_advance();
    test_wrap_around();
    test_full_buffer();
    test_contiguous_read_at_boundary();
    test_power_of_2_rounding();
    test_reset();
    test_move_semantics();
    test_large_throughput();
    std::cout << "\nAll tests passed!\n";
    return 0;
}
