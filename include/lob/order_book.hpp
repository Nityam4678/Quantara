#pragma once

#include "lob/order.hpp"
#include "lob/order_map.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// Trade -- one fill between an incoming aggressor and one resting maker.
// Executed at the MAKER's price (standard price-time priority: the passive
// side set the terms, the aggressor accepted them), so a marketable order
// that sweeps several levels produces one Trade per level touched, each at
// its own price.
// ---------------------------------------------------------------------------
struct Trade {
    OrderId  maker_order_id;
    OrderId  taker_order_id;
    Price    price;          // the resting order's price
    Quantity quantity;
    Side     aggressor_side; // side of the incoming order
};

enum class OrderStatus : std::uint8_t {
    Ok = 0,             // add: accepted (filled and/or resting). cancel: removed.
    RejectedDuplicateId,// add: order_id is already live in the book
    RejectedZeroQuantity,
    RejectedZeroPrice,
    RejectedPoolExhausted, // add: no free slot to rest the residual
    CancelUnknownId        // cancel: no live order with that id
};

// Outcome of the last add_order / cancel_order.
struct ExecutionReport {
    OrderId     order_id = 0;
    Quantity    filled   = 0;   // quantity traded against the book
    Quantity    resting  = 0;   // quantity left sitting on the book
    OrderStatus status   = OrderStatus::Ok;

    [[nodiscard]] bool ok()               const noexcept { return status == OrderStatus::Ok; }
    [[nodiscard]] bool fully_filled()     const noexcept { return ok() && resting == 0 && filled > 0; }
    [[nodiscard]] bool partially_filled() const noexcept { return filled > 0 && resting > 0; }
};

// ---------------------------------------------------------------------------
// OrderBook
//
// One instrument. Owns the order pool, the id -> Order* index, and the two
// sides of the book.
//
// Bids are sorted descending and asks ascending, so begin() on either map is
// always the best price and matching walks forward from there. Levels are
// created on demand and ERASED the moment they empty -- a book that ran all
// day would otherwise accumulate a tree node for every price ever touched.
//
// Not thread-safe: one book, one thread, one core. See OrderPool.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    explicit OrderBook(std::size_t capacity = OrderPool::kDefaultCapacity);

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = delete;
    OrderBook& operator=(OrderBook&&)      = delete;

    // ---- the two operations that matter -------------------------------

    // Submit a limit order. Matches against the opposite side as far as the
    // limit allows, then rests whatever is left. Fills land in last_trades();
    // the outcome lands in last_report().
    void add_order(OrderId order_id, Side side, Price price, Quantity qty) noexcept;

    // Pull a resting order: O(1) hash lookup, O(1) intrusive splice, O(log L)
    // only in the rare case where this was the level's last order and the
    // level itself has to be erased from the tree.
    void cancel_order(OrderId order_id) noexcept;

    // ---- results of the last call --------------------------------------

    [[nodiscard]] const std::vector<Trade>& last_trades() const noexcept { return trades_; }
    [[nodiscard]] const ExecutionReport&    last_report() const noexcept { return report_; }

    // ---- top of book ----------------------------------------------------
    // nullptr when that side is empty -- no sentinel price to misread.

    [[nodiscard]] const PriceLevel* best_bid() const noexcept {
        return bids_.empty() ? nullptr : &bids_.begin()->second;
    }
    [[nodiscard]] const PriceLevel* best_ask() const noexcept {
        return asks_.empty() ? nullptr : &asks_.begin()->second;
    }

    // Difference between best ask and best bid, in ticks. Only meaningful
    // when both sides are non-empty; check has_both_sides() first.
    [[nodiscard]] bool has_both_sides() const noexcept {
        return !bids_.empty() && !asks_.empty();
    }
    [[nodiscard]] Price spread() const noexcept {
        return has_both_sides() ? asks_.begin()->first - bids_.begin()->first : 0;
    }

    // ---- depth / introspection -------------------------------------------

    [[nodiscard]] Quantity volume_at(Side side, Price price) const noexcept;
    [[nodiscard]] const Order* find_order(OrderId id) const noexcept { return tracker_.find(id); }

    [[nodiscard]] std::size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] std::size_t ask_level_count() const noexcept { return asks_.size(); }
    [[nodiscard]] std::size_t live_order_count() const noexcept { return tracker_.size(); }

    [[nodiscard]] const BidLevels& bids() const noexcept { return bids_; }
    [[nodiscard]] const AskLevels& asks() const noexcept { return asks_; }

    [[nodiscard]] const OrderPool& pool() const noexcept { return pool_; }

    // Return every resting order to the pool and empty both sides.
    void clear() noexcept;

    // Cold, O(orders): every level verifies, no level is empty, the book is
    // not crossed, and the tracker agrees with the levels. Debug / test only.
    [[nodiscard]] bool verify() const noexcept;

private:
    // Sweep `side_book` while the incoming limit allows, filling from the top.
    // Returns the quantity left unfilled.
    template <typename LevelMap, typename Crosses>
    [[nodiscard]] Quantity match(LevelMap& book, OrderId taker_id, Side taker_side,
                                 Quantity qty, Crosses crosses) noexcept;

    // Allocate from the pool, link into `book` at `price`, index it.
    // Returns false only if the pool is exhausted.
    template <typename LevelMap>
    [[nodiscard]] bool rest(LevelMap& book, OrderId order_id, Side side,
                            Price price, Quantity qty) noexcept;

    OrderPool    pool_;
    OrderTracker tracker_;
    BidLevels    bids_;
    AskLevels    asks_;

    // Reused across calls: cleared, never shrunk, reserved once at start-up,
    // so appending a fill is a bounds check and a store -- no allocation on
    // the matching path.
    std::vector<Trade> trades_;
    ExecutionReport    report_;

    Timestamp sequence_ = 0;  // monotonic arrival stamp, drives FIFO priority
};

} // namespace lob
