// Correctness tests for OrderBook: crossing, partial fills on both sides,
// FIFO priority, level cleanup, cancels, rejects, and a randomised fuzz run
// that asserts the full book invariant after every single message.
#include "lob/order_book.hpp"

#include <chrono>
#include <cstdio>
#include <random>
#include <vector>

using namespace lob;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}

// Every message must leave the book fully consistent.
void step(OrderBook& b, const char* what) {
    check(b.verify(), what);
    check(b.pool().in_use() == b.live_order_count(), "pool accounting matches index");
}

} // namespace

int main() {
    // -----------------------------------------------------------------------
    // 1. Resting, top of book, spread
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        check(b.best_bid() == nullptr && b.best_ask() == nullptr, "empty book");
        check(!b.has_both_sides(), "empty has no sides");

        b.add_order(1, Side::BUY, 100, 10);
        check(b.last_report().ok() && b.last_report().resting == 10, "resting buy");
        check(b.last_report().filled == 0 && b.last_trades().empty(), "no trade");

        b.add_order(2, Side::BUY, 101, 20);
        b.add_order(3, Side::SELL, 105, 30);
        b.add_order(4, Side::SELL, 103, 40);
        step(b, "verify after resting");

        check(b.best_bid() != nullptr && b.best_bid()->price() == 101, "best bid = highest");
        check(b.best_ask() != nullptr && b.best_ask()->price() == 103, "best ask = lowest");
        check(b.spread() == 2, "spread");
        check(b.bid_level_count() == 2 && b.ask_level_count() == 2, "level counts");
        check(b.volume_at(Side::BUY, 101) == 20, "volume_at bid");
        check(b.volume_at(Side::SELL, 105) == 30, "volume_at ask");
        check(b.volume_at(Side::SELL, 999) == 0, "volume_at empty price");
        check(b.live_order_count() == 4, "live count");
    }

    // -----------------------------------------------------------------------
    // 2. FIFO time priority within a level
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        b.add_order(1, Side::BUY, 100, 10);   // older
        b.add_order(2, Side::BUY, 100, 10);   // newer
        check(b.volume_at(Side::BUY, 100) == 20, "two orders one level");

        b.add_order(3, Side::SELL, 100, 10);  // takes exactly one
        check(b.last_trades().size() == 1, "one trade");
        check(b.last_trades()[0].maker_order_id == 1, "oldest maker filled first");
        check(b.last_trades()[0].taker_order_id == 3, "taker id");
        check(b.last_trades()[0].price == 100 && b.last_trades()[0].quantity == 10, "trade terms");
        check(b.last_trades()[0].aggressor_side == Side::SELL, "aggressor side");
        check(b.last_report().filled == 10 && b.last_report().resting == 0, "taker fully filled");
        check(b.find_order(1) == nullptr, "filled maker untracked");
        check(b.find_order(2) != nullptr, "younger maker still live");
        check(b.volume_at(Side::BUY, 100) == 10, "level volume after fill");
        step(b, "verify after FIFO fill");
    }

    // -----------------------------------------------------------------------
    // 3. Maker partially filled: keeps front-of-queue time priority
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        b.add_order(1, Side::SELL, 100, 100);   // big resting maker
        b.add_order(2, Side::SELL, 100, 50);    // behind it
        b.add_order(3, Side::BUY, 100, 30);     // nibbles the front

        check(b.last_trades().size() == 1 && b.last_trades()[0].maker_order_id == 1, "hits front");
        check(b.last_trades()[0].quantity == 30, "partial fill size");

        const Order* maker = b.find_order(1);
        check(maker != nullptr && maker->quantity == 70, "maker quantity reduced");
        check(b.best_ask()->front() == maker, "partially filled maker keeps priority");
        check(b.volume_at(Side::SELL, 100) == 120, "level volume 70 + 50");
        check(b.last_report().filled == 30 && b.last_report().resting == 0, "taker done");
        check(b.bid_level_count() == 0, "nothing rested on the bid");
        step(b, "verify after partial maker fill");
    }

    // -----------------------------------------------------------------------
    // 4. Taker partially filled: residual rests on the other side
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        b.add_order(1, Side::SELL, 100, 40);
        b.add_order(2, Side::BUY, 100, 100);    // takes 40, rests 60

        check(b.last_report().filled == 40, "filled part");
        check(b.last_report().resting == 60, "rested part");
        check(b.last_report().partially_filled(), "partial flag");
        check(b.ask_level_count() == 0, "ask level erased when emptied");
        check(b.best_bid() != nullptr && b.best_bid()->price() == 100, "residual is the bid");
        check(b.volume_at(Side::BUY, 100) == 60, "residual volume");
        check(b.find_order(2) != nullptr && b.find_order(2)->quantity == 60, "residual tracked");
        check(b.find_order(1) == nullptr, "maker gone");
        step(b, "verify after taker residual");
    }

    // -----------------------------------------------------------------------
    // 5. Multi-level sweep: one trade per level, each at the maker's price
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        b.add_order(1, Side::SELL, 100, 10);
        b.add_order(2, Side::SELL, 101, 10);
        b.add_order(3, Side::SELL, 102, 10);
        b.add_order(4, Side::SELL, 103, 10);   // outside the sweep limit

        b.add_order(9, Side::BUY, 102, 25);    // sweeps 100, 101, then 5 @ 102

        const auto& t = b.last_trades();
        check(t.size() == 3, "three fills");
        check(t[0].price == 100 && t[0].quantity == 10, "level 1 at maker price");
        check(t[1].price == 101 && t[1].quantity == 10, "level 2 at maker price");
        check(t[2].price == 102 && t[2].quantity == 5,  "level 3 partial");
        check(b.last_report().filled == 25 && b.last_report().resting == 0, "sweep filled");

        check(b.ask_level_count() == 2, "emptied levels erased, 102 and 103 remain");
        check(b.volume_at(Side::SELL, 100) == 0, "100 gone");
        check(b.volume_at(Side::SELL, 101) == 0, "101 gone");
        check(b.volume_at(Side::SELL, 102) == 5, "102 reduced");
        check(b.volume_at(Side::SELL, 103) == 10, "103 untouched");
        check(b.best_ask()->price() == 102, "new best ask");
        step(b, "verify after sweep");

        // Sweep beyond the book: residual rests, book must not be crossed.
        b.add_order(10, Side::BUY, 200, 100);
        check(b.last_report().filled == 15, "swept the rest");
        check(b.last_report().resting == 85, "residual rests at 200");
        check(b.ask_level_count() == 0, "asks exhausted");
        check(b.best_bid()->price() == 200, "bid at 200");
        step(b, "verify after full sweep");
    }

    // -----------------------------------------------------------------------
    // 6. Cancels
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        b.add_order(1, Side::BUY, 100, 10);
        b.add_order(2, Side::BUY, 100, 20);
        b.add_order(3, Side::BUY, 100, 30);
        b.add_order(4, Side::BUY, 99, 40);

        b.cancel_order(2);                       // middle of the queue
        check(b.last_report().ok() && b.last_report().resting == 20, "cancel reports quantity");
        check(b.find_order(2) == nullptr, "cancelled order untracked");
        check(b.volume_at(Side::BUY, 100) == 40, "level volume after cancel");
        check(b.best_bid()->front()->order_id == 1, "head unchanged");
        check(b.best_bid()->back()->order_id == 3, "tail unchanged");
        step(b, "verify after middle cancel");

        b.cancel_order(1);                       // head
        b.cancel_order(3);                       // last one at this level
        check(b.bid_level_count() == 1, "empty level erased on cancel");
        check(b.best_bid()->price() == 99, "best bid falls to 99");
        step(b, "verify after level-emptying cancel");

        b.cancel_order(4);
        check(b.bid_level_count() == 0 && b.best_bid() == nullptr, "book empty");
        check(b.pool().in_use() == 0, "all orders returned to pool");

        b.cancel_order(999);
        check(b.last_report().status == OrderStatus::CancelUnknownId, "unknown cancel");
        b.add_order(5, Side::BUY, 100, 10);
        b.add_order(6, Side::SELL, 100, 10);     // fills 5 completely
        b.cancel_order(5);
        check(b.last_report().status == OrderStatus::CancelUnknownId, "cancel of filled order");
        step(b, "verify after late cancel");
    }

    // -----------------------------------------------------------------------
    // 7. Rejects
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        b.add_order(1, Side::BUY, 100, 10);

        b.add_order(1, Side::SELL, 100, 5);      // duplicate id would have traded
        check(b.last_report().status == OrderStatus::RejectedDuplicateId, "duplicate id");
        check(b.last_trades().empty(), "duplicate rejected before matching");
        check(b.volume_at(Side::BUY, 100) == 10, "book untouched by reject");

        b.add_order(2, Side::BUY, 100, 0);
        check(b.last_report().status == OrderStatus::RejectedZeroQuantity, "zero qty");
        b.add_order(3, Side::BUY, 0, 10);
        check(b.last_report().status == OrderStatus::RejectedZeroPrice, "zero price");
        check(b.live_order_count() == 1, "rejects left no residue");
        step(b, "verify after rejects");
    }

    // -----------------------------------------------------------------------
    // 8. Pool exhaustion. Note a SWEEP can never exhaust the pool: every
    //    complete fill hands a slot back before the residual asks for one.
    //    The only way to run dry is a non-crossing order with no slots left.
    // -----------------------------------------------------------------------
    {
        OrderBook b(2);
        b.add_order(1, Side::BUY, 100, 10);
        b.add_order(2, Side::BUY, 99, 10);
        check(b.pool().empty(), "pool drained");

        b.add_order(3, Side::BUY, 98, 10);       // nothing to cross, no slot
        check(b.last_report().status == OrderStatus::RejectedPoolExhausted, "exhausted");
        check(b.last_report().filled == 0 && b.last_report().resting == 0, "nothing done");
        check(b.live_order_count() == 2, "book unchanged by reject");
        step(b, "verify after exhaustion");

        // Same book, but now the incoming order crosses: the fill frees a slot
        // and the residual rests, even though the pool was empty on entry.
        b.add_order(4, Side::SELL, 100, 30);
        check(b.last_report().filled == 10, "crossed and filled");
        check(b.last_report().resting == 20, "residual rested on a freed slot");
        check(b.last_report().status == OrderStatus::Ok, "not rejected");
        step(b, "verify after self-freeing sweep");
    }

    // 9. clear()
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);
        for (OrderId i = 1; i <= 50; ++i) {
            b.add_order(i, Side::BUY, 100 + (i % 5), 10);
        }
        b.clear();
        check(b.live_order_count() == 0 && b.pool().in_use() == 0, "clear returns everything");
        check(b.bid_level_count() == 0 && b.ask_level_count() == 0, "clear empties both sides");
        step(b, "verify after clear");
    }

    // -----------------------------------------------------------------------
    // 10. Fuzz: random adds and cancels, full invariant check after each
    // -----------------------------------------------------------------------
    {
        OrderBook b(200'000);
        std::mt19937_64 rng(0xC0FFEE);
        std::vector<OrderId> live;
        live.reserve(4096);
        OrderId next_id = 1;

        std::uint64_t traded = 0;
        int msgs = 0;

        for (int i = 0; i < 20'000; ++i) {
            const bool do_cancel = !live.empty() && (rng() % 100) < 35;
            if (do_cancel) {
                const std::size_t k = rng() % live.size();
                b.cancel_order(live[k]);
                live[k] = live.back();
                live.pop_back();
            } else {
                const Side     side  = (rng() % 2) ? Side::BUY : Side::SELL;
                const Price    price = 990 + (rng() % 21);          // 990..1010
                const Quantity qty   = 1 + static_cast<Quantity>(rng() % 100);
                const OrderId  id    = next_id++;
                b.add_order(id, side, price, qty);
                for (const Trade& t : b.last_trades()) traded += t.quantity;
                if (b.last_report().resting > 0) live.push_back(id);
                // Makers we filled may still sit in `live`; cancelling them
                // later is a no-op the book must tolerate, which is the point.
            }
            ++msgs;
            // verify() now sweeps both 100k-entry arrays end to end, so it
            // costs ~6 MB of traffic per call. Run it on every message while
            // the book is still small and bugs surface fastest, then thin out
            // to keep the suite quick without giving up coverage.
            if (i < 2000 || (i % 64) == 0) {
                if (!b.verify()) {
                    std::printf("FAIL: fuzz invariant at msg %d\n", i);
                    ++failures;
                    break;
                }
            }
        }
        check(b.verify(), "fuzz final invariant");

        check(b.pool().in_use() == b.live_order_count(), "fuzz pool accounting");
        std::printf("fuzz: %d messages, %llu units traded, %zu levels live\n",
                    msgs, static_cast<unsigned long long>(traded),
                    b.bid_level_count() + b.ask_level_count());

        b.clear();
        check(b.pool().in_use() == 0, "fuzz cleanup");
    }

    // -----------------------------------------------------------------------
    // 11. Throughput against a book with realistic depth.
    //     Passive orders are priced away from the mid so depth actually
    //     accumulates; 1 message in 12 is aggressive and crosses. Measuring
    //     against uniform two-sided flow would keep the book near-empty and
    //     flatter the numbers.
    // -----------------------------------------------------------------------
    {
        OrderBook b(1'000'000);
        std::mt19937_64 rng(42);
        std::vector<OrderId> live;
        live.reserve(1 << 18);
        OrderId next_id = 1;
        constexpr Price kMid  = 10'000;
        constexpr int   kMsgs = 500'000;

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kMsgs; ++i) {
            const auto roll = rng() % 100;
            if (!live.empty() && roll < 40) {                    // 40% cancel
                const std::size_t k = rng() % live.size();
                b.cancel_order(live[k]);
                live[k] = live.back();
                live.pop_back();
            } else if (roll < 48) {                              // 8% aggressive
                const Side side = (rng() % 2) ? Side::BUY : Side::SELL;
                const Price px  = (side == Side::BUY) ? kMid + 25 : kMid - 25;
                b.add_order(next_id, side, px, 1 + static_cast<Quantity>(rng() % 400));
                if (b.last_report().resting > 0) live.push_back(next_id);
                ++next_id;
            } else {                                             // 52% passive
                const Side  side   = (rng() % 2) ? Side::BUY : Side::SELL;
                const Price offset = 1 + (rng() % 50);
                const Price px     = (side == Side::BUY) ? kMid - offset : kMid + offset;
                b.add_order(next_id, side, px, 1 + static_cast<Quantity>(rng() % 100));
                if (b.last_report().resting > 0) live.push_back(next_id);
                ++next_id;
            }
        }
        const auto t1 = std::chrono::steady_clock::now();
        const double ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / kMsgs;
        std::printf("throughput: %.0f ns/message (%.2f M msg/s) | %zu resting across %zu levels\n",
                    ns, 1000.0 / ns, b.live_order_count(),
                    b.bid_level_count() + b.ask_level_count());
        check(b.verify(), "verify after throughput run");
        check(b.live_order_count() > 1000, "book actually had depth");
    }

    // -----------------------------------------------------------------------
    // 12. Flat-array specifics: tick bounds, sentinels, best-price repair
    // -----------------------------------------------------------------------
    {
        OrderBook b(4096);

        // Sentinels on an empty book.
        check(b.best_bid_tick() == 0, "empty bid sentinel");
        check(b.best_ask_tick() == MAX_PRICE_TICKS, "empty ask sentinel");
        check(b.spread() == 0 && !b.has_both_sides(), "no spread when empty");

        // Off the tick grid is a reject, not an out-of-bounds write.
        b.add_order(1, Side::BUY, MAX_PRICE_TICKS, 10);
        check(b.last_report().status == OrderStatus::RejectedPriceOutOfRange, "price == MAX");
        b.add_order(2, Side::BUY, MAX_PRICE_TICKS + 5000, 10);
        check(b.last_report().status == OrderStatus::RejectedPriceOutOfRange, "price > MAX");
        check(b.live_order_count() == 0, "rejects left nothing behind");

        // The highest legal tick must work.
        b.add_order(3, Side::SELL, MAX_PRICE_TICKS - 1, 10);
        check(b.last_report().ok() && b.best_ask_tick() == MAX_PRICE_TICKS - 1, "top tick");
        b.cancel_order(3);
        check(b.best_ask_tick() == MAX_PRICE_TICKS, "sentinel restored after drain");
        check(b.ask_level_count() == 0, "no ask levels");
        step(b, "verify after bounds");
    }
    {
        // Best price must skip over empty ticks, not stop at the first gap.
        OrderBook b(4096);
        b.add_order(1, Side::BUY, 5000, 10);
        b.add_order(2, Side::BUY, 5001, 10);
        b.add_order(3, Side::BUY, 5900, 10);   // 899 empty ticks below it
        check(b.best_bid_tick() == 5900, "best bid at top");

        b.cancel_order(3);
        check(b.best_bid_tick() == 5001, "scanned down across the gap");
        check(b.bid_level_count() == 2, "two levels left");
        step(b, "verify after gap scan");

        b.cancel_order(2);
        check(b.best_bid_tick() == 5000, "adjacent step down");
        b.cancel_order(1);
        check(b.best_bid_tick() == 0, "sentinel after last bid");
        check(b.bid_level_count() == 0, "no bid levels");
        step(b, "verify after full bid drain");

        // Same on the ask side, and across a much wider gap.
        b.add_order(10, Side::SELL, 200, 10);
        b.add_order(11, Side::SELL, 90'000, 10);
        check(b.best_ask_tick() == 200, "best ask at bottom");
        b.cancel_order(10);
        check(b.best_ask_tick() == 90'000, "scanned up across a wide gap");
        step(b, "verify after wide ask gap");
        b.cancel_order(11);
        check(b.best_ask_tick() == MAX_PRICE_TICKS, "ask sentinel restored");
    }
    {
        // Matching across a gap must leave best_ask correct, including the
        // case where the sweep empties the side entirely.
        OrderBook b(4096);
        b.add_order(1, Side::SELL, 100, 10);
        b.add_order(2, Side::SELL, 700, 10);   // 599 empty ticks between
        b.add_order(3, Side::SELL, 701, 10);

        b.add_order(9, Side::BUY, 700, 15);    // takes 100 fully, 5 from 700
        check(b.last_trades().size() == 2, "two fills across the gap");
        check(b.last_trades()[0].price == 100 && b.last_trades()[1].price == 700, "gap skipped");
        check(b.best_ask_tick() == 700, "best ask advanced across the gap");
        check(b.volume_at(Side::SELL, 700) == 5, "partial level remains");
        check(b.ask_level_count() == 2, "700 and 701 left");
        step(b, "verify after gap sweep");

        b.add_order(10, Side::BUY, 99'999, 100);  // sweeps everything left
        check(b.last_report().filled == 15, "swept the rest");
        check(b.last_report().resting == 85, "residual rests");
        check(b.ask_level_count() == 0, "asks drained");
        check(b.best_ask_tick() == MAX_PRICE_TICKS, "sentinel after sweep drain");
        check(b.best_bid_tick() == 99'999, "residual is the new bid");
        step(b, "verify after drain sweep");
    }
    {
        // A stale best price is the bug class this refactor introduces, so
        // hammer the touch level specifically: repeatedly empty it and refill.
        OrderBook b(65'536);
        std::mt19937_64 rng(0xBEEF);
        OrderId next_id = 1;
        std::vector<OrderId> live;

        for (int round = 0; round < 300; ++round) {
            const Price base = 40'000 + static_cast<Price>(rng() % 200);
            for (int k = 0; k < 8; ++k) {
                const Side  side = (k % 2) ? Side::BUY : Side::SELL;
                const Price px   = (side == Side::BUY) ? base - 1 - (rng() % 5)
                                                       : base + 1 + (rng() % 5);
                b.add_order(next_id, side, px, 1 + static_cast<Quantity>(rng() % 20));
                if (b.last_report().resting > 0) live.push_back(next_id);
                ++next_id;
            }
            // Cancel roughly half, preferring whatever is at the touch.
            for (int k = 0; k < 4 && !live.empty(); ++k) {
                const std::size_t idx = rng() % live.size();
                b.cancel_order(live[idx]);
                live[idx] = live.back();
                live.pop_back();
            }
            if (!b.verify()) { std::printf("FAIL: touch churn round %d\n", round); ++failures; break; }
        }
        check(b.verify(), "touch churn final");
        b.clear();
        check(b.pool().in_use() == 0 && b.best_bid_tick() == 0 &&
              b.best_ask_tick() == MAX_PRICE_TICKS, "clear resets sentinels");
        check(b.bid_level_count() == 0 && b.ask_level_count() == 0, "clear resets counters");
        step(b, "verify after clear");
    }

    if (failures == 0) { std::puts("ALL TESTS PASSED"); return 0; }
    std::printf("%d FAILURES\n", failures);
    return 1;
}
