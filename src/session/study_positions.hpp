#pragma once
#include "study_policy.hpp"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace eme::session::detail {
// Exact base-1e9 accumulation of paid microUSD * nanoseconds, without floating
// point or compiler-specific wide integers. Unspent reservations are excluded.
struct CapitalTime final {
    std::uint64_t micro_usd_seconds{}, fractional_micro_usd_nanoseconds{};
    void add(const std::int64_t cash, const std::int64_t nanoseconds) {
        if (cash < 0 || nanoseconds < 0) { invalid("negative capital-time input"); }
        constexpr std::uint64_t scale = 1'000'000'000U;
        const auto amount = static_cast<std::uint64_t>(cash);
        const auto time = static_cast<std::uint64_t>(nanoseconds);
        const auto seconds = time / scale;
        const auto ns = time % scale;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (seconds != 0U && amount > maximum / seconds) { invalid("capital-time overflow"); }
        const auto whole = amount * seconds;
        const auto fractional = (amount % scale) * ns + fractional_micro_usd_nanoseconds;
        const auto extra = (amount / scale) * ns + fractional / scale;
        if (whole > maximum - extra || micro_usd_seconds > maximum - whole - extra) { invalid("capital-time overflow"); }
        micro_usd_seconds += whole + extra;
        fractional_micro_usd_nanoseconds = fractional % scale;
    }
};

// Each attempt owns its lots. Exiting one attempt cannot consume another's
// position or its matched portfolio. Index links remain valid across vector moves.
class StudyPositions final {
    static constexpr auto none = std::numeric_limits<std::size_t>::max();
    struct Lot final {
        std::int64_t quantity{}, debit{}, acquired{};
        std::size_t next{none};
    };
    struct Position final {
        constraint::PayoffLegTemplate leg{};
        std::int64_t quantity{};
        std::size_t head{none}, tail{none};
    };
public:
    void reserve(const std::size_t attempts, const std::size_t lots) {
        positions_.resize(attempts * 2U); lots_.reserve(lots);
    }
    void acquire(const std::size_t attempt, const std::size_t leg_index,
                 const constraint::PayoffLegTemplate leg, const std::int64_t quantity,
                 const std::int64_t debit, const std::int64_t time) {
        auto& position = at(attempt, leg_index);
        if (quantity <= 0 || quantity > quantity_limit - position.quantity || debit < 0 || debit > cash_limit) {
            invalid("position acquisition bound");
        }
        position.leg = leg;
        position.quantity += quantity;
        if (position.tail != none) { lots_[position.tail].next = lots_.size(); }
        if (position.head == none) { position.head = lots_.size(); }
        position.tail = lots_.size();
        lots_.push_back({quantity, debit, time, none});
    }
    [[nodiscard]] std::int64_t quantity(const std::size_t attempt, const std::size_t leg) const {
        return positions_.at(attempt * 2U + leg).quantity;
    }
    // FIFO by acquisition order. Allocate integer cost proportionally to sold
    // quantity; the unsold remainder retains every leftover microdollar.
    std::int64_t close(const std::size_t attempt, const std::size_t leg,
                       const std::int64_t quantity, const std::int64_t time) {
        return close_position(at(attempt, leg), quantity, time);
    }
    struct SettlementResult final { std::int64_t quantity{}, payout{}; };
    [[nodiscard]] SettlementResult settle(const Settlement& settlement) {
        SettlementResult result;
        for (auto& position : positions_) {
            if (position.quantity == 0 || position.leg.market_id != settlement.market_id) { continue; }
            const auto q = position.quantity;
            if (q > std::numeric_limits<std::int64_t>::max() - result.quantity) { invalid("settled quantity overflow"); }
            result.quantity += q;
            if ((position.leg.outcome == constraint::ContractOutcome::yes) == settlement.yes_wins) {
                if (q * 10'000 > cash_limit - result.payout) { invalid("settlement payout bound"); }
                result.payout += q * 10'000;
            }
            (void)close_position(position, q, settlement.time);
        }
        return result;
    }
    void finish(const std::int64_t time) {
        for (const auto& lot : lots_) {
            if (lot.quantity != 0) { capital_time_.add(lot.debit, time - lot.acquired); }
        }
    }
    [[nodiscard]] auto unsettled_lots() const {
        return std::count_if(lots_.begin(), lots_.end(), [](const auto& lot) { return lot.quantity != 0; });
    }
    [[nodiscard]] const CapitalTime& capital_time() const { return capital_time_; }
private:
    Position& at(const std::size_t attempt, const std::size_t leg) { return positions_.at(attempt * 2U + leg); }
    std::int64_t close_position(Position& position, const std::int64_t quantity, const std::int64_t time) {
        if (quantity < 0 || quantity > position.quantity) { invalid("cannot close unowned position"); }
        auto remaining = quantity;
        std::int64_t released = 0;
        while (remaining != 0) {
            if (position.head == none) { invalid("position lot accounting"); }
            auto& lot = lots_[position.head];
            const auto take = std::min(remaining, lot.quantity);
            // Both quantities <= 1e8: remainder * take <= 1e16 fits int64.
            const auto basis = take == lot.quantity ? lot.debit :
                (lot.debit / lot.quantity) * take + (lot.debit % lot.quantity) * take / lot.quantity;
            capital_time_.add(basis, time - lot.acquired);
            lot.quantity -= take; lot.debit -= basis;
            remaining -= take; released += basis;
            if (lot.quantity == 0) {
                if (lot.debit != 0) { invalid("closed lot cost remainder"); }
                position.head = lot.next;
                if (position.head == none) { position.tail = none; }
            }
        }
        position.quantity -= quantity;
        return released;
    }
    std::vector<Position> positions_;
    std::vector<Lot> lots_;
    CapitalTime capital_time_;
};
} // namespace eme::session::detail
