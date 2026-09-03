// Smoke test + rough latency sanity check for Order / OrderPool.
#include "lob/order_pool.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

int main() {
    using namespace lob;

    std::printf("sizeof(Order)=%zu alignof(Order)=%zu\n", sizeof(Order), alignof(Order));

    OrderPool pool(OrderPool::kDefaultCapacity);
    pool.prefault();

    if (pool.capacity() != 1'000'000 || pool.available() != 1'000'000 || pool.in_use() != 0) {
        std::puts("FAIL: initial accounting");
        return 1;
    }

    // --- allocate / populate / deallocate round trip -----------------------
    Order* a = pool.allocate();
    Order* b = pool.allocate();
    if (a == nullptr || b == nullptr || a == b) { std::puts("FAIL: allocate"); return 1; }
    if (pool.in_use() != 2) { std::puts("FAIL: in_use"); return 1; }

    *a = Order{.order_id = 1, .price = 100, .timestamp = 42,
               .prev = nullptr, .next = nullptr, .level = nullptr,
               .quantity = 500, .side = Side::BUY};
    *b = Order{.order_id = 2, .price = 101, .timestamp = 43,
               .prev = nullptr, .next = nullptr, .level = nullptr,
               .quantity = 250, .side = Side::SELL};

    if (!a->is_buy() || !b->is_sell() || opposite(a->side) != Side::SELL) {
        std::puts("FAIL: side"); return 1;
    }
    if (!pool.owns(a) || pool.from_index(pool.index_of(b)) != b) {
        std::puts("FAIL: handles"); return 1;
    }

    // --- intrusive list: link three, unlink the middle in O(1) -------------
    Order* c = pool.allocate();
    a->next = b; b->prev = a; b->next = c; c->prev = b;
    b->unlink();
    if (a->next != c || c->prev != a || b->prev != nullptr || b->next != nullptr) {
        std::puts("FAIL: unlink"); return 1;
    }

    pool.deallocate(c);
    pool.deallocate(b);
    pool.deallocate(a);
    if (pool.in_use() != 0) { std::puts("FAIL: free accounting"); return 1; }

    // --- exhaustion returns nullptr, never throws / grows ------------------
    {
        OrderPool tiny(2);
        Order* x = tiny.allocate();
        Order* y = tiny.allocate();
        if (x == nullptr || y == nullptr) { std::puts("FAIL: tiny alloc"); return 1; }
        if (!tiny.empty() || tiny.allocate() != nullptr) { std::puts("FAIL: exhaustion"); return 1; }
        tiny.deallocate(y);
        if (tiny.allocate() != y) { std::puts("FAIL: LIFO reuse"); return 1; }
    }

    // --- drain the whole pool and refill ------------------------------------
    std::vector<Order*> all;
    all.reserve(pool.capacity());
    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < pool.capacity(); ++i) {
        Order* o = pool.allocate();
        if (o == nullptr) { std::puts("FAIL: premature exhaustion"); return 1; }
        o->order_id = i;
        all.push_back(o);
    }
    auto t1 = std::chrono::steady_clock::now();
    for (Order* o : all) pool.deallocate(o);
    auto t2 = std::chrono::steady_clock::now();

    if (pool.available() != pool.capacity()) { std::puts("FAIL: refill"); return 1; }

    const double alloc_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / pool.capacity();
    const double free_ns  = std::chrono::duration<double, std::nano>(t2 - t1).count() / pool.capacity();
    std::printf("allocate: %.2f ns/op   deallocate: %.2f ns/op\n", alloc_ns, free_ns);

    pool.reset();
    if (pool.in_use() != 0) { std::puts("FAIL: reset"); return 1; }

    std::puts("ALL TESTS PASSED");
    return 0;
}
