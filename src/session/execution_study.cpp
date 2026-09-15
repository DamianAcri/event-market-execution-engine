#include "eme/session/execution_study.hpp"
#include "eme/core/execution_cost.hpp"
#include "eme/core/net_sizing.hpp"
#include "study_json.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <ostream>
#include <set>
#include <tuple>

namespace eme::session {
namespace {
using detail::Json;
using Outcome = constraint::ContractOutcome;
constexpr std::int64_t cash_limit = 1'000'000'000'000'000LL;
constexpr std::int64_t quantity_limit = 100'000'000LL;

struct Policy final {
    bool optimal_sizing{};
    std::uint64_t max_sizing_evaluations{};
    std::int64_t capital{};
    std::int64_t operating_cost{};
    std::int64_t quantity_cap{};
    std::int64_t quantity_step{};
    std::int64_t min_margin{};
    std::int64_t max_age{};
    std::array<std::int64_t, 2U> latency{};
    std::array<bool, 2U> reject{};
    std::int64_t fill_bps{};
    std::map<market::MarketId, core::FeePolicy> fees;
    Json json;
    std::string hash;
};

Policy load_policy(const std::filesystem::path& path, const ReplayInput& input) {
    const auto bytes = detail::read_text(path);
    const auto root = detail::parse_strict(bytes);
    const auto version = detail::integer(root, "schema_version");
    const bool optimal = version == 2U && root.value("strategy", "") == "one_attempt_net_profit_v2";
    if (!optimal && (version != 1U || root.value("strategy", "") != "one_attempt_per_constraint_v1")) {
        detail::invalid("policy schema/strategy");
    }
    auto shape = root;
    if (optimal) { shape.erase("max_sizing_evaluations"); }
    detail::shape(shape, {"schema_version", "strategy", "fee_provenance", "capital_micro_usd", "operating_cost_micro_usd",
        "quantity_cap_centicontracts", "quantity_step_centicontracts", "min_margin_micro_usd", "max_book_age_ns",
        "leg_latency_ns", "reject_legs", "available_liquidity_bps", "fees"});
    (void)detail::string(root, "fee_provenance");
    Policy policy;
    policy.optimal_sizing = optimal;
    if (optimal) {
        policy.max_sizing_evaluations = detail::integer(root, "max_sizing_evaluations", 1'000'000U);
        if (policy.max_sizing_evaluations == 0U) { detail::invalid("sizing evaluation budget"); }
    }
    policy.capital = static_cast<std::int64_t>(detail::integer(root, "capital_micro_usd", cash_limit));
    policy.operating_cost = static_cast<std::int64_t>(detail::integer(root, "operating_cost_micro_usd", cash_limit));
    policy.quantity_cap = static_cast<std::int64_t>(detail::integer(root, "quantity_cap_centicontracts", quantity_limit));
    policy.quantity_step = static_cast<std::int64_t>(detail::integer(root, "quantity_step_centicontracts", 100U));
    policy.min_margin = static_cast<std::int64_t>(detail::integer(root, "min_margin_micro_usd", cash_limit));
    policy.max_age = static_cast<std::int64_t>(detail::integer(root, "max_book_age_ns", cash_limit));
    policy.fill_bps = static_cast<std::int64_t>(detail::integer(root, "available_liquidity_bps", 10'000U));
    if (policy.capital == 0 || policy.quantity_cap == 0 ||
        (policy.quantity_step != 1 && policy.quantity_step != 100) || policy.quantity_cap < policy.quantity_step) {
        detail::invalid("policy bounds/grid");
    }
    if (!root["leg_latency_ns"].is_array() || root["leg_latency_ns"].size() != 2U ||
        !root["reject_legs"].is_array() || root["reject_legs"].size() != 2U) { detail::invalid("leg policy"); }
    for (std::size_t leg = 0U; leg < 2U; ++leg) {
        policy.latency[leg] = static_cast<std::int64_t>(detail::integer(Json{{"latency", root["leg_latency_ns"][leg]}}, "latency", cash_limit));
        if (!root["reject_legs"][leg].is_boolean()) { detail::invalid("reject_legs"); }
        policy.reject[leg] = root["reject_legs"][leg].get<bool>();
    }
    if (!root["fees"].is_array() || root["fees"].size() > input.session.metadata.markets().size()) { detail::invalid("fees"); }
    for (const auto& item : root["fees"]) {
        detail::shape(item, {"market_id", "coefficient_ppm", "balance_quantum_micro"});
        const auto id = static_cast<market::MarketId>(detail::integer(item, "market_id", std::numeric_limits<market::MarketId>::max()));
        const core::FeePolicy fee{static_cast<std::uint32_t>(detail::integer(item, "coefficient_ppm", 1'000'000U)),
            static_cast<std::uint32_t>(detail::integer(item, "balance_quantum_micro", 10'000U))};
        if (!input.session.metadata.markets().find(id) ||
            (fee.balance_quantum_micro != 100U && fee.balance_quantum_micro != 10'000U) ||
            !policy.fees.emplace(id, fee).second) { detail::invalid("fee market/quantum"); }
    }
    for (const auto id : input.session.metadata.constraints().sorted_ids()) {
        for (const auto market : input.session.metadata.constraints().find(id)->dependent_markets) {
            if (!policy.fees.contains(market)) { detail::invalid("missing market fee policy"); }
        }
    }
    policy.json = root;
    policy.hash = detail::fingerprint_bytes(bytes).sha256;
    return policy;
}

// The ledger uses microdollars/centicontracts explicitly. Its input caps prove
// aggregate cash <= 1e15 and any single position <= 1e8 centicontracts.
struct Execution final {
    std::int64_t quantity{};
    std::int64_t notional{};
    std::int64_t debit{};
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
};
struct Pending final {
    std::int64_t arrival{};
    std::size_t attempt{};
    std::size_t leg{};
};

class Simulation final : public ReplayObserver {
public:
    Simulation(const ReplayInput& input, Policy policy, std::ostream& output)
        : input_{input}, policy_{std::move(policy)}, output_{output}, available_{policy_.capital} {}

    void before(const std::int64_t time, const market::MarketState& state) override {
        // All observations at an equal timestamp precede simulated arrivals.
        drain(time, false, state);
    }
    void after(const ReplayFrame& frame, std::span<const opportunity::CandidateEvent>,
               const market::MarketState& state) override {
        if (frame.market_id && frame.applied) { last_update_[*frame.market_id] = frame.time_ns; }
        if (!frame.market_id || !frame.applied) { return; }
        auto dependencies = input_.session.metadata.constraints().dependencies(*frame.market_id);
        std::vector<constraint::ConstraintId> ordered(dependencies.begin(), dependencies.end());
        std::sort(ordered.begin(), ordered.end());
        for (const auto id : ordered) {
            if (!attempted_.contains(id)) { decide(id, frame, state); }
        }
    }
    void finish(const std::int64_t time, const market::MarketState& state) override {
        drain(time, true, state);
        for (const auto& pending : pending_) {
            auto& attempt = attempts_[pending.attempt];
            available_ += attempt.reserve[pending.leg];
            emit({{"type", "unobserved_order"}, {"constraint_id", attempt.id}, {"leg", pending.leg},
                  {"arrival_ns", pending.arrival}, {"observation_end_ns", time}});
            ++unobserved_;
        }
        pending_.clear();
    }
    void report() {
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
            {"manifest_sha256", input_.plan.manifest_sha256}, {"plan_sha256", input_.plan.plan_sha256},
            {"source_kind", input_.plan.source_kind}, {"strategy", policy_.json["strategy"]},
            {"evidence_status", input_.plan.source_kind == "synthetic" ? "synthetic_validation_only" : "observational_simulation_not_profitability_proof"},
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
        emit(result);
    }
    void start() {
        emit({{"type", "study_start"}, {"schema_version", 1U}, {"policy_sha256", policy_.hash},
              {"policy", policy_.json}, {"currency", "USD"}, {"execution", "offline_simulated_ioc"}});
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
    Execution walk(const constraint::PayoffLegTemplate leg, const std::int64_t quantity,
                   const std::int64_t limit, const market::MarketState& state, const bool execute) {
        Execution result;
        core::FeeAccumulator fees;
        const auto* book = state.find_book(leg.market_id);
        if (!book) { return result; }
        book->visit_levels(leg.outcome == Outcome::yes ? book::Side::ask : book::Side::bid,
            [&](const book::Level level) {
                const auto price = leg.outcome == Outcome::yes ? level.price.raw() : 10'000 - level.price.raw();
                if (price > limit) { return false; }
                const LiquidityKey key{leg.market_id, leg.outcome, price};
                const auto found = consumed_.find(key);
                const auto used = found == consumed_.end() ? 0 : found->second;
                auto available = level.quantity.raw() > used ? level.quantity.raw() - used : 0;
                // Cap before scaling; prevents overflow on arbitrary feed sizes.
                available = std::min(available, quantity_limit);
                if (execute) { available = available * policy_.fill_bps / 10'000; }
                const auto fill = std::min(available, quantity - result.quantity) / policy_.quantity_step * policy_.quantity_step;
                if (fill == 0) { return true; }
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
        const auto& compiled = *input_.session.metadata.constraints().find(id);
        auto templates = compiled.guaranteed_leg_templates;
        if (templates.size() != 2U) { detail::invalid("unsupported portfolio"); }
        std::sort(templates.begin(), templates.end(), [](const auto& a, const auto& b) { return a.market_id < b.market_id; });
        const std::array<constraint::PayoffLegTemplate, 2U> legs{templates[0U], templates[1U]};
        if (!fresh(legs[0U], frame.time_ns, state) || !fresh(legs[1U], frame.time_ns, state)) { decline("stale_or_missing_book"); return; }
        const auto selected = policy_.optimal_sizing ? optimal_quote(legs, state) : legacy_quote(legs, state);
        if (!selected) { return; }
        const auto quantity = selected->quantity.raw();
        const auto cost = selected->legs[0U].debit.raw() + selected->legs[1U].debit.raw();
        Attempt attempt{id, legs, *state.connection_generation(), quantity,
            {selected->legs[0U].limit.raw(), selected->legs[1U].limit.raw()},
            {selected->legs[0U].reservation.raw(), selected->legs[1U].reservation.raw()}, {}};
        for (std::size_t leg = 0U; leg < 2U; ++leg) {
            if (frame.time_ns > std::numeric_limits<std::int64_t>::max() - policy_.latency[leg]) { detail::invalid("arrival clock overflow"); }
            pending_.push_back({frame.time_ns + policy_.latency[leg], attempts_.size(), leg});
            available_ -= attempt.reserve[leg];
        }
        emit({{"type", "decision"}, {"record_index", frame.record_index}, {"time_ns", frame.time_ns},
              {"constraint_id", id}, {"quantity_centicontracts", quantity}, {"limits_1e4", attempt.limit},
              {"quoted_debit_micro_usd", cost}, {"quoted_net_margin_micro_usd", quantity * 10'000 - cost},
              {"reserved_micro_usd", attempt.reserve[0U] + attempt.reserve[1U]}});
        attempted_.insert(id);
        attempts_.push_back(std::move(attempt));
        std::sort(pending_.begin(), pending_.end(), [](const auto& a, const auto& b) {
            return std::tie(a.arrival, a.attempt, a.leg) < std::tie(b.arrival, b.attempt, b.leg);
        });
    }
    void drain(const std::int64_t time, const bool inclusive, const market::MarketState& state) {
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

    const ReplayInput& input_;
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
};
}  // namespace

std::optional<ReplayError> run_execution_study(const ReplayInput& input,
    const std::filesystem::path& policy_path, std::ostream& output) {
    try {
        Simulation simulation{input, load_policy(policy_path, input), output};
        simulation.start();
        const auto result = replay(input, simulation, &output);
        if (const auto* failure = std::get_if<ReplayError>(&result)) { return *failure; }
        simulation.report();
        if (!output) { return ReplayError{"output write failed", 0U}; }
        return std::nullopt;
    } catch (const ReplayError& failure) { return failure; }
    catch (const Json::exception&) { return ReplayError{"invalid policy JSON", 0U}; }
}
}  // namespace eme::session
