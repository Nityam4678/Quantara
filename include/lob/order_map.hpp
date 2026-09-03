#pragma once

#include "lob/order.hpp"

#include <cstddef>
#include <unordered_map>

namespace lob {

// ---------------------------------------------------------------------------
// OrderMap -- order_id -> resting Order*
//
// This is what makes cancels O(1): the client sends an id, we get back the
// exact node, and PriceLevel::remove_order splices it out without touching
// any other order.
//
// Identity hash. order_id is already a dense counter from the gateway, and
// libstdc++'s std::hash<uint64_t> is the identity anyway; naming it here
// documents the assumption and stops anyone "improving" it into something
// with a multiply on the hot path.
// ---------------------------------------------------------------------------
struct OrderIdHash {
    using is_avalanching = void;  // hint for ankerl/boost flat maps, ignored by std
    [[nodiscard]] constexpr std::size_t operator()(OrderId id) const noexcept {
        return static_cast<std::size_t>(id);
    }
};

using OrderMap = std::unordered_map<OrderId, Order*, OrderIdHash>;

// ---------------------------------------------------------------------------
// OrderTracker
//
// Thin wrapper over OrderMap. It exists to be the single seam we replace: once
// ids are confirmed contiguous this becomes a flat std::vector<Order*> indexed
// by (id - base_id), or an open-addressing table, and nothing above it changes.
//
// Honest caveat: std::unordered_map is node-based, so track() can allocate --
// it is the one remaining malloc on an otherwise allocation-free path. reserve
// up front to push rehashing out of the session, and treat this as the next
// thing to optimise.
// ---------------------------------------------------------------------------
class OrderTracker {
public:
    static constexpr std::size_t kDefaultCapacity = 1'000'000;

    explicit OrderTracker(std::size_t expected_orders = kDefaultCapacity) {
        // Fewer collisions, more memory. Buckets are pointers; the trade is
        // cheap and a rehash mid-session is a latency spike we cannot take.
        map_.max_load_factor(0.5F);
        map_.reserve(expected_orders);
    }

    OrderTracker(const OrderTracker&)            = delete;
    OrderTracker& operator=(const OrderTracker&) = delete;

    // Register a live order. May allocate (see caveat above).
    // Returns false if the id was already present -- a duplicate id is a
    // gateway bug, and silently overwriting would leak the earlier order.
    [[nodiscard]] bool track(OrderId id, Order* order) {
        return map_.emplace(id, order).second;
    }

    // O(1). Returns nullptr if unknown (already filled, already cancelled, or
    // never existed) -- the common case for a late cancel, not an error.
    [[nodiscard]] Order* find(OrderId id) const noexcept {
        const auto it = map_.find(id);
        return it != map_.end() ? it->second : nullptr;
    }

    // Deregister. Returns the Order* that was removed, or nullptr if the id
    // was unknown. Returning it saves the caller a second lookup before
    // handing the order back to the pool.
    Order* untrack(OrderId id) noexcept {
        const auto it = map_.find(id);
        if (it == map_.end()) [[unlikely]] {
            return nullptr;
        }
        Order* const order = it->second;
        map_.erase(it);
        return order;
    }

    [[nodiscard]] bool        contains(OrderId id) const noexcept { return map_.contains(id); }
    [[nodiscard]] std::size_t size()               const noexcept { return map_.size(); }
    [[nodiscard]] bool        empty()              const noexcept { return map_.empty(); }

    void clear() noexcept { map_.clear(); }

    [[nodiscard]] const OrderMap& raw() const noexcept { return map_; }

private:
    OrderMap map_;
};

} // namespace lob
