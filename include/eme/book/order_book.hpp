#pragma once

#include "eme/core/fixed_point.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

namespace eme::book {

using StreamId = std::uint64_t;
using SequenceNumber = std::uint64_t;

enum class Side : std::uint8_t {
    bid,
    ask,
};

enum class BookState : std::uint8_t {
    empty,
    valid,
    stale,
    recovering,
};

enum class BookUpdateResult : std::uint8_t {
    applied,
    requires_snapshot,
    stream_mismatch,
    sequence_gap,
    invalid_level,
    recovery_not_started,
    stale_snapshot,
};

struct Level final {
    core::Price price;
    core::Quantity quantity;
};

[[nodiscard]] constexpr std::string_view to_string(const BookState state) noexcept {
    switch (state) {
        case BookState::empty:
            return "EMPTY";
        case BookState::valid:
            return "VALID";
        case BookState::stale:
            return "STALE";
        case BookState::recovering:
            return "RECOVERING";
    }
    return "UNKNOWN";
}

class OrderBook final {
public:
    [[nodiscard]] BookUpdateResult apply_snapshot(
        StreamId stream_id,
        SequenceNumber sequence,
        const std::vector<Level>& bids,
        const std::vector<Level>& asks);

    [[nodiscard]] BookUpdateResult apply_delta(
        StreamId stream_id,
        SequenceNumber sequence,
        Side side,
        core::Price price,
        core::QuantityDelta quantity_delta);

    void mark_stale() noexcept;
    [[nodiscard]] bool begin_recovery() noexcept;

    [[nodiscard]] BookState state() const noexcept { return state_; }
    [[nodiscard]] std::optional<StreamId> stream_id() const noexcept { return stream_id_; }
    [[nodiscard]] std::optional<SequenceNumber> last_sequence() const noexcept {
        return last_sequence_;
    }
    [[nodiscard]] std::optional<core::Price> best_bid() const noexcept;
    [[nodiscard]] std::optional<core::Price> best_ask() const noexcept;
    [[nodiscard]] core::Quantity quantity_at(Side side, core::Price price) const noexcept;
    [[nodiscard]] std::size_t level_count(Side side) const noexcept;

    // Best to worst, without copying depth. A false return stops traversal.
    // The visitor must not mutate this book; invalid books expose no liquidity.
    template <typename Visitor>
    void visit_levels(const Side side, Visitor&& visitor) const {
        if (state_ != BookState::valid) { return; }
        const auto visit = [&visitor](const auto& entry) {
            return visitor(Level{*core::Price::from_raw(entry.first),
                                 *core::Quantity::from_raw(entry.second)});
        };
        if (side == Side::bid) {
            for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
                if (!visit(*it)) { break; }
            }
        } else {
            for (const auto& entry : asks_) { if (!visit(entry)) { break; } }
        }
    }

private:
    using Levels = std::map<std::int64_t, std::int64_t>;

    [[nodiscard]] Levels& levels_for(Side side) noexcept;
    [[nodiscard]] const Levels& levels_for(Side side) const noexcept;

    BookState state_{BookState::empty};
    std::optional<StreamId> stream_id_;
    std::optional<SequenceNumber> last_sequence_;
    Levels bids_;
    Levels asks_;
};

}  // namespace eme::book
