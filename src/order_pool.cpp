#include "lob/order_pool.hpp"

#include <new>
#include <stdexcept>

namespace lob {

namespace {
// Conservative page size; only used to stride the prefault loop, so being
// wrong (small) merely costs a few extra writes at start-up.
constexpr std::size_t kPageSize = 4096;
} // namespace

OrderPool::OrderPool(std::size_t capacity)
    : slab_(),
      free_head_(nullptr),
      capacity_(capacity),
      free_count_(0) {
    if (capacity_ == 0) {
        throw std::invalid_argument("OrderPool capacity must be non-zero");
    }
    // 32-bit slot handles (index_of / from_index) cap the addressable slab.
    if (capacity_ > static_cast<std::size_t>(UINT32_MAX)) {
        throw std::invalid_argument("OrderPool capacity exceeds 32-bit index range");
    }

    // The single allocation of the process' trading lifetime. resize() (not
    // reserve()) so the objects legally exist and their addresses are stable
    // for as long as the pool lives -- the vector is never grown afterwards,
    // so no reallocation can ever invalidate a handed-out Order*.
    slab_.resize(capacity_);

    reset();
}

void OrderPool::reset() noexcept {
    Order* const base = slab_.data();

    // Thread the free list backwards so that popping walks the slab forwards,
    // from slot 0 upwards. Sequential first-touch keeps the early allocations
    // dense in cache and lets the hardware prefetcher help.
    free_head_ = nullptr;
    for (std::size_t i = capacity_; i-- > 0;) {
        Order* const node = base + i;
        node->prev = nullptr;
        node->next = free_head_;
        free_head_ = node;
    }
    free_count_ = capacity_;
}

void OrderPool::prefault() noexcept {
    auto* const bytes = reinterpret_cast<volatile unsigned char*>(slab_.data());
    const std::size_t total = capacity_ * sizeof(Order);
    for (std::size_t offset = 0; offset < total; offset += kPageSize) {
        bytes[offset] = bytes[offset];
    }
}

} // namespace lob
