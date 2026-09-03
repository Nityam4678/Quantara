#pragma once

#include "lob/order.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// OrderPool
//
// Fixed-capacity slab of Order objects, carved out once at start-up and never
// touched by the allocator again. During the trading session allocate() and
// deallocate() are pure pointer swings: no new/malloc, no locks, no syscalls,
// no page faults (the slab is pre-faulted by prefault()), no branches beyond a
// single exhaustion check.
//
// The free list is INTRUSIVE: a free Order stores the next free Order in its
// own `next` field. That costs zero extra memory and, more importantly, zero
// extra cache misses -- the line we touch to pop the free list is the exact
// line the caller is about to write into anyway. A side vector of Order*
// would need a second, independent line per allocation.
//
// Not thread-safe by design. In this architecture one book is owned by one
// core; sharing a pool across threads would reintroduce the atomics we are
// trying to avoid. Give each engine thread its own pool.
// ---------------------------------------------------------------------------
class OrderPool {
public:
    static constexpr std::size_t kDefaultCapacity = 1'000'000;

    explicit OrderPool(std::size_t capacity = kDefaultCapacity);

    ~OrderPool() = default;

    // A pool owns raw pointers handed out to the book; copying or moving it
    // would dangle every one of them.
    OrderPool(const OrderPool&)            = delete;
    OrderPool& operator=(const OrderPool&) = delete;
    OrderPool(OrderPool&&)                 = delete;
    OrderPool& operator=(OrderPool&&)      = delete;

    // ---- hot path -----------------------------------------------------

    // Pop one Order off the free list. O(1), no allocation, never throws.
    // Returns nullptr if the pool is exhausted -- the caller must reject the
    // incoming order rather than block or grow (growing would mean a malloc
    // and a multi-microsecond latency spike mid-session).
    //
    // The returned object is raw memory: every field except prev/next is
    // indeterminate and the caller is expected to overwrite all of them.
    [[nodiscard]] Order* allocate() noexcept {
        Order* const node = free_head_;
        if (node == nullptr) [[unlikely]] {
            return nullptr;
        }
        free_head_ = node->next;
        node->next = nullptr;
        node->prev = nullptr;
        --free_count_;
        return node;
    }

    // Push an Order back onto the free list. O(1), never throws.
    // `order` must have come from this pool and must already be unlinked from
    // any book-side list.
    void deallocate(Order* order) noexcept {
        assert(order != nullptr && "deallocate(nullptr)");
        assert(owns(order) && "order does not belong to this pool");
        assert(free_count_ < capacity_ && "double free / pool overflow");

        order->prev = nullptr;
        order->next = free_head_;
        free_head_  = order;
        ++free_count_;
    }

    // ---- compact handles ----------------------------------------------
    // 32-bit slot index instead of a 64-bit pointer: halves the size of any
    // id -> order index built on top of the pool.

    [[nodiscard]] std::uint32_t index_of(const Order* order) const noexcept {
        assert(owns(order));
        return static_cast<std::uint32_t>(order - slab_.data());
    }

    [[nodiscard]] Order* from_index(std::uint32_t index) noexcept {
        assert(index < capacity_);
        return slab_.data() + index;
    }

    // ---- introspection (cold) -----------------------------------------

    [[nodiscard]] std::size_t capacity()  const noexcept { return capacity_; }
    [[nodiscard]] std::size_t available() const noexcept { return free_count_; }
    [[nodiscard]] std::size_t in_use()    const noexcept { return capacity_ - free_count_; }
    [[nodiscard]] bool        empty()     const noexcept { return free_head_ == nullptr; }

    [[nodiscard]] bool owns(const Order* order) const noexcept {
        return order >= slab_.data() && order < slab_.data() + capacity_;
    }

    // Return every object to the free list. Start-of-day / test use only --
    // invalidates every outstanding pointer.
    void reset() noexcept;

    // Touch one byte per 4 KiB page so the OS commits the whole slab before
    // the session opens. Without this the first million allocations each pay
    // a soft page fault. Cold path: call once, at start-up.
    void prefault() noexcept;

private:
    std::vector<Order> slab_;        // the one and only allocation
    Order*             free_head_;   // head of the intrusive free list
    std::size_t        capacity_;
    std::size_t        free_count_;
};

} // namespace lob
