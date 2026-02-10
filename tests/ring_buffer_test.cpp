#include "utils/ring_buffer.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>
using namespace strike;

void test_spsc_basic() {
    std::cout << "test_spsc_basic... ";
    SPSCRingBuffer<int, 16> r;
    assert(r.empty_approx());
    assert(r.try_push(42));
    auto v = r.try_pop(); assert(v && *v == 42);
    assert(r.empty_approx());
    std::cout << "PASS\n";
}

void test_spsc_fill() {
    std::cout << "test_spsc_fill... ";
    SPSCRingBuffer<int, 8> r;
    for (int i = 0; i < 8; ++i) assert(r.try_push(i));
    assert(!r.try_push(99));
    for (int i = 0; i < 8; ++i) { auto v = r.try_pop(); assert(v && *v == i); }
    assert(!r.try_pop());
    std::cout << "PASS\n";
}

void test_spsc_batch() {
    std::cout << "test_spsc_batch... ";
    SPSCRingBuffer<int, 64> r;
    for (int i = 0; i < 32; ++i) (void)r.try_push(i);
    int out[64]; size_t n = r.try_pop_batch(out, 64);
    assert(n == 32);
    for (size_t i = 0; i < n; ++i) assert(out[i] == (int)i);
    std::cout << "PASS\n";
}

void test_spsc_concurrent() {
    std::cout << "test_spsc_concurrent... ";
    SPSCRingBuffer<uint64_t, 65536> r;
    constexpr uint64_t N = 10000000;
    uint64_t sp = 0, sc = 0;
    std::thread prod([&]{ for (uint64_t i=0;i<N;++i){ while(!r.try_push(i)); sp+=i; }});
    std::thread cons([&]{ uint64_t c=0; while(c<N){ auto v=r.try_pop(); if(v){sc+=*v;++c;} }});
    prod.join(); cons.join();
    assert(sp == sc);
    std::cout << "PASS (" << N << " items)\n";
}

void test_mpsc() {
    std::cout << "test_mpsc_concurrent... ";
    MPSCRingBuffer<uint64_t, 65536> r;
    constexpr int NP = 4; constexpr uint64_t IPP = 100000;
    std::atomic<uint64_t> tp{0};
    std::vector<std::thread> prods;
    for (int p=0;p<NP;++p) prods.emplace_back([&,p]{
        for (uint64_t i=0;i<IPP;++i){ uint64_t v=p*IPP+i; while(!r.try_push(v)); tp.fetch_add(v); }
    });
    uint64_t tc=0; uint64_t cnt=0;
    while(cnt < NP*IPP) { auto v=r.try_pop(); if(v){tc+=*v;++cnt;} }
    for(auto&t:prods) t.join();
    assert(tc == tp.load());
    std::cout << "PASS (" << cnt << " items)\n";
}

int main() {
    std::cout << "═══════════════════════════════════════\n  StrikeMQ Ring Buffer Tests\n═══════════════════════════════════════\n\n";
    test_spsc_basic(); test_spsc_fill(); test_spsc_batch(); test_spsc_concurrent(); test_mpsc();
    std::cout << "\nAll tests passed! ✅\n"; return 0;
}
