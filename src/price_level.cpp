#include "lob/price_level.hpp"

namespace lob {

// Cold path: O(n) integrity walk, for asserts and tests. Deliberately out of
// line so it can never be inlined into anything on the hot path.
bool PriceLevel::verify() const noexcept {
    if (head_ == nullptr || tail_ == nullptr) {
        // An empty level must be empty in every field.
        return head_ == nullptr && tail_ == nullptr &&
               total_volume_ == 0 && order_count_ == 0;
    }

    if (head_->prev != nullptr || tail_->next != nullptr) {
        return false;
    }

    std::size_t   count  = 0;
    std::uint64_t volume = 0;
    const Order*  prev   = nullptr;

    for (const Order* node = head_; node != nullptr; node = node->next) {
        if (node->prev != prev) {
            return false;  // backward link disagrees with forward link
        }
        if (node->level != this) {
            return false;  // order thinks it rests somewhere else
        }
        volume += node->quantity;
        ++count;
        if (count > order_count_) {
            return false;  // longer than advertised, or cyclic
        }
        prev = node;
    }

    return prev == tail_ &&
           count == order_count_ &&
           volume == static_cast<std::uint64_t>(total_volume_);
}

} // namespace lob
