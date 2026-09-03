#pragma once

#include "lob/order.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lob {

// ---------------------------------------------------------------------------
// PriceLevel
//
// One price point in the book, holding its resting orders in strict FIFO
// time priority (head = oldest = next to be filled, tail = newest).
//
// The queue is an INTRUSIVE doubly linked list running through Order::prev /
// Order::next. There is no node allocation and no indirection: given an
// Order* -- which the OrderMap hands us in O(1) -- a cancel is a pointer
// splice, no traversal. std::list would allocate a separate node per order
// and put the payload one pointer hop away from the links, which is two cache
// misses per hop instead of one (Order is exactly one cache line).
//
// `total_volume` is the level's aggregate resting quantity, kept incrementally
// so the book never has to walk the queue to quote a size.
//
// Not thread-safe: one book, one thread. See OrderPool.
// ---------------------------------------------------------------------------
class PriceLevel {
public:
    PriceLevel() noexcept = default;

    explicit PriceLevel(Price price) noexcept : price_(price) {}

    // A level owns no memory -- the Orders live in the OrderPool -- but the
    // head/tail pointers alias pool objects, so copying would create two
    // levels claiming the same queue.
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    // A PriceLevel is PINNED: every resting Order holds a back-pointer to its
    // level, so relocating one would dangle every order on it. Move is deleted
    // to make that a compile error rather than a 3am bug -- std::map never
    // relocates its nodes, so the book gets stability for free. If levels are
    // ever moved into a flat array, orders must reference them by index, not
    // by pointer.
    PriceLevel(PriceLevel&&)            = delete;
    PriceLevel& operator=(PriceLevel&&) = delete;

    // ---- hot path ------------------------------------------------------

    // Append at the tail: newest order, lowest time priority. O(1).
    // The order must not already be linked into a level.
    inline void append_order(Order* order) noexcept {
        assert(order != nullptr);
        assert(order->prev == nullptr && order->next == nullptr &&
               order->level == nullptr &&
               "order is still linked into another level");
        assert(total_volume_ <= std::numeric_limits<Quantity>::max() - order->quantity &&
               "total_volume overflow");

        order->prev = tail_;
        order->next = nullptr;

        if (tail_ != nullptr) {
            tail_->next = order;
        } else {
            head_ = order;   // first order on the level
        }
        tail_  = order;
        order->level = this;

        total_volume_ += order->quantity;
        ++order_count_;
    }

    // Splice an order out of the queue. O(1) -- no search, the order carries
    // its own neighbours. `order` must currently be linked into THIS level.
    inline void remove_order(Order* order) noexcept {
        assert(order != nullptr);
        assert(order->level == this && "order does not rest on this level");
        assert(order_count_ > 0);
        assert(total_volume_ >= order->quantity);

        Order* const prev = order->prev;
        Order* const next = order->next;

        if (prev != nullptr) {
            prev->next = next;
        } else {
            assert(head_ == order && "order is not the head of this level");
            head_ = next;
        }

        if (next != nullptr) {
            next->prev = prev;
        } else {
            assert(tail_ == order && "order is not the tail of this level");
            tail_ = prev;
        }

        order->prev  = nullptr;
        order->next  = nullptr;
        order->level = nullptr;

        total_volume_ -= order->quantity;
        --order_count_;
    }

    // Detach and return the oldest order, or nullptr if empty. O(1).
    // Used by the matching loop when an aggressor fully consumes the front.
    [[nodiscard]] inline Order* pop_front() noexcept {
        Order* const order = head_;
        if (order == nullptr) [[unlikely]] {
            return nullptr;
        }
        remove_order(order);
        return order;
    }

    // Partial fill: shrink a resting order and the level aggregate together,
    // so total_volume can never drift from the sum of the queue. O(1).
    inline void reduce_order(Order* order, Quantity filled) noexcept {
        assert(order != nullptr);
        assert(filled <= order->quantity && "fill exceeds resting quantity");
        assert(total_volume_ >= filled);

        order->quantity -= filled;
        total_volume_   -= filled;
    }

    // ---- accessors -----------------------------------------------------

    [[nodiscard]] inline Price       price()        const noexcept { return price_; }
    [[nodiscard]] inline Quantity    total_volume() const noexcept { return total_volume_; }
    [[nodiscard]] inline std::size_t order_count()  const noexcept { return order_count_; }  // NOLINT
    [[nodiscard]] inline bool        empty()        const noexcept { return head_ == nullptr; }

    [[nodiscard]] inline Order*       front()       noexcept { return head_; }
    [[nodiscard]] inline const Order* front() const noexcept { return head_; }
    [[nodiscard]] inline Order*       back()        noexcept { return tail_; }
    [[nodiscard]] inline const Order* back()  const noexcept { return tail_; }

    inline void set_price(Price price) noexcept {
        assert(empty() && "cannot reprice a level that holds orders");
        price_ = price;
    }

    // Drop every order without returning them, and without clearing their
    // `level` back-pointers. The caller is responsible for handing the Orders
    // back to the pool first, or they leak into the slab.
    inline void clear() noexcept {
        head_         = nullptr;
        tail_         = nullptr;
        total_volume_ = 0;
        order_count_  = 0;
    }

    // ---- cold path -----------------------------------------------------

    // Walk the queue and confirm the links, the count and the aggregate
    // volume all agree. Debug / test only -- O(n).
    [[nodiscard]] bool verify() const noexcept;

private:
    // Exactly 32 bytes: two levels per cache line. That matters now that the
    // book stores levels in a flat array and scans them linearly to find the
    // next non-empty price -- halving the stride halves the scan's cache cost.
    Order*        head_         = nullptr;  // oldest -- next to fill
    Order*        tail_         = nullptr;  // newest
    Price         price_        = 0;
    Quantity      total_volume_ = 0;
    std::uint32_t order_count_  = 0;        // exposed as size_t; 4B here
};

static_assert(sizeof(PriceLevel) == 32, "PriceLevel must be half a cache line");
static_assert(alignof(PriceLevel) == 8, "PriceLevel alignment");

} // namespace lob
