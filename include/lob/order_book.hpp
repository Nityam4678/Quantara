#pragma once

#include "lob/order.hpp"
#include "lob/order_map.hpp"
#include "lob/order_pool.hpp"
#include "lob/price_level.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// Fixed tick grid.
//
// Every price in the book is an integer tick in [1, MAX_PRICE_TICKS), and that
// tick IS the array index. No tree, no hashing, no comparison chain: locating
// a price level is one shift and one add.
//
// Tick 0 is reserved as the "no bid" sentinel, so a price of 0 is rejected.
// ---------------------------------------------------------------------------
inline constexpr Price MAX_PRICE_TICKS = 100'000;

using LevelArray = std::array<PriceLevel, MAX_PRICE_TICKS>;

// ---------------------------------------------------------------------------
// Trade -- one fill between an incoming aggressor and one resting maker.
// Executed at the MAKER's price (standard price-time priority: the passive
// side set the terms, the aggressor accepted them), so a marketable order that
// sweeps several levels produces one Trade per level touched, each at its own
// price.
// ---------------------------------------------------------------------------
struct Trade {
    OrderId  maker_order_id;
    OrderId  taker_order_id;
    Price    price;          // the resting order's price
    Quantity quantity;
    Side     aggressor_side; // side of the incoming order
};

enum class OrderStatus : std::uint8_t {
    Ok = 0,                  // add: accepted. cancel: removed.
    RejectedDuplicateId,     // add: order_id is already live in the book
    RejectedZeroQuantity,
    RejectedZeroPrice,       // tick 0 is the empty-bid sentinel
    RejectedPriceOutOfRange, // add: price >= MAX_PRICE_TICKS
    RejectedPoolExhausted,   // add: no free slot to rest the residual
    CancelUnknownId          // cancel: no live order with that id
};

// Outcome of the last add_order / cancel_order.
struct ExecutionReport {
    OrderId     order_id = 0;
    Quantity    filled   = 0;   // quantity traded against the book
    Quantity    resting  = 0;   // quantity left sitting (add) / withdrawn (cancel)
    OrderStatus status   = OrderStatus::Ok;

    [[nodiscard]] bool ok()               const noexcept { return status == OrderStatus::Ok; }
    [[nodiscard]] bool fully_filled()     const noexcept { return ok() && resting == 0 && filled > 0; }
    [[nodiscard]] bool partially_filled() const noexcept { return filled > 0 && resting > 0; }
};

// ---------------------------------------------------------------------------
// OrderBook
//
// Price levels live in two flat arrays indexed directly by tick. Compared to
// the std::map version this removes, per operation: an O(log L) tree descent,
// a pointer chase per tree node, and a heap allocation whenever a price is
// touched for the first time. Levels are now permanently resident, so an
// emptied level is simply left empty rather than erased.
//
// Top of book is cached in best_bid_ / best_ask_ and repaired by a linear
// scan whenever the touch level empties. Sentinels:
//     best_bid_ == 0                -> no bids
//     best_ask_ == MAX_PRICE_TICKS  -> no asks
// Both sentinels fail their own cross test against any legal price, so the
// matching loops need no separate emptiness check.
//
// The arrays are heap-allocated (6.1 MB for the pair) because putting them
// inline would make OrderBook far too large to place on the stack. They are
// allocated once and never moved, so Order::level back-pointers stay valid.
//
// Not thread-safe: one book, one thread, one core. See OrderPool.
// ---------------------------------------------------------------------------
class OrderBook {
public:
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

    // Pull a resting order: O(1) hash lookup, O(1) intrusive splice, plus a
    // best-price rescan only when the cancel empties the touch level.
    void cancel_order(OrderId order_id) noexcept;

    // ---- results of the last call --------------------------------------

    [[nodiscard]] const std::vector<Trade>& last_trades() const noexcept { return trades_; }
    [[nodiscard]] const ExecutionReport&    last_report() const noexcept { return report_; }

    // ---- top of book ----------------------------------------------------
    // nullptr when that side is empty -- no sentinel price to misread.

    [[nodiscard]] const PriceLevel* best_bid() const noexcept {
        return (best_bid_ == 0) ? nullptr : &(*bids_)[best_bid_];
    }
    [[nodiscard]] const PriceLevel* best_ask() const noexcept {
        return (best_ask_ == MAX_PRICE_TICKS) ? nullptr : &(*asks_)[best_ask_];
    }

    // Raw ticks, sentinels included. Prefer best_bid()/best_ask() unless you
    // are deliberately working with the sentinel values.
    [[nodiscard]] Price best_bid_tick() const noexcept { return best_bid_; }
    [[nodiscard]] Price best_ask_tick() const noexcept { return best_ask_; }

    [[nodiscard]] bool has_both_sides() const noexcept {
        return best_bid_ != 0 && best_ask_ != MAX_PRICE_TICKS;
    }
    [[nodiscard]] Price spread() const noexcept {
        return has_both_sides() ? best_ask_ - best_bid_ : 0;
    }

    // ---- depth / introspection -------------------------------------------

    [[nodiscard]] Quantity volume_at(Side side, Price price) const noexcept {
        if (price == 0 || price >= MAX_PRICE_TICKS) [[unlikely]] {
            return 0;
        }
        return (side == Side::BUY) ? (*bids_)[price].total_volume()
                                   : (*asks_)[price].total_volume();
    }

    [[nodiscard]] const PriceLevel* level_at(Side side, Price price) const noexcept {
        if (price == 0 || price >= MAX_PRICE_TICKS) [[unlikely]] {
            return nullptr;
        }
        return (side == Side::BUY) ? &(*bids_)[price] : &(*asks_)[price];
    }

    [[nodiscard]] const Order* find_order(OrderId id) const noexcept { return tracker_.find(id); }

    [[nodiscard]] std::size_t bid_level_count()  const noexcept { return bid_levels_; }
    [[nodiscard]] std::size_t ask_level_count()  const noexcept { return ask_levels_; }
    [[nodiscard]] std::size_t live_order_count() const noexcept { return tracker_.size(); }

    [[nodiscard]] const OrderPool& pool() const noexcept { return pool_; }

    // Return every resting order to the pool and empty both sides.
    void clear() noexcept;

    // Cold, O(MAX_PRICE_TICKS): sweeps BOTH arrays end to end, so it also
    // catches a stale best_bid_/best_ask_ that a bounded scan would miss.
    // Debug / test only -- far too slow for the hot path.
    [[nodiscard]] bool verify() const noexcept;

private:
    // Sweep the opposing side while the aggressor's limit still crosses.
    // Returns the quantity left unfilled. TakerSide picks the side, the scan
    // direction and the cross test at compile time.
    template <Side TakerSide>
    [[nodiscard]] Quantity match(OrderId taker_id, Quantity qty, Price limit) noexcept;

    // Allocate from the pool, link into the level at `price`, index it.
    // Returns false only if the pool is exhausted.
    template <Side OrderSide>
    [[nodiscard]] bool rest(OrderId order_id, Price price, Quantity qty) noexcept;

    // Walk best_bid_ down / best_ask_ up to the next non-empty level. Called
    // only when the touch level empties. Both short-circuit to the sentinel
    // when the side has no levels left, which is what stops a drained side
    // from scanning the entire array.
    void repair_best_bid() noexcept;
    void repair_best_ask() noexcept;

    OrderPool    pool_;
    OrderTracker tracker_;

    std::unique_ptr<LevelArray> bids_;
    std::unique_ptr<LevelArray> asks_;

    Price       best_bid_   = 0;                // 0 == no bids
    Price       best_ask_   = MAX_PRICE_TICKS;  // MAX == no asks
    std::size_t bid_levels_ = 0;                // non-empty bid levels
    std::size_t ask_levels_ = 0;                // non-empty ask levels

    // Reused across calls: cleared, never shrunk, reserved once at start-up,
    // so appending a fill is a bounds check and a store -- no allocation on
    // the matching path.
    std::vector<Trade> trades_;
    ExecutionReport    report_;

    Timestamp sequence_ = 0;  // monotonic arrival stamp, drives FIFO priority
};

} // namespace lob
