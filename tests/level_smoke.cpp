// Smoke test for PriceLevel + OrderTracker, driven through the OrderPool.
#include "lob/order_map.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"

#include <chrono>
#include <cstdio>
#include <type_traits>
#include <vector>

using namespace lob;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

Order* make(OrderPool& pool, OrderId id, Price px, Quantity qty, Side side) {
    Order* o = pool.allocate();
    *o = Order{.order_id = id, .price = px, .timestamp = id,
               .prev = nullptr, .next = nullptr, .level = nullptr,
               .quantity = qty, .side = side};
    return o;
}

} // namespace

int main() {
    OrderPool    pool(4096);
    OrderTracker tracker(4096);
    PriceLevel   level(10'000);

    check(level.empty() && level.total_volume() == 0 && level.verify(), "empty level");
    check(level.price() == 10'000, "price");
    check(level.pop_front() == nullptr, "pop_front on empty");

    // --- FIFO ordering ------------------------------------------------------
    Order* a = make(pool, 1, 10'000, 100, Side::BUY);
    Order* b = make(pool, 2, 10'000, 200, Side::BUY);
    Order* c = make(pool, 3, 10'000, 300, Side::BUY);
    for (Order* o : {a, b, c}) {
        level.append_order(o);
        check(tracker.track(o->order_id, o), "track");
    }
    check(level.order_count() == 3 && level.total_volume() == 600, "append accounting");
    check(level.front() == a && level.back() == c, "head/tail");
    check(a->next == b && b->next == c && c->next == nullptr, "forward links");
    check(c->prev == b && b->prev == a && a->prev == nullptr, "backward links");
    check(level.verify(), "verify after appends");

    // --- O(1) cancel by id, middle of the queue -----------------------------
    Order* cancelled = tracker.untrack(2);
    check(cancelled == b, "untrack returns the order");
    level.remove_order(cancelled);
    pool.deallocate(cancelled);
    check(level.order_count() == 2 && level.total_volume() == 400, "remove middle");
    check(a->next == c && c->prev == a, "links repaired");
    check(tracker.find(2) == nullptr && tracker.size() == 2, "untracked");
    check(level.verify(), "verify after middle removal");

    // --- head removal, tail removal ----------------------------------------
    level.remove_order(a);
    check(level.front() == c && level.back() == c && c->prev == nullptr, "remove head");
    level.remove_order(c);
    check(level.empty() && level.total_volume() == 0 && level.order_count() == 0, "remove tail");
    check(level.verify(), "verify after drain");
    pool.deallocate(a);
    pool.deallocate(c);
    tracker.clear();

    // --- partial fill then full fill ----------------------------------------
    Order* d = make(pool, 10, 10'000, 500, Side::SELL);
    Order* e = make(pool, 11, 10'000, 250, Side::SELL);
    level.append_order(d);
    level.append_order(e);
    level.reduce_order(d, 200);
    check(d->quantity == 300 && level.total_volume() == 550, "partial fill");
    check(level.verify(), "verify after partial fill");

    Order* filled = level.pop_front();
    check(filled == d && level.total_volume() == 250 && level.order_count() == 1, "pop_front");
    check(level.front() == e && e->prev == nullptr, "head after pop");
    check(filled->prev == nullptr && filled->next == nullptr, "popped order detached");
    pool.deallocate(filled);

    // --- duplicate id is rejected -------------------------------------------
    check(tracker.track(99, e), "track fresh id");
    check(!tracker.track(99, e), "duplicate id rejected");
    check(tracker.untrack(12345) == nullptr, "untrack unknown id");

    level.remove_order(e);
    pool.deallocate(e);
    tracker.clear();

    // --- levels are pinned: moving one would dangle every back-pointer -----
    static_assert(!std::is_move_constructible_v<PriceLevel>, "PriceLevel must be pinned");
    static_assert(!std::is_move_assignable_v<PriceLevel>,    "PriceLevel must be pinned");
    static_assert(!std::is_copy_constructible_v<PriceLevel>, "PriceLevel must be pinned");

    // --- churn: append/cancel 100k times, aggregates must not drift ----------
    {
        PriceLevel churn(777);
        std::vector<Order*> live;
        live.reserve(2048);
        constexpr int kIters = 100'000;

        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) {
            Order* o = make(pool, static_cast<OrderId>(i), 777,
                            static_cast<Quantity>(1 + (i % 97)), Side::BUY);
            churn.append_order(o);
            live.push_back(o);
            if (live.size() >= 2048) {                 // cancel from the middle
                Order* victim = live[live.size() / 2];
                churn.remove_order(victim);
                pool.deallocate(victim);
                live.erase(live.begin() + static_cast<long>(live.size() / 2));
            }
        }
        auto t1 = std::chrono::steady_clock::now();

        check(churn.verify(), "verify after churn");
        check(churn.order_count() == live.size(), "churn count");

        std::uint64_t expected = 0;
        for (const Order* o : live) expected += o->quantity;
        check(churn.total_volume() == expected, "churn volume");

        while (Order* o = churn.pop_front()) pool.deallocate(o);
        check(churn.empty() && churn.verify() && pool.in_use() == 0, "churn drained");

        std::printf("append+cancel churn: %.2f ns/iter\n",
                    std::chrono::duration<double, std::nano>(t1 - t0).count() / kIters);
    }

    if (failures == 0) { std::puts("ALL TESTS PASSED"); return 0; }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
