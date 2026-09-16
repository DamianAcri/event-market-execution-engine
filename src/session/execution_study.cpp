#include "eme/session/execution_study.hpp"
#include "eme/core/execution_cost.hpp"
#include "eme/core/net_sizing.hpp"
#include "study_policy.hpp"
#include "study_positions.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <ostream>
#include <queue>
#include <set>
#include <tuple>

namespace eme::session {
namespace {
using detail::Json;
using Outcome = constraint::ContractOutcome;
using detail::Policy;
using detail::Settlement;
using detail::cash_limit;
using detail::quantity_limit;
using detail::load_policy;

// The ledger uses microdollars/centicontracts explicitly. Its input caps prove
// aggregate cash <= 1e15 and any single position <= 1e8 centicontracts.
struct Execution final {
    std::int64_t quantity{};
    std::int64_t notional{};
    std::int64_t debit{};
    std::int64_t credit{};
    std::int64_t worst_price{};
    std::uint64_t levels{};
};
using LiquidityKey = std::tuple<market::MarketId, Outcome, std::int64_t>;
struct Attempt final {
    constraint::ConstraintId id{};
    std::array<constraint::PayoffLegTemplate, 2U> legs;
    market::ConnectionGeneration generation{};
    std::int64_t quantity{};
    std::array<std::int64_t, 2U> limit{};
    std::array<std::int64_t, 2U> reserve{};
    std::array<Execution, 2U> fills;
    std::int64_t started{};
    std::uint64_t completion_orders{};
    std::size_t pending_buys{};
    bool exit_considered{};
};
struct Pending final {
    std::int64_t arrival{};
    std::size_t attempt{};
    std::size_t leg{};
};

// Independent order arrivals/responses are explicit events. The min-heap avoids
// scanning all pending orders at every market observation as this lifecycle grows.
struct LifecycleOrder final {
    std::size_t attempt{}, leg{};
    std::int64_t quantity{}, limit{}, reserve{};
    Execution fill;
    bool arrived{}, responded{}, sell{};
};
struct LifecycleEvent final {
    std::int64_t time{};
    std::size_t order{};
    bool response{};
    bool operator<(const LifecycleEvent& other) const {
        return std::tie(time, order, response) > std::tie(other.time, other.order, other.response);
    }
};
class Simulation final : public ExecutionSimulation {
public:
    Simulation(const gateway::kalshi::MetadataSnapshot& metadata, Policy policy, std::ostream& output)
        : metadata_{metadata}, policy_{std::move(policy)}, output_{output}, available_{policy_.capital} {
        if (policy_.lifecycle) {
            const auto per_attempt = policy_.lifecycle->sequential ? 1U + policy_.lifecycle->maximum_completion_orders : 2U;
            const auto bound = metadata.constraints().size() * static_cast<std::size_t>(per_attempt + (policy_.residual_exit ? 1U : 0U));
            // At most one attempt per constraint, with a fixed retry budget.
            // Allocate event/order/position capacity before processing the feed.
            attempts_.reserve(metadata.constraints().size());
            orders_.reserve(bound); positions_.reserve(metadata.constraints().size(), bound);
            std::vector<LifecycleEvent> storage; storage.reserve(bound);
            events_ = decltype(events_){std::less<LifecycleEvent>{}, std::move(storage)};
        }
    }

    void before(const std::int64_t time, const market::MarketState& state) override {
        // All observations at an equal timestamp precede simulated arrivals.
        drain(time, false, state);
    }
    void after(const ReplayFrame& frame, std::span<const opportunity::CandidateEvent>,
               const market::MarketState& state) override {
        if (frame.market_id && frame.applied) { last_update_[*frame.market_id] = frame.time_ns; }
        if (!frame.market_id || !frame.applied) { return; }
        auto dependencies = metadata_.constraints().dependencies(*frame.market_id);
        std::vector<constraint::ConstraintId> ordered(dependencies.begin(), dependencies.end());
        std::sort(ordered.begin(), ordered.end());
        for (const auto id : ordered) {
            if (!attempted_.contains(id)) { decide(id, frame, state); }
        }
    }
    void finish(const std::int64_t time, const market::MarketState& state) override {
        drain(time, true, state);
        if (policy_.lifecycle) {
            // An arrival/response beyond EOF is uncertain, not a cancelled order.
            // Keep its reservation; future labels never create unobserved fills.
            while (!events_.empty()) {
                const auto event = events_.top(); events_.pop();
                emit({{"type", "unobserved_order_event"}, {"order_id", event.order + 1U},
                    {"event", event.response ? "response" : "arrival"}, {"time_ns", event.time}, {"observation_end_ns", time}});
                ++unobserved_;
            }
            while (settlement_index_ < policy_.lifecycle->settlements.size()) { settle(policy_.lifecycle->settlements[settlement_index_++]); }
            positions_.finish(time);
            return;
        }
        for (const auto& pending : pending_) {
            auto& attempt = attempts_[pending.attempt];
            available_ += attempt.reserve[pending.leg];
            emit({{"type", "unobserved_order"}, {"constraint_id", attempt.id}, {"leg", pending.leg},
                  {"arrival_ns", pending.arrival}, {"observation_end_ns", time}});
            ++unobserved_;
        }
        pending_.clear();
    }
    void report(const ReplayPlan& plan) override {
        std::int64_t floor = 0;
        std::uint64_t complete = 0U;
        std::uint64_t legged = 0U;
        for (const auto& attempt : attempts_) {
            const auto a = attempt.fills[0U].quantity;
            const auto b = attempt.fills[1U].quantity;
            const auto payout = std::min(a, b) * 10'000;
            if (floor > cash_limit - payout) { detail::invalid("aggregate payout exceeds study bound"); }
            floor += payout;
            if (a == attempt.quantity && b == attempt.quantity) { ++complete; }
            if (a != b) { ++legged; }
        }
        Json result{{"type", "study_complete"}, {"schema_version", 1U}, {"policy_sha256", policy_.hash},
            {"manifest_sha256", plan.manifest_sha256}, {"plan_sha256", plan.plan_sha256},
            {"source_kind", plan.source_kind}, {"strategy", policy_.json["strategy"]},
            {"evidence_status", plan.source_kind == "synthetic" ? "synthetic_validation_only" : "observational_simulation_not_profitability_proof"},
            {"attempts", attempts_.size()}, {"completed_pairs", complete}, {"unbalanced_pairs", legged},
            {"unobserved_orders", unobserved_}, {"decisions_evaluated", evaluated_},
            {"declined", declines_}, {"initial_capital_micro_usd", policy_.capital},
            {"available_cash_micro_usd", available_}, {"notional_micro_usd", notional_},
            {"fees_micro_usd", spent_ - notional_}, {"spent_micro_usd", spent_},
            {"settlement_floor_micro_usd", floor}, {"operating_cost_micro_usd", policy_.operating_cost},
            {"net_settlement_bound_micro_usd", floor - spent_ - policy_.operating_cost},
            {"realized_pnl_micro_usd", nullptr}};
        if (policy_.optimal_sizing) {
            result["sizing"] = {{"evaluated_quantities", sizing_evaluations_}, {"intervals_pruned", sizing_pruned_}};
        }
        if (policy_.lifecycle) {
            const auto unresolved = positions_.unsettled_lots();
            const auto& capital_time = positions_.capital_time();
            const auto unknown = std::count_if(orders_.begin(), orders_.end(), [](const auto& order) { return !order.responded; });
            const auto final_cash = policy_.capital - spent_ + settlement_cash_ + exit_credit_;
            std::int64_t reserved = 0;
            for (const auto& attempt : attempts_) { reserved += attempt.reserve[0U] + attempt.reserve[1U]; }
            const bool accounting_complete = unresolved == 0 && unknown == 0 && reserved == 0;
            if (reserved != reserved_total_) { detail::invalid("reservation ledger disagreement"); }
            check_cash();
            result["lifecycle"] = {{"execution_policy", policy_.lifecycle->sequential ? "sequential_complete" : "parallel_hold"},
                {"first_leg", policy_.lifecycle->reverse_legs ? 1U : 0U},
                {"orders", orders_.size()}, {"unknown_orders", unknown}, {"unsettled_lots", unresolved},
                {"reserved_micro_usd", reserved}, {"settlement_cash_micro_usd", settlement_cash_},
                {"cash_micro_usd", final_cash}, {"accounting_complete", accounting_complete},
                {"simulated_net_pnl_micro_usd", accounting_complete ? Json(final_cash - policy_.capital - policy_.operating_cost) : Json(nullptr)},
                {"capital_time_micro_usd_seconds", capital_time.micro_usd_seconds},
                {"capital_time_fractional_micro_usd_nanoseconds", capital_time.fractional_micro_usd_nanoseconds}};
        }
        if (policy_.residual_exit) {
            result["fees_micro_usd"] = spent_ - notional_ + exit_fees_;
            result["net_settlement_bound_micro_usd"] = floor - spent_ + exit_credit_ - policy_.operating_cost;
            result["residual_exit"] = {{"mode", policy_.residual_exit->reduce ? "reduce_once" : "hold"},
                {"sold_centicontracts", exit_quantity_}, {"credit_micro_usd", exit_credit_},
                {"fees_micro_usd", exit_fees_}, {"released_basis_micro_usd", exit_basis_},
                {"credit_awaiting_response_micro_usd", unconfirmed_credit_}};
        }
        emit(result);
    }
    void start(const bool live = false) override {
        emit({{"type", "study_start"}, {"schema_version", 1U}, {"policy_sha256", policy_.hash},
              {"policy", policy_.json}, {"currency", "USD"}, {"execution", live ? "live_simulated_ioc" : "offline_simulated_ioc"}});
    }

    std::optional<std::int64_t> next_event_time() const override {
        std::optional<std::int64_t> next;
        if (policy_.lifecycle) {
            if (!events_.empty()) { next = events_.top().time; }
            if (settlement_index_ < policy_.lifecycle->settlements.size()) {
                const auto time = policy_.lifecycle->settlements[settlement_index_].time;
                next = next ? std::min(*next, time) : time;
            }
        } else if (!pending_.empty()) { next = pending_.front().arrival; }
        return next;
    }
    void checkpoint(const std::int64_t time) override {
        Json holdings = Json::array();
        for (std::size_t index = 0; index < attempts_.size(); ++index) {
            const auto& attempt = attempts_[index];
            for (std::size_t leg = 0; leg < 2U; ++leg) {
                const auto quantity = positions_.quantity(index, leg);
                if (quantity != 0) { holdings.push_back({{"constraint_id", attempt.id},
                    {"market_id", attempt.legs[leg].market_id},
                    {"outcome", attempt.legs[leg].outcome == Outcome::yes ? "yes" : "no"},
                    {"quantity_centicontracts", quantity}}); }
            }
        }
        emit({{"type", "paper_status"}, {"time_ns", time}, {"attempts", attempts_.size()},
            {"decisions_evaluated", evaluated_}, {"declined", declines_}, {"orders", orders_.size()},
            {"available_cash_micro_usd", available_}, {"reserved_micro_usd", reserved_total_},
            {"spent_micro_usd", spent_}, {"holdings", holdings}, {"realized_pnl_micro_usd", nullptr}});
    }
private:
    void emit(const Json& value) { output_ << value.dump() << '\n'; }
    bool fresh(const constraint::PayoffLegTemplate leg, const std::int64_t time,
               const market::MarketState& state) const {
        const auto found = last_update_.find(leg.market_id);
        const auto* book = state.find_book(leg.market_id);
        return state.connected() && book && book->state() == book::BookState::valid &&
            found != last_update_.end() && time >= found->second && time - found->second <= policy_.max_age;
    }
    template<bool Sell = false>
    Execution walk(const constraint::PayoffLegTemplate leg, const std::int64_t quantity,
                   const std::int64_t limit, const market::MarketState& state, const bool execute) {
        Execution result;
        core::FeeAccumulator fees;
        const auto* book = state.find_book(leg.market_id);
        if (!book) { return result; }
        book->visit_levels((leg.outcome == Outcome::yes) != Sell ? book::Side::ask : book::Side::bid,
            [&](const book::Level level) {
                const auto price = leg.outcome == Outcome::yes ? level.price.raw() : 10'000 - level.price.raw();
                if (Sell ? price < limit : price > limit) { return false; }
                // Selling an outcome takes the same physical book liquidity as
                // buying its complement. Share the existing depletion key.
                const auto direction = Sell ? (leg.outcome == Outcome::yes ? Outcome::no : Outcome::yes) : leg.outcome;
                const LiquidityKey key{leg.market_id, direction, Sell ? 10'000 - price : price};
                const auto found = consumed_.find(key);
                const auto used = found == consumed_.end() ? 0 : found->second;
                auto available = level.quantity.raw() > used ? level.quantity.raw() - used : 0;
                // Cap before scaling; prevents overflow on arbitrary feed sizes.
                available = std::min(available, quantity_limit);
                if (execute) { available = available * (Sell ? policy_.residual_exit->fill_bps : policy_.fill_bps) / 10'000; }
                const auto fill = std::min(available, quantity - result.quantity) / policy_.quantity_step * policy_.quantity_step;
                if (fill == 0) { return true; }
                if constexpr (Sell) {
                    const auto credit = core::credit_sell_fill(*core::Quantity::from_raw(fill),
                        *core::Price::from_raw(price), policy_.fees.at(leg.market_id), fees);
                    if (!credit) { detail::invalid("sale credit overflow"); }
                    result.quantity += fill; result.notional += credit->notional.raw();
                    result.credit += credit->credit.raw(); result.worst_price = price; ++result.levels;
                    if (execute) {
                        if (used > std::numeric_limits<std::int64_t>::max() - fill) { detail::invalid("liquidity overflow"); }
                        consumed_[key] = used + fill;
                        emit({{"type", "sell_fill"}, {"market_id", leg.market_id}, {"outcome", leg.outcome == Outcome::yes ? "yes" : "no"},
                            {"quantity_centicontracts", fill}, {"price_1e4", price}, {"notional_micro_usd", credit->notional.raw()},
                            {"trade_fee_micro_usd", credit->trade_fee.raw()}, {"rounding_fee_micro_usd", credit->rounding_fee.raw()},
                            {"rebate_micro_usd", credit->rebate.raw()}, {"credit_micro_usd", credit->credit.raw()}});
                    }
                    return result.quantity < quantity;
                }
                const auto charge = core::charge_buy_fill(*core::Quantity::from_raw(fill),
                    *core::Price::from_raw(price), policy_.fees.at(leg.market_id), fees);
                if (!charge) { detail::invalid("fill charge overflow"); }
                result.quantity += fill;
                result.notional += charge->notional.raw();
                result.debit += charge->debit.raw();
                result.worst_price = price;
                ++result.levels;
                if (execute) {
                    if (used > std::numeric_limits<std::int64_t>::max() - fill) { detail::invalid("liquidity overflow"); }
                    consumed_[key] = used + fill;
                    emit({{"type", "fill"}, {"market_id", leg.market_id}, {"outcome", leg.outcome == Outcome::yes ? "yes" : "no"},
                        {"quantity_centicontracts", fill}, {"price_1e4", price}, {"notional_micro_usd", charge->notional.raw()},
                        {"trade_fee_micro_usd", charge->trade_fee.raw()}, {"rounding_fee_micro_usd", charge->rounding_fee.raw()},
                        {"rebate_micro_usd", charge->rebate.raw()}, {"debit_micro_usd", charge->debit.raw()}});
                }
                return result.quantity < quantity;
            });
        return result;
    }
    std::int64_t reserve(const constraint::PayoffLegTemplate leg, const std::int64_t q, const std::int64_t price) const {
        const auto value = core::buy_reservation(*core::Quantity::from_raw(q),
            *core::Quantity::from_raw(policy_.quantity_step), *core::Price::from_raw(price), policy_.fees.at(leg.market_id));
        if (!value) { detail::invalid("reservation overflow"); }
        return value->raw();
    }
    std::optional<core::SizedPair> optimal_quote(const std::array<constraint::PayoffLegTemplate, 2U>& legs,
                                               const market::MarketState& state) {
        std::array<core::BuyDepth, 2U> depth;
        for (std::size_t i = 0U; i < 2U; ++i) {
            auto& levels = sizing_depth_[i];
            levels.clear();
            std::int64_t total = 0;
            const auto leg = legs[i];
            state.find_book(leg.market_id)->visit_levels(leg.outcome == Outcome::yes ? book::Side::ask : book::Side::bid,
                [&](const book::Level level) {
                    const auto price = leg.outcome == Outcome::yes ? level.price.raw() : 10'000 - level.price.raw();
                    const auto found = consumed_.find({leg.market_id, leg.outcome, price});
                    const auto used = found == consumed_.end() ? 0 : found->second;
                    const auto available = level.quantity.raw() > used ? level.quantity.raw() - used : 0;
                    const auto take = std::min(available, policy_.quantity_cap - total) / policy_.quantity_step * policy_.quantity_step;
                    if (take != 0) {
                        levels.push_back({*core::Price::from_raw(price), *core::Quantity::from_raw(take)});
                        total += take;
                    }
                    return total < policy_.quantity_cap / policy_.quantity_step * policy_.quantity_step;
                });
            depth[i] = {levels, policy_.fees.at(leg.market_id)};
        }
        const auto result = core::size_buy_pair(depth, {*core::Quantity::from_raw(policy_.quantity_cap),
            *core::Quantity::from_raw(policy_.quantity_step), *core::Cash::from_raw(available_),
            *core::Cash::from_raw(policy_.min_margin), policy_.max_sizing_evaluations});
        if (sizing_evaluations_ > std::numeric_limits<std::uint64_t>::max() - result.evaluated_quantities ||
            sizing_pruned_ > std::numeric_limits<std::uint64_t>::max() - result.intervals_pruned) {
            detail::invalid("sizing counter overflow");
        }
        sizing_evaluations_ += result.evaluated_quantities;
        sizing_pruned_ += result.intervals_pruned;
        switch (result.status) {
        case core::SizingStatus::optimal: return result.quote;
        case core::SizingStatus::no_positive_margin: decline("non_positive_costed_margin"); break;
        case core::SizingStatus::no_depth: decline("no_depth"); break;
        case core::SizingStatus::insufficient_cash: decline("insufficient_cash"); break;
        case core::SizingStatus::search_budget_exceeded: decline("sizing_search_budget_exceeded"); break;
        default: detail::invalid("net sizing input/arithmetic invariant");
        }
        return std::nullopt;
    }
    void decline(const std::string& reason) { ++declines_[reason]; }
    std::optional<core::SizedPair> legacy_quote(const std::array<constraint::PayoffLegTemplate, 2U>& legs,
                                              const market::MarketState& state) {
        auto quantity = policy_.quantity_cap / policy_.quantity_step * policy_.quantity_step;
        const auto depth0 = walk(legs[0U], quantity, 10'000, state, false);
        const auto depth1 = walk(legs[1U], quantity, 10'000, state, false);
        quantity = std::min(depth0.quantity, depth1.quantity);
        if (quantity == 0) { decline("no_depth"); return std::nullopt; }
        const auto quotes = [&](const std::int64_t q) {
            return std::array{walk(legs[0U], q, 10'000, state, false), walk(legs[1U], q, 10'000, state, false)};
        };
        // Reuse the depth traversal when both sides reach the quantity cap.
        // Funded full size needs no binary search through the same levels.
        auto quote = std::array{
            depth0.quantity == quantity ? depth0 : walk(legs[0U], quantity, 10'000, state, false),
            depth1.quantity == quantity ? depth1 : walk(legs[1U], quantity, 10'000, state, false)};
        if (reserve(legs[0U], quantity, quote[0U].worst_price) +
            reserve(legs[1U], quantity, quote[1U].worst_price) > available_) {
            // Only funding is monotone; net profit need not be.
            std::int64_t lo = 0;
            std::int64_t hi = quantity / policy_.quantity_step;
            while (lo < hi) {
                const auto mid = lo + (hi - lo + 1) / 2;
                const auto q = mid * policy_.quantity_step;
                const auto trial = quotes(q);
                const auto needed = reserve(legs[0U], q, trial[0U].worst_price) + reserve(legs[1U], q, trial[1U].worst_price);
                if (needed <= available_) { lo = mid; } else { hi = mid - 1; }
            }
            quantity = lo * policy_.quantity_step;
            if (quantity == 0) { decline("insufficient_cash"); return std::nullopt; }
            quote = quotes(quantity);
        }
        const auto cost = quote[0U].debit + quote[1U].debit;
        if (quantity * 10'000 - cost <= policy_.min_margin) { decline("non_positive_costed_margin"); return std::nullopt; }
        return core::SizedPair{*core::Quantity::from_raw(quantity),
            {{{*core::Price::from_raw(quote[0U].worst_price), *core::Cash::from_raw(quote[0U].notional),
               *core::Cash::from_raw(quote[0U].debit), *core::Cash::from_raw(reserve(legs[0U], quantity, quote[0U].worst_price))},
              {*core::Price::from_raw(quote[1U].worst_price), *core::Cash::from_raw(quote[1U].notional),
               *core::Cash::from_raw(quote[1U].debit), *core::Cash::from_raw(reserve(legs[1U], quantity, quote[1U].worst_price))}}},
            *core::Cash::from_raw(quantity * 10'000), *core::Cash::from_raw(quantity * 10'000 - cost)};
    }
    void decide(const constraint::ConstraintId id, const ReplayFrame& frame, const market::MarketState& state) {
        ++evaluated_;
        const auto& compiled = *metadata_.constraints().find(id);
        auto templates = compiled.guaranteed_leg_templates;
        if (templates.size() != 2U) { detail::invalid("unsupported portfolio"); }
        std::sort(templates.begin(), templates.end(), [](const auto& a, const auto& b) { return a.market_id < b.market_id; });
        if (policy_.lifecycle && policy_.lifecycle->reverse_legs) { std::swap(templates[0U], templates[1U]); }
        const std::array<constraint::PayoffLegTemplate, 2U> legs{templates[0U], templates[1U]};
        if (!fresh(legs[0U], frame.time_ns, state) || !fresh(legs[1U], frame.time_ns, state)) { decline("stale_or_missing_book"); return; }
        if (policy_.lifecycle && (settled_markets_.contains(legs[0U].market_id) || settled_markets_.contains(legs[1U].market_id))) {
            decline("settled_market"); return;
        }
        const auto selected = policy_.optimal_sizing ? optimal_quote(legs, state) : legacy_quote(legs, state);
        if (!selected) { return; }
        const auto quantity = selected->quantity.raw();
        const auto cost = selected->legs[0U].debit.raw() + selected->legs[1U].debit.raw();
        Attempt attempt{id, legs, *state.connection_generation(), quantity,
            {selected->legs[0U].limit.raw(), selected->legs[1U].limit.raw()},
            {selected->legs[0U].reservation.raw(), selected->legs[1U].reservation.raw()}, {}};
        emit({{"type", "decision"}, {"record_index", frame.record_index}, {"time_ns", frame.time_ns},
              {"constraint_id", id}, {"quantity_centicontracts", quantity}, {"limits_1e4", attempt.limit},
              {"quoted_debit_micro_usd", cost}, {"quoted_net_margin_micro_usd", quantity * 10'000 - cost},
              {"reserved_micro_usd", attempt.reserve[0U] + attempt.reserve[1U]}});
        attempt.started = frame.time_ns;
        for (std::size_t leg = 0U; leg < 2U; ++leg) {
            if (frame.time_ns > std::numeric_limits<std::int64_t>::max() - policy_.latency[leg]) { detail::invalid("arrival clock overflow"); }
            if (policy_.lifecycle) {
                if (!policy_.lifecycle->sequential || leg == 0U) {
                    submit(attempts_.size(), leg, quantity, attempt.limit[leg], attempt.reserve[leg], frame.time_ns + policy_.latency[leg]);
                    ++attempt.pending_buys;
                }
            } else { pending_.push_back({frame.time_ns + policy_.latency[leg], attempts_.size(), leg}); }
            available_ -= attempt.reserve[leg];
            if (policy_.lifecycle) { reserved_total_ += attempt.reserve[leg]; }
        }
        attempted_.insert(id);
        attempts_.push_back(std::move(attempt));
        if (policy_.lifecycle) { check_cash(); }
        std::sort(pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
            return std::tie(a.arrival, a.attempt, a.leg) < std::tie(b.arrival, b.attempt, b.leg);
        });
    }
    void drain(const std::int64_t time, const bool inclusive, const market::MarketState& state) {
        if (policy_.lifecycle) { drain_lifecycle(time, inclusive, state); return; }
        std::size_t count = 0U;
        for (const auto& pending : pending_) {
            if (pending.arrival > time || (!inclusive && pending.arrival == time)) { break; }
            auto& attempt = attempts_[pending.attempt];
            auto& fill = attempt.fills[pending.leg];
            const auto leg = attempt.legs[pending.leg];
            emit({{"type", "order_arrival"}, {"constraint_id", attempt.id}, {"leg", pending.leg}, {"time_ns", pending.arrival}});
            if (!policy_.reject[pending.leg] && state.connection_generation() == attempt.generation && fresh(leg, pending.arrival, state)) {
                fill = walk(leg, attempt.quantity, attempt.limit[pending.leg], state, true);
            }
            if (fill.debit > attempt.reserve[pending.leg]) { detail::invalid("reservation invariant"); }
            available_ += attempt.reserve[pending.leg] - fill.debit;
            spent_ += fill.debit;
            notional_ += fill.notional;
            emit({{"type", "ioc_complete"}, {"constraint_id", attempt.id}, {"leg", pending.leg},
                {"filled_centicontracts", fill.quantity}, {"unfilled_centicontracts", attempt.quantity - fill.quantity},
                {"debit_micro_usd", fill.debit}});
            ++count;
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(count));
    }

    void check_cash() const {
        if (available_ < 0 || reserved_total_ < 0 || unconfirmed_debit_ < 0 || unconfirmed_credit_ < 0 ||
            available_ + reserved_total_ - unconfirmed_debit_ + unconfirmed_credit_ != policy_.capital - spent_ + settlement_cash_ + exit_credit_) {
            detail::invalid("cash conservation");
        }
    }
    void submit(const std::size_t attempt, const std::size_t leg, const std::int64_t quantity,
                const std::int64_t limit, const std::int64_t reservation, const std::int64_t arrival, const bool sell = false) {
        events_.push({arrival, orders_.size(), false});
        orders_.push_back({attempt, leg, quantity, limit, reservation, {}, false, false, sell});
        emit({{"type", sell ? "exit_intent" : "order_intent"}, {"order_id", orders_.size()}, {"attempt_index", attempt},
            {"leg", leg}, {"quantity_centicontracts", quantity}, {"limit_1e4", limit},
            {"reserved_micro_usd", reservation}, {"arrival_ns", arrival}});
    }
    void finish_acquisition(const std::size_t index, const std::string_view reason,
                       const std::int64_t time, const market::MarketState& state) {
        auto& attempt = attempts_[index];
        reserved_total_ -= attempt.reserve[1U];
        available_ += attempt.reserve[1U];
        attempt.reserve[1U] = 0;
        emit({{"type", policy_.residual_exit ? "acquisition_stopped" : "residual_hold"}, {"constraint_id", attempt.id}, {"reason", reason},
            {"unpaired_centicontracts", attempt.fills[0U].quantity - attempt.fills[1U].quantity}});
        consider_exit(index, time, state);
    }
    void complete_second(const std::size_t index, const std::int64_t time, const market::MarketState& state) {
        auto& attempt = attempts_[index];
        const auto& config = *policy_.lifecycle;
        const auto remaining = attempt.fills[0U].quantity - attempt.fills[1U].quantity;
        if (remaining <= 0) { finish_acquisition(index, "balanced", time, state); return; }
        if (time < attempt.started || time - attempt.started > config.completion_timeout ||
            policy_.latency[1U] > config.completion_timeout - (time - attempt.started) ||
            attempt.completion_orders >= config.maximum_completion_orders) { finish_acquisition(index, "completion_budget", time, state); return; }
        const auto leg = attempt.legs[1U];
        if (!fresh(leg, time, state) || state.connection_generation() != attempt.generation || settled_markets_.contains(leg.market_id)) {
            finish_acquisition(index, "unavailable_book", time, state); return;
        }
        const auto quote = walk(leg, remaining, 10'000, state, false);
        if (quote.quantity == 0) { finish_acquisition(index, "no_depth", time, state); return; }
        const auto total_cost = attempt.fills[0U].debit + attempt.fills[1U].debit + quote.debit;
        const auto matched_payout = (attempt.fills[1U].quantity + quote.quantity) * 10'000;
        if (total_cost - matched_payout > config.completion_loss_limit) { finish_acquisition(index, "completion_loss_limit", time, state); return; }
        const auto needed = reserve(leg, quote.quantity, quote.worst_price);
        if (needed > available_ + attempt.reserve[1U]) { finish_acquisition(index, "insufficient_cash", time, state); return; }
        reserved_total_ += needed - attempt.reserve[1U];
        available_ += attempt.reserve[1U] - needed;
        attempt.reserve[1U] = needed;
        ++attempt.completion_orders;
        ++attempt.pending_buys;
        if (time > std::numeric_limits<std::int64_t>::max() - policy_.latency[1U]) { detail::invalid("completion clock overflow"); }
        submit(index, 1U, quote.quantity, quote.worst_price, needed, time + policy_.latency[1U]);
    }
    void settle(const Settlement& settlement) {
        settled_markets_.insert(settlement.market_id);
        const auto closed = positions_.settle(settlement);
        const auto payout = closed.payout;
        const auto quantity = closed.quantity;
        if (payout > cash_limit - settlement_cash_ || payout > cash_limit - available_) { detail::invalid("settlement cash bound"); }
        settlement_cash_ += payout;
        available_ += payout;
        check_cash();
        emit({{"type", "simulated_settlement"}, {"market_id", settlement.market_id}, {"time_ns", settlement.time},
            {"yes_wins", settlement.yes_wins}, {"closed_centicontracts", quantity}, {"payout_micro_usd", payout}});
    }
    void consider_exit(const std::size_t index, const std::int64_t time, const market::MarketState& state) {
        if (!policy_.residual_exit) { return; }
        auto& attempt = attempts_[index];
        if (attempt.exit_considered) { detail::invalid("duplicate exit decision"); }
        if (attempt.pending_buys != 0U) { detail::invalid("exit before acquisition responses"); }
        attempt.exit_considered = true;
        const auto& config = *policy_.residual_exit;
        const auto difference = attempt.fills[0U].quantity - attempt.fills[1U].quantity;
        const std::size_t leg_index = difference > 0 ? 0U : 1U;
        const auto leg = attempt.legs[leg_index];
        const auto skip = [&](const std::string_view reason) {
            emit({{"type", "exit_declined"}, {"constraint_id", attempt.id}, {"time_ns", time}, {"reason", reason}});
        };
        if (difference == 0) { skip("balanced"); return; }
        if (!config.reduce) { skip("hold_policy"); return; }
        if (time < attempt.started || time - attempt.started > config.timeout ||
            config.latency > config.timeout - (time - attempt.started)) { skip("exit_timeout"); return; }
        if (settled_markets_.contains(leg.market_id)) { skip("settled_market"); return; }
        if (!fresh(leg, time, state) || state.connection_generation() != attempt.generation) { skip("unavailable_book"); return; }
        const auto quantity = std::min(difference > 0 ? difference : -difference, positions_.quantity(index, leg_index));
        const auto quote = walk<true>(leg, quantity, config.minimum_price, state, false);
        if (quote.quantity == 0) { skip("no_depth_at_exit_price"); return; }
        if (time > std::numeric_limits<std::int64_t>::max() - config.latency) { detail::invalid("exit clock overflow"); }
        // All buys have responded, this attempt gets one exit, and only its
        // unmatched holdings are eligible. No short sale or paired-leg unwind.
        submit(index, leg_index, quote.quantity, quote.worst_price, 0, time + config.latency, true);
    }
    void arrive_exit(LifecycleOrder& order, const std::size_t order_id, const std::int64_t time, const market::MarketState& state) {
        const auto& attempt = attempts_[order.attempt];
        const auto leg = attempt.legs[order.leg];
        if (!policy_.residual_exit->reject && !settled_markets_.contains(leg.market_id) &&
            state.connection_generation() == attempt.generation && fresh(leg, time, state)) {
            const auto owned = positions_.quantity(order.attempt, order.leg);
            if (owned != 0) { order.fill = walk<true>(leg, std::min(owned, order.quantity), order.limit, state, true); }
        }
        const auto& fill = order.fill;
        const auto fee = fill.notional - fill.credit;
        if (fill.credit > cash_limit - exit_credit_ || fee > cash_limit - exit_fees_) { detail::invalid("exit cash bound"); }
        const auto basis = positions_.close(order.attempt, order.leg, fill.quantity, time);
        exit_credit_ += fill.credit; exit_fees_ += fee; exit_basis_ += basis; exit_quantity_ += fill.quantity;
        unconfirmed_credit_ += fill.credit;
        emit({{"type", "exit_execution"}, {"order_id", order_id},
            {"time_ns", time}, {"sold_centicontracts", fill.quantity}, {"released_basis_micro_usd", basis}, {"credit_micro_usd", fill.credit}});
        check_cash();
    }
    void drain_lifecycle(const std::int64_t time, const bool inclusive, const market::MarketState& state) {
        const auto due = [&](const std::int64_t at) { return at < time || (inclusive && at == time); };
        const auto& settlements = policy_.lifecycle->settlements;
        for (;;) {
            const bool settlement_due = settlement_index_ < settlements.size() && settlements[settlement_index_].time <= time;
            const bool order_due = !events_.empty() && due(events_.top().time);
            if (!settlement_due && !order_due) { break; }
            // Terminal settlement wins ties: no assumed trading after resolution.
            if (settlement_due && (!order_due || settlements[settlement_index_].time <= events_.top().time)) {
                settle(settlements[settlement_index_++]); continue;
            }
            const auto event = events_.top(); events_.pop();
            auto& order = orders_[event.order];
            auto& attempt = attempts_[order.attempt];
            if (event.response) {
                if (!order.arrived || order.responded) { detail::invalid("response lifecycle"); }
                order.responded = true;
                if (order.sell) {
                    if (order.fill.credit > cash_limit - available_) { detail::invalid("available exit cash bound"); }
                    unconfirmed_credit_ -= order.fill.credit;
                    available_ += order.fill.credit;
                    emit({{"type", "exit_response"}, {"order_id", event.order + 1U}, {"time_ns", event.time},
                        {"sold_centicontracts", order.fill.quantity}, {"cancelled_centicontracts", order.quantity - order.fill.quantity},
                        {"credit_micro_usd", order.fill.credit}});
                    check_cash(); continue;
                }
                if (attempt.pending_buys == 0U) { detail::invalid("acquisition response count"); }
                --attempt.pending_buys;
                reserved_total_ -= order.reserve;
                unconfirmed_debit_ -= order.fill.debit;
                available_ += order.reserve - order.fill.debit;
                attempt.reserve[order.leg] = 0;
                emit({{"type", "order_response"}, {"order_id", event.order + 1U}, {"time_ns", event.time},
                    {"filled_centicontracts", order.fill.quantity}, {"cancelled_centicontracts", order.quantity - order.fill.quantity},
                    {"debit_micro_usd", order.fill.debit}});
                if (policy_.lifecycle->sequential) { complete_second(order.attempt, event.time, state); }
                else if (attempt.pending_buys == 0U) { consider_exit(order.attempt, event.time, state); }
                check_cash();
                continue;
            }
            if (order.arrived) { detail::invalid("duplicate order arrival"); }
            order.arrived = true;
            const auto leg = attempt.legs[order.leg];
            emit({{"type", order.sell ? "exit_arrival" : "order_arrival"}, {"order_id", event.order + 1U}, {"constraint_id", attempt.id}, {"leg", order.leg}, {"time_ns", event.time}});
            if (order.sell) {
                arrive_exit(order, event.order + 1U, event.time, state);
                const auto latency = policy_.residual_exit->response_latency;
                if (event.time > std::numeric_limits<std::int64_t>::max() - latency) { detail::invalid("exit response clock overflow"); }
                events_.push({event.time + latency, event.order, true});
                continue;
            }
            if (!policy_.reject[order.leg] && !settled_markets_.contains(leg.market_id) &&
                state.connection_generation() == attempt.generation && fresh(leg, event.time, state)) {
                order.fill = walk(leg, order.quantity, order.limit, state, true);
            }
            const auto& fill = order.fill;
            if (fill.debit > order.reserve || fill.debit > cash_limit - spent_) { detail::invalid("lifecycle cash reservation"); }
            auto& aggregate = attempt.fills[order.leg];
            aggregate.quantity += fill.quantity; aggregate.notional += fill.notional; aggregate.debit += fill.debit;
            aggregate.worst_price = std::max(aggregate.worst_price, fill.worst_price); aggregate.levels += fill.levels;
            spent_ += fill.debit; notional_ += fill.notional;
            unconfirmed_debit_ += fill.debit;
            check_cash();
            if (fill.quantity != 0) { positions_.acquire(order.attempt, order.leg, leg, fill.quantity, fill.debit, event.time); }
            const auto latency = policy_.lifecycle->response_latency[order.leg];
            if (event.time > std::numeric_limits<std::int64_t>::max() - latency) { detail::invalid("response clock overflow"); }
            events_.push({event.time + latency, event.order, true});
        }
    }

    const gateway::kalshi::MetadataSnapshot& metadata_;
    Policy policy_;
    std::ostream& output_;
    std::int64_t available_{};
    std::int64_t spent_{};
    std::int64_t notional_{};
    std::map<market::MarketId, std::int64_t> last_update_;
    std::map<LiquidityKey, std::int64_t> consumed_;
    std::array<std::vector<core::BuyLevel>, 2U> sizing_depth_;
    std::vector<Attempt> attempts_;
    std::set<constraint::ConstraintId> attempted_;
    std::vector<Pending> pending_;
    std::map<std::string, std::uint64_t> declines_;
    std::uint64_t evaluated_{};
    std::uint64_t unobserved_{};
    std::uint64_t sizing_evaluations_{};
    std::uint64_t sizing_pruned_{};
    std::vector<LifecycleOrder> orders_;
    std::priority_queue<LifecycleEvent> events_;
    detail::StudyPositions positions_;
    std::set<market::MarketId> settled_markets_;
    std::size_t settlement_index_{};
    std::int64_t settlement_cash_{};
    std::int64_t reserved_total_{}, unconfirmed_debit_{};
    std::int64_t exit_credit_{}, exit_fees_{}, exit_quantity_{}, exit_basis_{}, unconfirmed_credit_{};
};
}  // namespace

std::unique_ptr<ExecutionSimulation> make_execution_simulation(
    const gateway::kalshi::MetadataSnapshot& metadata, const std::filesystem::path& path,
    std::ostream& output, const bool live) {
    auto policy = load_policy(path, metadata);
    // Future settlement labels must never enter a prospective simulation.
    if (live && (!policy.lifecycle || !policy.lifecycle->settlements.empty())) {
        detail::invalid("live paper requires lifecycle policy without settlement labels");
    }
    return std::make_unique<Simulation>(metadata, std::move(policy), output);
}

std::optional<ReplayError> run_execution_study(const ReplayInput& input,
    const std::filesystem::path& policy_path, std::ostream& output) {
    try {
        Simulation simulation{input.session.metadata, load_policy(policy_path, input.session.metadata), output};
        simulation.start();
        const auto result = replay(input, simulation, &output);
        if (const auto* failure = std::get_if<ReplayError>(&result)) { return *failure; }
        simulation.report(input.plan);
        if (!output) { return ReplayError{"output write failed", 0U}; }
        return std::nullopt;
    } catch (const ReplayError& failure) { return failure; }
    catch (const Json::exception&) { return ReplayError{"invalid policy JSON", 0U}; }
}
}  // namespace eme::session
