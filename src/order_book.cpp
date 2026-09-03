#include "lob/order_book.hpp"

#include <cassert>

namespace lob {

namespace {
// One aggressive order can fill against many makers. The buffer is reserved
// once and never shrunk, so it self-tunes to the worst sweep the session has
// seen; only a sweep deeper than anything before it can reallocate.
constexpr std::size_t kTradeBufferReserve = 4096;
} // namespace

OrderBook::OrderBook(std::size_t capacity)
    : pool_(capacity),
      tracker_(capacity),
      bids_(std::make_unique<LevelArray>()),
      asks_(std::make_unique<LevelArray>()),
      trades_(),
      report_() {
    // Each level's price is its own index. Stamping it once here keeps
    // PriceLevel self-describing, so Order::level->price() still answers
    // without the book having to work out which array the level came from.
    for (Price tick = 0; tick < MAX_PRICE_TICKS; ++tick) {
        (*bids_)[tick].set_price(tick);
        (*asks_)[tick].set_price(tick);
    }
    pool_.prefault();
    trades_.reserve(kTradeBufferReserve);
}

// ---------------------------------------------------------------------------
// Best-price repair
//
// Called only when the touch level empties. The level-count check first is
// what keeps this cheap: a side that has just been fully drained jumps
// straight to its sentinel instead of scanning every remaining tick to the
// end of the array. With levels still present, the scan is bounded by the gap
// to the next populated price, which in a real book is a handful of ticks.
// ---------------------------------------------------------------------------
void OrderBook::repair_best_bid() noexcept {
    if (bid_levels_ == 0) {
        best_bid_ = 0;
        return;
    }
    const LevelArray& bids = *bids_;
    while (best_bid_ > 0 && bids[best_bid_].empty()) {
        --best_bid_;
    }
}

void OrderBook::repair_best_ask() noexcept {
    if (ask_levels_ == 0) {
        best_ask_ = MAX_PRICE_TICKS;
        return;
    }
    const LevelArray& asks = *asks_;
    while (best_ask_ < MAX_PRICE_TICKS && asks[best_ask_].empty()) {
        ++best_ask_;
    }
}

// ---------------------------------------------------------------------------
// Matching
//
// Walks the opposing side from the touch outwards while the aggressor's limit
// still crosses, and inside each level walks the intrusive queue front to
// back -- price priority across levels, time priority within one.
//
// Every fill is at the MAKER's price, so a sweep of three levels produces
// three trades at three prices, which is what an exchange would print.
//
// The empty-side sentinels make the loop condition self-guarding: with no
// asks, best_ask_ == MAX_PRICE_TICKS fails `<= limit` for every legal limit;
// with no bids, best_bid_ == 0 fails `>= limit` for every legal limit.
// ---------------------------------------------------------------------------
template <Side TakerSide>
Quantity OrderBook::match(OrderId taker_id, Quantity qty, Price limit) noexcept {
    constexpr bool kBuy = (TakerSide == Side::BUY);

    LevelArray& book = kBuy ? *asks_ : *bids_;
    Quantity    remaining = qty;

    for (;;) {
        if (remaining == 0) break;

        // Cross test against the cached touch price -- no lookup, no compare
        // chain, just the two integers we already hold.
        if constexpr (kBuy) {
            if (best_ask_ > limit) break;
        } else {
            if (best_bid_ < limit) break;
        }

        const Price trade_price = kBuy ? best_ask_ : best_bid_;
        PriceLevel& level       = book[trade_price];
        assert(!level.empty() && "best price points at an empty level");

        // Drain this level, oldest maker first.
        while (remaining > 0) {
            Order* const maker = level.front();
            if (maker == nullptr) {
                break;  // level exhausted, aggressor still has quantity
            }

            const Quantity fill = (remaining < maker->quantity) ? remaining : maker->quantity;
            trades_.push_back(Trade{.maker_order_id = maker->order_id,
                                    .taker_order_id = taker_id,
                                    .price          = trade_price,
                                    .quantity       = fill,
                                    .aggressor_side = TakerSide});
            remaining -= fill;

            if (fill == maker->quantity) {
                // Maker fully filled: it leaves the book entirely.
                // remove_order() splices it out and fixes the level
                // aggregates; untrack() drops the id so a late cancel cannot
                // reach freed memory; deallocate() returns the slot to the
                // pool -- which is why a sweep can never exhaust the pool.
                level.remove_order(maker);
                tracker_.untrack(maker->order_id);
                pool_.deallocate(maker);
            } else {
                // Maker partially filled: it stays at the FRONT of the queue,
                // keeping its time priority, with its quantity reduced. This
                // also decrements the level's total_volume in the same step,
                // so the aggregate can never drift.
                level.reduce_order(maker, fill);
                assert(remaining == 0);
            }
        }

        if (!level.empty()) {
            break;  // only reachable once remaining == 0
        }

        // Level drained. Nothing to erase -- the slot stays resident, it is
        // simply empty now. Step past it to the next populated price.
        if constexpr (kBuy) {
            --ask_levels_;
            if (ask_levels_ == 0) {
                best_ask_ = MAX_PRICE_TICKS;
            } else {
                // ask_levels_ > 0 guarantees a populated tick above, so the
                // scan cannot run off the end -- asserted, not branch-checked.
                do {
                    ++best_ask_;
                    assert(best_ask_ < MAX_PRICE_TICKS && "ask_levels_ disagrees with the array");
                } while (book[best_ask_].empty());
            }
        } else {
            --bid_levels_;
            if (bid_levels_ == 0) {
                best_bid_ = 0;
            } else {
                do {
                    --best_bid_;
                    assert(best_bid_ > 0 && "bid_levels_ disagrees with the array");
                } while (book[best_bid_].empty());
            }
        }
    }

    return remaining;
}

// ---------------------------------------------------------------------------
// Resting the residual
// ---------------------------------------------------------------------------
template <Side OrderSide>
bool OrderBook::rest(OrderId order_id, Price price, Quantity qty) noexcept {
    constexpr bool kBuy = (OrderSide == Side::BUY);

    Order* const order = pool_.allocate();
    if (order == nullptr) [[unlikely]] {
        return false;  // pool exhausted -- caller reports the residual dropped
    }

    *order = Order{.order_id  = order_id,
                   .price     = price,
                   .timestamp = ++sequence_,
                   .prev      = nullptr,
                   .next      = nullptr,
                   .level     = nullptr,
                   .quantity  = qty,
                   .side      = OrderSide};

    // The whole point of the refactor: the level lookup is an array index.
    PriceLevel& level = kBuy ? (*bids_)[price] : (*asks_)[price];

    if (level.empty()) {
        if constexpr (kBuy) { ++bid_levels_; } else { ++ask_levels_; }
    }
    level.append_order(order);

    // A resting order can only ever improve the touch: anything that would
    // have crossed was already matched away above.
    if constexpr (kBuy) {
        if (price > best_bid_) best_bid_ = price;
    } else {
        if (price < best_ask_) best_ask_ = price;
    }

    const bool tracked = tracker_.track(order_id, order);
    assert(tracked && "duplicate id survived the pre-match check");
    (void)tracked;

    return true;
}

// ---------------------------------------------------------------------------
// add_order
// ---------------------------------------------------------------------------
void OrderBook::add_order(OrderId order_id, Side side, Price price, Quantity qty) noexcept {
    trades_.clear();
    report_ = ExecutionReport{.order_id = order_id, .filled = 0, .resting = 0,
                              .status = OrderStatus::Ok};

    if (qty == 0) [[unlikely]] {
        report_.status = OrderStatus::RejectedZeroQuantity;
        return;
    }
    if (price == 0) [[unlikely]] {
        // Tick 0 is the "no bid" sentinel and can never hold an order.
        report_.status = OrderStatus::RejectedZeroPrice;
        return;
    }
    if (price >= MAX_PRICE_TICKS) [[unlikely]] {
        // The tick grid is fixed. Off the grid is a reject, not an array
        // overrun -- this check is the only thing standing between a bad
        // feed and a 6 MB out-of-bounds write.
        report_.status = OrderStatus::RejectedPriceOutOfRange;
        return;
    }
    // Checked BEFORE matching: a duplicate id is a gateway bug, and it must
    // not be allowed to trade before being rejected.
    if (tracker_.contains(order_id)) [[unlikely]] {
        report_.status = OrderStatus::RejectedDuplicateId;
        return;
    }

    const Quantity remaining = (side == Side::BUY)
                                   ? match<Side::BUY>(order_id, qty, price)
                                   : match<Side::SELL>(order_id, qty, price);

    report_.filled = qty - remaining;

    if (remaining > 0) {
        const bool rested = (side == Side::BUY) ? rest<Side::BUY>(order_id, price, remaining)
                                                : rest<Side::SELL>(order_id, price, remaining);
        if (rested) [[likely]] {
            report_.resting = remaining;
        } else {
            // Only reachable when nothing traded: every complete fill above
            // returned a slot to the pool, so an order that traded at all has
            // freed at least one slot for its own residual. Any fills that did
            // happen are final and stay in last_trades() regardless.
            report_.status = OrderStatus::RejectedPoolExhausted;
        }
    }
}

// ---------------------------------------------------------------------------
// cancel_order
// ---------------------------------------------------------------------------
void OrderBook::cancel_order(OrderId order_id) noexcept {
    trades_.clear();
    report_ = ExecutionReport{.order_id = order_id, .filled = 0, .resting = 0,
                              .status = OrderStatus::Ok};

    // O(1). Returns nullptr for an id that was already filled or cancelled --
    // a routine race with an incoming aggressor, not an error.
    Order* const order = tracker_.untrack(order_id);
    if (order == nullptr) [[unlikely]] {
        report_.status = OrderStatus::CancelUnknownId;
        return;
    }

    PriceLevel* const level = order->level;
    assert(level != nullptr && "tracked order is not resting on a level");

    const Price price = level->price();
    const Side  side  = order->side;

    report_.resting = order->quantity;  // quantity withdrawn from the book

    // O(1): the intrusive links mean no search through the queue.
    level->remove_order(order);
    pool_.deallocate(order);

    if (!level->empty()) {
        return;  // depth still here, touch price unchanged
    }

    // Level is now empty. The slot stays where it is; only the counters and,
    // if this was the touch, the cached best price need fixing.
    if (side == Side::BUY) {
        --bid_levels_;
        if (price == best_bid_) repair_best_bid();
    } else {
        --ask_levels_;
        if (price == best_ask_) repair_best_ask();
    }
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------
void OrderBook::clear() noexcept {
    for (Price tick = 0; tick < MAX_PRICE_TICKS; ++tick) {
        PriceLevel& bid = (*bids_)[tick];
        while (Order* o = bid.pop_front()) pool_.deallocate(o);
        PriceLevel& ask = (*asks_)[tick];
        while (Order* o = ask.pop_front()) pool_.deallocate(o);
    }
    best_bid_   = 0;
    best_ask_   = MAX_PRICE_TICKS;
    bid_levels_ = 0;
    ask_levels_ = 0;
    tracker_.clear();
    trades_.clear();
    report_ = ExecutionReport{};
}

bool OrderBook::verify() const noexcept {
    std::size_t counted     = 0;
    std::size_t bid_levels  = 0;
    std::size_t ask_levels  = 0;
    Price       highest_bid = 0;
    Price       lowest_ask  = MAX_PRICE_TICKS;

    const auto check_level = [&](const PriceLevel& level, Price tick, Side side) noexcept {
        if (level.price() != tick)     return false;  // index vs payload
        if (level.empty())             return true;   // empty slots are normal now
        if (!level.verify())           return false;  // links / aggregates

        for (const Order* o = level.front(); o != nullptr; o = o->next) {
            if (o->side != side)                 return false;
            if (o->price != tick)                return false;
            if (tracker_.find(o->order_id) != o) return false;  // index disagrees
            ++counted;
        }
        return true;
    };

    // Full sweep of both arrays. Deliberately unbounded: a scan limited to the
    // known range could not detect resting depth sitting outside best_bid_ /
    // best_ask_, which is exactly the bug class this refactor introduces.
    for (Price tick = 0; tick < MAX_PRICE_TICKS; ++tick) {
        const PriceLevel& bid = (*bids_)[tick];
        const PriceLevel& ask = (*asks_)[tick];

        if (!check_level(bid, tick, Side::BUY))  return false;
        if (!check_level(ask, tick, Side::SELL)) return false;

        if (!bid.empty()) {
            ++bid_levels;
            highest_bid = tick;                       // ascending sweep: last wins
            if (tick == 0) return false;              // tick 0 is the sentinel
        }
        if (!ask.empty()) {
            ++ask_levels;
            if (lowest_ask == MAX_PRICE_TICKS) lowest_ask = tick;
            if (tick == 0) return false;
        }
    }

    // Cached top of book must equal what the sweep actually found.
    if (best_bid_ != highest_bid) return false;
    if (best_ask_ != lowest_ask)  return false;

    // Level counters must match reality, or repair_best_*() will short-circuit
    // to a sentinel while depth is still resting.
    if (bid_levels_ != bid_levels) return false;
    if (ask_levels_ != ask_levels) return false;

    // The index must hold exactly the resting orders -- no leaks, no ghosts.
    if (counted != tracker_.size()) return false;

    // A book must never be crossed once a call returns.
    if (best_bid_ != 0 && best_ask_ != MAX_PRICE_TICKS && best_bid_ >= best_ask_) {
        return false;
    }

    return true;
}

} // namespace lob
