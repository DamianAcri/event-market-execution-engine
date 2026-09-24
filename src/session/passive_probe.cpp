#include "eme/session/passive_probe.hpp"
#include "eme/session/basket_observation.hpp"
#include "eme/core/basket_sizing.hpp"
#include "eme/core/passive_queue.hpp"
#include "study_json.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <map>
#include <ostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace eme::session {
namespace {
using detail::Json;
using I = std::int64_t;
using Id = market::MarketId;
constexpr I ms = 1'000'000;
struct Cached {
    core::FeePolicy fee;
    std::array<std::vector<core::BuyLevel>, 2> depth;
    std::array<I, 2> bid{-1, -1}, queue{}, last_trade{-1, -1};
    I updated{-1}; bool valid{};
};
struct Basket { std::uint32_t id; std::array<Id, 3> ids; };
struct Entry { std::size_t basket{}, leg{}; I price{}, quantity{}, signal{}, activation{}, expiry{}, wall{}, predicted{}; };
struct Scenario {
    bool favorable{}, done{}, closed{}, halted{};
    I delay{}, cash{}, filled{}, debit{}, close_time{}, conditional_margin{}, floor{}, spent{}, residual{};
    std::array<I, 3> holdings{};
    std::array<bool, 3> hedged{};
    std::optional<core::PassiveQueue> queue;
    core::FeeAccumulator accumulator;
    std::uint64_t fills{}, completed{}, empty{}, censored{};
};
struct Cost { I quantity{}, debit{}; };
Cost buy(const std::vector<core::BuyLevel>& levels, core::FeePolicy fee, I desired) {
    Cost out; core::FeeAccumulator accumulator;
    for (const auto& level : levels) {
        const auto quantity = std::min(desired - out.quantity, level.quantity.raw());
        if (!quantity) { break; }
        const auto charged = core::charge_buy_fill(*core::Quantity::from_raw(quantity), level.price, fee, accumulator);
        if (!charged) { detail::invalid("passive hedge arithmetic"); }
        out.quantity += quantity; out.debit += charged->debit.raw();
    }
    return out;
}

class Probe final : public PassiveProbe {
public:
    Probe(const gateway::kalshi::MetadataSnapshot& metadata, const std::filesystem::path& basket_path,
          const std::filesystem::path& probe_path, std::ostream& output) : output_{output} {
        std::ostringstream ignored;
        // Share the production rule, cohort, fee and metadata validation boundary.
        const auto reference = make_basket_observation(metadata, basket_path, ignored);
        const auto basket_bytes = detail::read_text(basket_path, 32U * 1024U * 1024U);
        const auto bytes = detail::read_text(probe_path);
        policy_hash_ = detail::fingerprint_bytes(bytes).sha256;
        basket_hash_ = detail::fingerprint_bytes(basket_bytes).sha256;
        const auto p = detail::parse_strict(bytes), b = detail::parse_strict(basket_bytes);
        detail::shape(p, {"schema_version", "kind", "basket_policy_sha256", "entry_delay_ms", "rest_ms",
            "cancel_delay_ms", "hedge_delays_ms", "reconcile_ms", "max_gap_ms", "trade_lookback_ms",
            "max_spread_1e4", "minimum_margin_micro", "maker_coefficient_ppm", "max_attempts", "assumed_clock_error_ms"});
        if (detail::integer(p, "schema_version", 1) != 1 || p.at("kind") != "passive_basket_probe" ||
            p.at("basket_policy_sha256") != basket_hash_) { detail::invalid("passive policy binding"); }
        const auto duration = [&](const char* key, std::uint64_t maximum) {
            const auto value = detail::integer(p, key, maximum);
            if (!value) { detail::invalid(key); }
            return static_cast<I>(value) * ms;
        };
        entry_delay_ = duration("entry_delay_ms", 10000); rest_ = duration("rest_ms", 60000);
        cancel_delay_ = duration("cancel_delay_ms", 10000); reconcile_ = duration("reconcile_ms", 5000);
        max_gap_ = duration("max_gap_ms", 15000); lookback_ = duration("trade_lookback_ms", 300000);
        clock_error_ = static_cast<I>(detail::integer(p, "assumed_clock_error_ms", 5000)) * ms;
        spread_ = static_cast<I>(detail::integer(p, "max_spread_1e4", 10000));
        margin_ = static_cast<I>(detail::integer(p, "minimum_margin_micro", 1000000));
        maker_ = static_cast<std::uint32_t>(detail::integer(p, "maker_coefficient_ppm", 1000000));
        max_attempts_ = detail::integer(p, "max_attempts", 10000);
        if (!max_attempts_ || !spread_ || !margin_) { detail::invalid("passive limits"); }
        from_ = b.at("valid_from_unix_ms").get<I>() * ms;
        until_ = b.at("valid_until_unix_ms").get<I>() * ms;
        const auto& screen = b.at("screen");
        cap_ = screen.at("sizing").at("cap_centicontracts").get<I>();
        initial_cash_ = screen.at("sizing").at("available_cash_micro").get<I>();
        for (const auto& row : screen.at("fees")) {
            markets_[row.at("market_id").get<Id>()].fee = {
                row.at("coefficient_ppm").get<std::uint32_t>(), row.at("balance_quantum_micro").get<std::uint32_t>()};
        }
        for (const auto& row : screen.at("baskets")) {
            Basket basket{row.at("id").get<std::uint32_t>(), {row.at("lower_market_id").get<Id>(),
                row.at("upper_market_id").get<Id>(), row.at("range_market_id").get<Id>()}};
            for (auto id : basket.ids) { dependencies_[id].push_back(baskets_.size()); }
            baskets_.push_back(basket);
        }
        const auto& delays = p.at("hedge_delays_ms");
        if (!delays.is_array() || delays.empty() || delays.size() > 3) { detail::invalid("hedge delay grid"); }
        std::unordered_set<I> unique;
        for (const auto& delay : delays) {
            if (!delay.is_number_unsigned()) { detail::invalid("hedge delay"); }
            const auto value = delay.get<I>();
            if (value < 1 || value > 10000 || !unique.insert(value).second) { detail::invalid("hedge delay"); }
            for (bool favorable : {false, true}) {
                Scenario s; s.favorable = favorable; s.delay = value * ms; s.cash = initial_cash_;
                scenarios_.push_back(std::move(s));
            }
        }
        emit({{"type", "passive_start"}, {"policy_sha256", policy_hash_}, {"basket_policy_sha256", basket_hash_},
            {"orders_sent", 0}, {"execution", "offline_counterfactual_no_orders"},
            {"maker_fee_verified", false}, {"production_certificate", false}, {"policy", p}});
    }

    void before(I time, const market::MarketState& state) override {
        if (time < 0 || (last_time_ >= 0 && time < last_time_)) { detail::invalid("passive clock regression"); }
        if (last_time_ >= 0 && time - last_time_ > max_gap_) {
            censor(time, "record_gap"); clear(); continuity_failed_ = halted_ = true;
        }
        if (!state.connected() || state.connection_generation() != generation_) { censor(time, "connection_change"); clear(); }
        if (deadline_ && time >= *deadline_) { censor(*deadline_, "policy_expired"); expired_ = true; }
        last_time_ = time;
    }

    void after(const ReplayFrame& frame, std::span<const opportunity::CandidateEvent>, const market::MarketState& state) override {
        ++records_;
        if (frame.trade) { ++total_trades_; }
        if (!frame.observed_wall_ns || *frame.observed_wall_ns < from_) { detail::invalid("passive wall binding"); }
        if (last_wall_ && (*frame.observed_wall_ns < *last_wall_ || *frame.observed_wall_ns - *last_wall_ > max_gap_)) {
            censor(frame.time_ns, "wall_clock_discontinuity"); clear(); continuity_failed_ = halted_ = true;
        }
        last_wall_ = frame.observed_wall_ns;
        if (*frame.observed_wall_ns >= until_) { censor(frame.time_ns, "policy_expired"); expired_ = true; }
        else {
            const auto remaining = until_ - *frame.observed_wall_ns;
            if (frame.time_ns > std::numeric_limits<I>::max() - remaining) { detail::invalid("passive deadline overflow"); }
            deadline_ = deadline_ ? std::min(*deadline_, frame.time_ns + remaining) : frame.time_ns + remaining;
        }
        if (!state.connected() || generation_ != state.connection_generation()) {
            censor(frame.time_ns, "connection_change"); clear(); generation_ = state.connection_generation();
        }
        if (expired_ || continuity_failed_ || !state.connected()) { return; }
        if (frame.market_id && markets_.contains(*frame.market_id)) {
            const auto* book = state.find_book(*frame.market_id);
            if (!book || book->state() != book::BookState::valid ||
                (book->best_bid() && book->best_ask() && book->best_bid()->raw() > book->best_ask()->raw())) {
                markets_.at(*frame.market_id).valid = false;
                censor(frame.time_ns, "invalid_book"); return;
            }
        }
        // Validate this record's wall clock before advancing timers: monotonic
        // clocks can pause during sleep. Costs still use the cache from earlier
        // records, before applying this record's book update (no look-ahead).
        advance(frame.time_ns, state);
        if (frame.market_id && markets_.contains(*frame.market_id)) {
            auto& cache = markets_.at(*frame.market_id);
            const auto* book = state.find_book(*frame.market_id);
            if (!book || book->state() != book::BookState::valid ||
                (book->best_bid() && book->best_ask() && book->best_bid()->raw() > book->best_ask()->raw())) {
                cache.valid = false;
                if (entry_) { censor(frame.time_ns, "invalid_book"); }
            } else if (frame.applied) {
                update(cache, *book, frame.time_ns);
                if (entry_ && baskets_[entry_->basket].ids[entry_->leg] == *frame.market_id) {
                    const auto quantity = book->quantity_at(entry_->leg == 0 ? book::Side::bid : book::Side::ask,
                        *core::Price::from_raw(entry_->leg == 0 ? entry_->price : 10000 - entry_->price)).raw();
                    for (auto& s : scenarios_) { if (s.queue && !s.closed && !s.done) { s.queue->depth(frame.time_ns, quantity); } }
                }
            }
        }
        if (frame.trade) { on_trade(frame); }
        if (!entry_ && !halted_ && attempts_ < max_attempts_ && frame.applied && frame.market_id) { select(frame); }
    }

    void finish(I time, const market::MarketState&) override {
        // Never extrapolate missing end-of-file prices or fills into the future.
        censor(time, "end_of_observation"); finished_ = true;
    }
    void report(const ReplayPlan& plan) override {
        if (!finished_) { detail::invalid("passive report before finish"); }
        Json rows = Json::array();
        for (std::size_t i = 0; i < scenarios_.size(); ++i) {
            const auto& s = scenarios_[i];
            rows.push_back({{"scenario", i}, {"queue_model", s.favorable ? "unmatched_reductions_ahead_sensitivity" : "trades_only_priority"},
                {"hedge_leg_delay_ns", s.delay}, {"modeled_fill_events", s.fills}, {"completed_baskets", s.completed},
                {"unfilled_attempts", s.empty}, {"censored_attempts", s.censored}, {"cash_remaining_micro", s.cash},
                {"cash_spent_micro", s.spent}, {"conditional_payout_floor_micro", s.floor},
                {"conditional_margin_micro", s.conditional_margin}, {"unresolved_inventory_centicontracts", s.residual},
                {"halted_on_residual", s.halted}});
        }
        emit({{"type", "passive_complete"}, {"policy_sha256", policy_hash_}, {"basket_policy_sha256", basket_hash_},
            {"manifest_sha256", plan.manifest_sha256}, {"source_kind", plan.source_kind}, {"records", records_},
            {"public_trades_in_capture", total_trades_}, {"public_trades_analyzed", public_trades_},
            {"excluded_trades", excluded_trades_}, {"trade_exclusion_reasons", exclusions_}, {"attempts", attempts_},
            {"receive_minus_exchange_ms_min", offset_min_ ? Json(*offset_min_) : Json(nullptr)},
            {"receive_minus_exchange_ms_max", offset_max_ ? Json(*offset_max_) : Json(nullptr)},
            {"clock_error_bound_verified", false},
            {"attempt_budget_reached", attempts_ == max_attempts_}, {"candidate_sizes_evaluated", sizes_},
            {"rejected_without_recent_trade", no_trade_}, {"rejected_spread", wide_},
            {"aggressive_positive_states", aggressive_}, {"continuity_failed", continuity_failed_}, {"policy_expired", expired_},
            {"scenarios", rows}, {"orders_sent", 0}, {"simulated_fills", true}, {"own_fills_observed", false},
            {"realized_pnl_micro", nullptr}, {"scenario_results_are_additive", false},
            {"fill_model_calibrated", false}, {"counterfactual_market_impact_modeled", false}});
    }
private:
    void emit(const Json& value) {
        if (++output_records_ > 100000) { detail::invalid("passive trace budget"); }
        output_ << value.dump() << '\n'; if (!output_) { detail::invalid("passive output failure"); }
    }
    void clear() { for (auto& [id, m] : markets_) { (void)id; m.valid = false; m.last_trade = {-1, -1}; } }
    void update(Cached& m, const book::OrderBook& book, I time) {
        m.valid = true; m.updated = time;
        for (std::size_t side = 0; side < 2; ++side) {
            auto& levels = m.depth[side]; levels.clear(); I used = 0;
            book.visit_levels(side == 0 ? book::Side::ask : book::Side::bid, [&](book::Level level) {
                const auto q = std::min(cap_ - used, level.quantity.raw());
                if (q) { levels.push_back({*core::Price::from_raw(side == 0 ? level.price.raw() : 10000 - level.price.raw()), *core::Quantity::from_raw(q)}); used += q; }
                return used < cap_;
            });
            const auto bid = side == 0 ? book.best_bid() : book.best_ask();
            m.bid[side] = bid ? (side == 0 ? bid->raw() : 10000 - bid->raw()) : -1;
            m.queue[side] = bid ? book.quantity_at(side == 0 ? book::Side::bid : book::Side::ask, *bid).raw() : 0;
        }
    }
    void select(const ReplayFrame& frame) {
        std::optional<Entry> selected; I best = 0;
        I available = initial_cash_; for (const auto& s : scenarios_) { available = std::min(available, s.cash); }
        if (available <= 0) { return; }
        for (auto index : dependencies_.at(*frame.market_id)) {
            const auto& b = baskets_[index]; bool valid = true;
            std::array<core::BuyDepth, 3> depth;
            for (std::size_t leg = 0; leg < 3; ++leg) {
                const auto& m = markets_.at(b.ids[leg]);
                valid = valid && m.valid && m.updated > last_attempt_end_;
                depth[leg] = {m.depth[leg == 0 ? 0 : 1], m.fee};
            }
            if (!valid) { continue; }
            const core::SizingLimits limits{*core::Quantity::from_raw(cap_), *core::Quantity::from_raw(100),
                *core::Cash::from_raw(available), *core::Cash::from_raw(margin_), 100};
            const auto immediate = core::size_buy_basket(depth, limits);
            if (immediate.quote) { ++aggressive_; }
            for (std::size_t leg = 0; leg < 3; ++leg) {
                const auto& m = markets_.at(b.ids[leg]); const auto side = leg == 0 ? 0U : 1U;
                if (m.bid[side] <= 0 || m.depth[side].empty() || m.bid[side] >= m.depth[side][0].price.raw() ||
                    m.depth[side][0].price.raw() - m.bid[side] > spread_) { ++wide_; continue; }
                if (m.last_trade[side] < 0 || frame.time_ns - m.last_trade[side] > lookback_) { ++no_trade_; continue; }
                const std::array<core::BuyLevel, 1> resting{{{*core::Price::from_raw(m.bid[side]), *core::Quantity::from_raw(cap_)}}};
                auto hypothetical = depth; hypothetical[leg] = {resting, {maker_, m.fee.balance_quantum_micro}};
                const auto sized = core::size_buy_basket(hypothetical, limits); sizes_ += sized.evaluated_quantities;
                if (sized.status == core::SizingStatus::search_budget_exceeded || sized.status == core::SizingStatus::arithmetic_error ||
                    sized.status == core::SizingStatus::invalid_input) { detail::invalid("passive sizing incomplete"); }
                if (sized.quote && sized.quote->net_margin_micro > best) {
                    best = sized.quote->net_margin_micro;
                    selected = Entry{index, leg, m.bid[side], sized.quote->quantity.raw(), frame.time_ns,
                        frame.time_ns + entry_delay_, frame.time_ns + entry_delay_ + rest_ + cancel_delay_,
                        *frame.observed_wall_ns + entry_delay_, best};
                }
            }
        }
        if (!selected) { return; }
        entry_ = selected; ++attempts_;
        for (auto& s : scenarios_) { s.done = false; s.closed = false; s.filled = s.debit = 0;
            s.holdings = {}; s.hedged = {}; s.queue.reset(); s.accumulator = {}; }
        emit({{"type", "passive_signal"}, {"attempt", attempts_}, {"time_ns", entry_->signal},
            {"basket_id", baskets_[entry_->basket].id}, {"passive_leg", entry_->leg}, {"price_1e4", entry_->price},
            {"quantity_centicontracts", entry_->quantity}, {"conditional_quote_margin_micro", entry_->predicted}});
    }
    void close_entry(Scenario& s, I time) {
        s.closed = true; s.close_time = time;
        if (!s.filled) { s.done = true; ++s.empty; }
    }
    void settle(std::size_t index, I time) {
        auto& s = scenarios_[index]; s.done = true;
        const auto complete = *std::min_element(s.holdings.begin(), s.holdings.end());
        if (complete == s.filled) { ++s.completed; s.floor += complete * 20000; s.conditional_margin += complete * 20000 - s.debit; }
        else { s.halted = halted_ = true; for (auto q : s.holdings) { s.residual += q; } }
        emit({{"type", "passive_outcome"}, {"attempt", attempts_}, {"scenario", index}, {"time_ns", time},
            {"holdings_centicontracts", s.holdings}, {"debit_micro", s.debit}, {"fully_hedged", complete == s.filled},
            {"conditional_margin_micro", complete == s.filled ? Json(complete * 20000 - s.debit) : Json(nullptr)},
            {"realized_pnl_micro", nullptr}});
    }
    void advance(I time, const market::MarketState&) {
        if (!entry_) { return; }
        const auto& b = baskets_[entry_->basket];
        for (std::size_t index = 0; index < scenarios_.size(); ++index) {
            auto& s = scenarios_[index]; if (s.done) { continue; }
            if (!s.queue && time >= entry_->activation) {
                const auto& m = markets_.at(b.ids[entry_->leg]); const auto side = entry_->leg == 0 ? 0U : 1U;
                if (!m.valid || m.bid[side] != entry_->price || m.depth[side].empty() || m.depth[side][0].price.raw() <= entry_->price) {
                    s.done = true; ++s.censored; continue;
                }
                s.queue.emplace(m.queue[side], s.favorable, reconcile_);
                emit({{"type", "passive_resting"}, {"attempt", attempts_}, {"scenario", index},
                    {"time_ns", entry_->activation}, {"queue_ahead_centicontracts", m.queue[side]}});
            }
            if (s.queue) { s.queue->advance(time); }
            if (!s.closed && time >= entry_->expiry) { close_entry(s, entry_->expiry); }
            if (!s.closed || s.done) { continue; }
            I ordinal = 0;
            for (std::size_t leg = 0; leg < 3; ++leg) {
                if (leg == entry_->leg) { continue; }
                ++ordinal; const auto due = s.close_time + ordinal * s.delay;
                if (s.hedged[leg] || time < due) { continue; }
                s.hedged[leg] = true;
                const auto& m = markets_.at(b.ids[leg]);
                auto cost = m.valid ? buy(m.depth[leg == 0 ? 0 : 1], m.fee, s.filled) : Cost{};
                if (cost.debit > s.cash) { cost = {}; }
                s.holdings[leg] = cost.quantity; s.cash -= cost.debit; s.debit += cost.debit; s.spent += cost.debit;
                emit({{"type", "passive_hedge"}, {"attempt", attempts_}, {"scenario", index}, {"time_ns", due},
                    {"leg", leg}, {"quantity_centicontracts", cost.quantity}, {"debit_micro", cost.debit}});
            }
            bool done = true; for (std::size_t leg = 0; leg < 3; ++leg) { if (leg != entry_->leg) { done = done && s.hedged[leg]; } }
            if (done) { settle(index, time); }
        }
        if (std::all_of(scenarios_.begin(), scenarios_.end(), [](const auto& s) { return s.done; })) {
            entry_.reset(); last_attempt_end_ = time;
        }
    }
    void on_trade(const ReplayFrame& frame) {
        ++public_trades_; const auto& trade = *frame.trade;
        const auto exclude = [&](const char* reason) { ++excluded_trades_; ++exclusions_[reason]; };
        if (!markets_.contains(trade.market_id)) { exclude("outside_cohort"); return; }
        if (!trade.block_trade.has_value()) { exclude("unknown_block_status"); return; }
        if (*trade.block_trade) { exclude("block_trade"); return; }
        if (trade.exchange_time_ms > static_cast<std::uint64_t>(std::numeric_limits<I>::max() / ms)) { exclude("invalid_exchange_timestamp"); return; }
        const I offset = *frame.observed_wall_ns / ms - static_cast<I>(trade.exchange_time_ms);
        offset_min_ = offset_min_ ? std::min(*offset_min_, offset) : offset;
        offset_max_ = offset_max_ ? std::max(*offset_max_, offset) : offset;
        if (offset < -clock_error_ / ms) { exclude("clock_bound_exceeded"); return; }
        if (offset + clock_error_ / ms > max_gap_ / ms) {
            exclude("stale_trade"); return;
        }
        if (seen_.size() >= 100'000U) { detail::invalid("passive trade identity budget"); }
        if (!seen_.insert(trade.trade_id).second) { exclude("duplicate_trade"); return; }
        const auto side = trade.taker_yes ? 1U : 0U; // Contra-side to a resting buy.
        markets_.at(trade.market_id).last_trade[side] = frame.time_ns;
        if (!entry_ || trade.market_id != baskets_[entry_->basket].ids[entry_->leg] || side != (entry_->leg == 0 ? 0U : 1U) ||
            (side == 0 ? trade.yes_price_1e4 : 10000 - trade.yes_price_1e4) != entry_->price ||
            // Attribute only trades after activation even at the declared
            // adverse clock offset; that bound remains an assumption.
            static_cast<I>(trade.exchange_time_ms) * ms - entry_->wall <= clock_error_) { return; }
        for (std::size_t index = 0; index < scenarios_.size(); ++index) {
            auto& s = scenarios_[index]; if (!s.queue || s.done || s.closed) { continue; }
            const auto quantity = s.queue->trade(frame.time_ns, trade.quantity_centicontracts, entry_->quantity - s.filled);
            if (!quantity) { continue; }
            auto accumulator = s.accumulator;
            const auto& m = markets_.at(trade.market_id);
            const auto charge = core::charge_buy_fill(*core::Quantity::from_raw(quantity), *core::Price::from_raw(entry_->price),
                {maker_, m.fee.balance_quantum_micro}, accumulator);
            if (!charge || charge->debit.raw() > s.cash) { detail::invalid("passive entry funding invariant"); }
            s.accumulator = accumulator; s.filled += quantity; s.holdings[entry_->leg] += quantity;
            s.cash -= charge->debit.raw(); s.debit += charge->debit.raw(); s.spent += charge->debit.raw(); ++s.fills;
            emit({{"type", "passive_modeled_fill"}, {"attempt", attempts_}, {"scenario", index}, {"time_ns", frame.time_ns},
                {"trade_id", trade.trade_id}, {"quantity_centicontracts", quantity}, {"debit_micro", charge->debit.raw()}, {"own_fill_observed", false}});
            if (s.filled == entry_->quantity) { close_entry(s, frame.time_ns); }
        }
    }
    void censor(I time, const char* reason) {
        if (!entry_) { return; }
        for (auto& s : scenarios_) {
            if (s.done) { continue; } ++s.censored; s.done = true;
            if (s.filled) { s.halted = halted_ = true; for (auto q : s.holdings) { s.residual += q; } }
        }
        emit({{"type", "passive_censored"}, {"attempt", attempts_}, {"time_ns", time}, {"reason", reason}});
        entry_.reset(); last_attempt_end_ = time;
    }
    std::ostream& output_;
    std::unordered_map<Id, Cached> markets_;
    std::unordered_map<Id, std::vector<std::size_t>> dependencies_;
    std::vector<Basket> baskets_;
    std::vector<Scenario> scenarios_;
    std::unordered_set<std::string> seen_;
    std::map<std::string, std::uint64_t> exclusions_;
    std::optional<Entry> entry_;
    std::optional<market::ConnectionGeneration> generation_;
    std::optional<I> deadline_, last_wall_, offset_min_, offset_max_;
    std::string policy_hash_, basket_hash_;
    I entry_delay_{}, rest_{}, cancel_delay_{}, reconcile_{}, max_gap_{}, lookback_{}, clock_error_{}, spread_{}, margin_{}, from_{}, until_{}, cap_{}, initial_cash_{};
    I last_time_{-1}, last_attempt_end_{-1};
    std::uint32_t maker_{};
    std::uint64_t max_attempts_{}, attempts_{}, records_{}, total_trades_{}, public_trades_{}, excluded_trades_{}, sizes_{}, no_trade_{}, wide_{}, aggressive_{}, output_records_{};
    bool expired_{}, halted_{}, finished_{}, continuity_failed_{};
};
} // namespace

std::unique_ptr<PassiveProbe> make_passive_probe(const gateway::kalshi::MetadataSnapshot& metadata,
    const std::filesystem::path& basket_policy, const std::filesystem::path& probe_policy, std::ostream& output) {
    try { return std::make_unique<Probe>(metadata, basket_policy, probe_policy, output); }
    catch (const Json::exception&) { detail::invalid("invalid passive policy"); }
}
std::optional<ReplayError> run_passive_probe(const ReplayInput& input, const std::filesystem::path& basket_policy,
    const std::filesystem::path& probe_policy, std::ostream& output) {
    try {
        if (!input.plan.ws_controller || !input.plan.shared_subscription || !input.plan.public_trades) { detail::invalid("passive probe requires shared book and public trades"); }
        auto observer = make_passive_probe(input.session.metadata, basket_policy, probe_policy, output);
        const auto result = replay(input, *observer);
        if (const auto* error = std::get_if<ReplayError>(&result)) { return *error; }
        observer->report(input.plan); return std::nullopt;
    } catch (const ReplayError& error) { return error; }
      catch (const std::exception&) { return ReplayError{"passive probe failed", 0}; }
}
} // namespace eme::session
