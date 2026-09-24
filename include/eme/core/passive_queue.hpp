#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <stdexcept>

namespace eme::core {

// Sensitivity model for an aggregate feed, never an exchange queue position.
// Exact-price trades consume priority. Depth reductions cannot create fills.
// In the favorable scenario only, unmatched reductions advance priority after
// a reconciliation window; matching works for trade-before-delta or the reverse.
class PassiveQueue final {
public:
    PassiveQueue(std::int64_t displayed, bool cancellations_ahead, std::int64_t window)
        : ahead_{displayed}, displayed_{displayed}, window_{window}, favorable_{cancellations_ahead} {}
    void advance(std::int64_t time) {
        while (!reductions_.empty() && time - reductions_.front().time > window_) {
            if (favorable_) { ahead_ = std::max<std::int64_t>(0, ahead_ - reductions_.front().quantity); }
            reductions_.pop_front();
        }
        while (!credits_.empty() && time - credits_.front().time > window_) { credits_.pop_front(); }
    }
    void depth(std::int64_t time, std::int64_t displayed) {
        advance(time);
        auto reduction = std::max<std::int64_t>(0, displayed_ - displayed);
        displayed_ = displayed;
        match(credits_, reduction);
        append(reductions_, time, reduction);
    }
    std::int64_t trade(std::int64_t time, std::int64_t quantity, std::int64_t remaining) {
        advance(time);
        auto unmatched = quantity;
        match(reductions_, unmatched);
        append(credits_, time, unmatched);
        const auto consumed = std::min(ahead_, quantity);
        ahead_ -= consumed;
        return std::min(remaining, quantity - consumed);
    }
    std::int64_t ahead() const { return ahead_; }
private:
    struct Change { std::int64_t time, quantity; };
    static void match(std::deque<Change>& changes, std::int64_t& quantity) {
        while (quantity > 0 && !changes.empty()) {
            const auto used = std::min(quantity, changes.front().quantity);
            quantity -= used; changes.front().quantity -= used;
            if (!changes.front().quantity) { changes.pop_front(); }
        }
    }
    static void append(std::deque<Change>& changes, std::int64_t time, std::int64_t quantity) {
        if (!quantity) { return; }
        if (changes.size() >= 1024U) { throw std::runtime_error("passive queue reconciliation budget"); }
        changes.push_back({time, quantity});
    }
    std::int64_t ahead_, displayed_, window_;
    bool favorable_;
    std::deque<Change> reductions_, credits_;
};
}  // namespace eme::core
