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
      bids_(),
      asks_(),
      trades_(),
      report_() {
    pool_.prefault();
    trades_.reserve(kTradeBufferReserve);
}

// ---------------------------------------------------------------------------
// Matching
//
// Walks the opposing book from the top down while the aggressor limit still
// crosses, and inside each level walks the intrusive queue front to back --
// price priority across levels, time priority within one.
//
// Every fill is at the MAKER price, so a sweep of three levels produces three
// trades at three prices, which is what an exchange would print.
// ---------------------------------------------------------------------------
template <typename LevelMap, typename Crosses>
Quantity OrderBook::match(LevelMap& book, OrderId taker_id, Side taker_side,
                          Quantity qty, Crosses crosses) noexcept {
    Quantity remaining = qty;
    auto     it        = book.begin();

    while (remaining > 0 && it != book.end() && crosses(it->first)) {
        PriceLevel& level       = it->second;
        const Price trade_price = it->first;

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
                                    .aggressor_side = taker_side});
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
                // also decrements the level total_volume in the same step, so
                // the aggregate can never drift.
                level.reduce_order(maker, fill);
                assert(remaining == 0);
            }
        }

        if (level.empty()) {
            // Erase the level as soon as it empties. Without this the tree
            // keeps a node for every price ever traded and grows all session.
            // erase() returns the next iterator, so the walk continues safely.
            it = book.erase(it);
        } else {
            ++it;  // only reachable once remaining == 0; loop exits next test
        }
    }

    return remaining;
}

// ---------------------------------------------------------------------------
// Resting the residual
// ---------------------------------------------------------------------------
template <typename LevelMap>
bool OrderBook::rest(LevelMap& book, OrderId order_id, Side side,
                     Price price, Quantity qty) noexcept {
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
                   .side      = side};

    // try_emplace constructs the level in place -- PriceLevel is pinned
    // (non-movable), which std::map is fine with because it never relocates a
    // node once built.
    const auto [it, inserted] = book.try_emplace(price, price);
    (void)inserted;

    it->second.append_order(order);

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
        report_.status = OrderStatus::RejectedZeroPrice;
        return;
    }
    // Checked BEFORE matching: a duplicate id is a gateway bug, and it must
    // not be allowed to trade before being rejected.
    if (tracker_.contains(order_id)) [[unlikely]] {
        report_.status = OrderStatus::RejectedDuplicateId;
        return;
    }

    // A BUY crosses an ask priced at or below its limit; a SELL crosses a bid
    // priced at or above its limit. The maps are already ordered best-first,
    // so the first level that fails this test ends the sweep.
    const Quantity remaining =
        (side == Side::BUY)
            ? match(asks_, order_id, Side::BUY, qty,
                    [price](Price ask) noexcept { return ask <= price; })
            : match(bids_, order_id, Side::SELL, qty,
                    [price](Price bid) noexcept { return bid >= price; });

    report_.filled = qty - remaining;

    if (remaining > 0) {
        const bool rested = (side == Side::BUY)
                                ? rest(bids_, order_id, side, price, remaining)
                                : rest(asks_, order_id, side, price, remaining);
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

    // Only if that was the last order here do we pay a tree erase. Note that
    // the pointer `level` is dead after this block.
    if (level->empty()) {
        if (side == Side::BUY) {
            bids_.erase(price);
        } else {
            asks_.erase(price);
        }
    }
}

// ---------------------------------------------------------------------------
// Introspection
// ---------------------------------------------------------------------------
Quantity OrderBook::volume_at(Side side, Price price) const noexcept {
    if (side == Side::BUY) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? Quantity{0} : it->second.total_volume();
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? Quantity{0} : it->second.total_volume();
}

void OrderBook::clear() noexcept {
    for (auto& entry : bids_) {
        while (Order* o = entry.second.pop_front()) {
            pool_.deallocate(o);
        }
    }
    for (auto& entry : asks_) {
        while (Order* o = entry.second.pop_front()) {
            pool_.deallocate(o);
        }
    }
    bids_.clear();
    asks_.clear();
    tracker_.clear();
    trades_.clear();
    report_ = ExecutionReport{};
}

bool OrderBook::verify() const noexcept {
    std::size_t counted = 0;

    const auto check_side = [&](const auto& book, Side side) noexcept {
        for (const auto& entry : book) {
            const PriceLevel& level = entry.second;
            if (entry.first != level.price()) return false;  // key vs payload
            if (level.empty())                return false;  // empty level not erased
            if (!level.verify())              return false;  // links / aggregates

            for (const Order* o = level.front(); o != nullptr; o = o->next) {
                if (o->side != side)                 return false;
                if (o->price != level.price())       return false;
                if (tracker_.find(o->order_id) != o) return false;  // index disagrees
                ++counted;
            }
        }
        return true;
    };

    if (!check_side(bids_, Side::BUY))  return false;
    if (!check_side(asks_, Side::SELL)) return false;

    // The index must hold exactly the resting orders -- no leaks, no ghosts.
    if (counted != tracker_.size()) return false;

    // A book must never be crossed once a call returns: any crossing would
    // have been matched away.
    if (has_both_sides() && bids_.begin()->first >= asks_.begin()->first) {
        return false;
    }

    return true;
}

} // namespace lob
