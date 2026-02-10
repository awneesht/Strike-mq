#include "utils/memory_pool.h"
#include <cassert>
#include <iostream>
#include <vector>
using namespace strike;

int main() {
    std::cout << "═══════════════════════════════════════\n  StrikeMQ Memory Pool Tests\n═══════════════════════════════════════\n\n";

    std::cout << "test_alloc_dealloc... ";
    { HugePagePool pool(4096, 1024);
      assert(pool.available() == 1024);
      void* p = pool.allocate(); assert(p); assert(pool.allocated() == 1);
      pool.deallocate(p); assert(pool.allocated() == 0); }
    std::cout << "PASS\n";

    std::cout << "test_exhaust_pool... ";
    { HugePagePool pool(64, 16);
      std::vector<void*> ptrs;
      for (int i = 0; i < 16; ++i) { void* p = pool.allocate(); assert(p); ptrs.push_back(p); }
      assert(!pool.allocate());
      for (auto* p : ptrs) pool.deallocate(p);
      assert(pool.available() == 16); }
    std::cout << "PASS\n";

    std::cout << "test_ownership... ";
    { HugePagePool pool(4096, 64);
      void* p = pool.allocate(); assert(pool.owns(p));
      int x; assert(!pool.owns(&x));
      pool.deallocate(p); }
    std::cout << "PASS\n";

    std::cout << "test_pool_buffer... ";
    { HugePagePool pool(4096, 64);
      { PoolBuffer buf(&pool, 100); assert(buf.valid()); assert(pool.allocated() == 1); }
      assert(pool.allocated() == 0); }
    std::cout << "PASS\n";

    std::cout << "\nAll tests passed! ✅\n"; return 0;
}
