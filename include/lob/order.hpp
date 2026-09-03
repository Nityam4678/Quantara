#pragma once

#include <cstdint>
#include <type_traits>

namespace lob {

class PriceLevel;  // forward decl -- Order carries a back-pointer to its level

// ---------------------------------------------------------------------------
// Fundamental scalar types.
// Prices are integral ticks (never floating point): deterministic comparison,
// no rounding, exact equality for price-level bucketing.
// ---------------------------------------------------------------------------
using OrderId  = std::uint64_t;
using Price    = std::uint64_t;  // price in ticks
using Quantity = std::uint32_t;  // lots / shares
using Timestamp = std::uint64_t; // nanoseconds since epoch (or raw TSC ticks)

enum class Side : std::uint8_t {
    BUY  = 0,
    SELL = 1
};

[[nodiscard]] constexpr Side opposite(Side s) noexcept {
    return static_cast<Side>(static_cast<std::uint8_t>(s) ^ std::uint8_t{1});
}

// ---------------------------------------------------------------------------
// Order
//
// Intrusive node of the doubly linked FIFO queue that hangs off each price
// level. Because the links live *inside* the order, cancelling is O(1):
// given an Order* (found via an id -> Order* hash map) we splice it out
// without ever searching the level.
//
// Layout is hand-ordered: the 8-byte members first, then the small scalars,
// so the compiler inserts zero internal padding. The type stays trivial
// (trivially constructible / copyable / destructible) so the pool can hand
// out raw memory without running any constructor on the hot path.
// ---------------------------------------------------------------------------
struct alignas(64) Order {
    OrderId     order_id;   // 8
    Price       price;      // 8  (ticks)
    Timestamp   timestamp;  // 8  (arrival time, drives FIFO priority)
    Order*      prev;       // 8  intrusive link
    Order*      next;       // 8  intrusive link (also the pool free-list link)
    PriceLevel* level;      // 8  owning level, nullptr when not resting
    Quantity    quantity;   // 4  remaining, unfilled quantity
    Side        side;       // 1
    // 11 bytes of tail padding up to the 64-byte alignment.
    //
    // `level` rides for free in what was already padding, so sizeof stays 64.
    // Without it a cancel would have to re-find the level by price -- an
    // O(log L) tree descent through the std::map. With it, cancel is a hash
    // lookup plus a pointer splice, which is what "O(1) cancel" actually
    // requires. std::map guarantees its nodes never move, so the pointer
    // stays valid for as long as the level is in the book, and a level is
    // only erased once it is empty (i.e. once nothing points at it).

    // Explicitly trivial: no user-provided ctor/dtor anywhere in this struct.

    // ---- intrusive list helpers (all branchless-ish, all O(1)) ----

    // Low-level splice: repairs the neighbours only. It cannot fix the owning
    // level's head_/tail_ or its aggregates, so it is NOT the way to cancel --
    // use PriceLevel::remove_order for anything resting in the book. Kept for
    // detached lists that no level owns.
    void unlink() noexcept {
        if (prev != nullptr) prev->next = next;
        if (next != nullptr) next->prev = prev;
        prev  = nullptr;
        next  = nullptr;
        level = nullptr;
    }

    [[nodiscard]] bool is_resting() const noexcept { return level != nullptr; }

    [[nodiscard]] bool is_buy()  const noexcept { return side == Side::BUY;  }
    [[nodiscard]] bool is_sell() const noexcept { return side == Side::SELL; }
};

// Hard guarantees the rest of the engine relies on.
static_assert(std::is_trivial_v<Order>,                "Order must be trivial");
static_assert(std::is_trivially_destructible_v<Order>, "Order must be trivially destructible");
static_assert(std::is_standard_layout_v<Order>,        "Order must be standard layout");
static_assert(sizeof(Order)  == 64, "Order must occupy exactly one cache line");
static_assert(alignof(Order) == 64, "Order must be cache-line aligned");

} // namespace lob
