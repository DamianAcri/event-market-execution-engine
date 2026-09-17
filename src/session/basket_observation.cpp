#include "eme/session/basket_observation.hpp"

#include "eme/core/basket_sizing.hpp"
#include "study_json.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ostream>
#include <unordered_map>
#include <unordered_set>

namespace eme::session {
namespace {
using detail::Json;
using MarketId = market::MarketId;
constexpr std::int64_t nanos_per_milli = 1'000'000;
constexpr auto maximum_wall_ms = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / nanos_per_milli);

enum class Status : std::size_t { unavailable, crossed, expired, no_depth, unfunded, nonpositive, positive, count };
constexpr std::array<std::string_view, static_cast<std::size_t>(Status::count)> status_names{
    "stream_or_book_unavailable", "crossed_book", "policy_expired", "no_depth", "insufficient_cash", "no_positive_margin", "positive_conditional_quote"};

struct CachedMarket final {
    MarketId id{};
    core::FeePolicy fee;
    std::array<bool, 2U> needed{};
    std::array<std::vector<core::BuyLevel>, 2U> depth;
    std::array<std::int64_t, 2U> available{};
    std::vector<std::size_t> dependencies;
    std::int64_t last_update{};
    bool valid{}, crossed{};
};
struct Witness final {
    core::SizedBasket quote;
    std::int64_t time{}, maximum_book_age{}, book_update_skew{};
    std::array<std::int64_t, 3U> available{};
};
struct ObservedBasket final {
    std::uint32_t id{};
    std::string key;
    std::array<std::size_t, 3U> markets{};
    Status status{Status::unavailable};
    std::array<std::uint64_t, static_cast<std::size_t>(Status::count)> counts{}, durations{};
    std::int64_t status_since{}, opened_at{}, episode_min_margin{};
    std::uint64_t evaluations{}, quantities{}, episodes{}, closed_episodes{}, left_censored{}, right_censored{}, episode_evaluations{};
    bool eligible_before{}, active{}, opening_left_censored{};
    std::optional<Witness> best_one, episode_peak;
};

std::uint32_t positive_id(const Json& row, const std::string_view field) {
    const auto id = detail::integer(row, field, std::numeric_limits<std::uint32_t>::max());
    if (id == 0U) { detail::invalid(std::string{field}); }
    return static_cast<std::uint32_t>(id);
}
std::string hash_field(const Json& row, const std::string_view field) {
    auto value = detail::string(row, field);
    if (value.size() != 64U || value.find_first_not_of("0123456789abcdef") != std::string::npos) { detail::invalid(std::string{field}); }
    return value;
}

class Observation final : public BasketObservation {
public:
    Observation(const gateway::kalshi::MetadataSnapshot& metadata, const std::filesystem::path& path, std::ostream& output)
        : output_{output} {
        const auto bytes = detail::read_text(path, 32U * 1024U * 1024U);
        policy_hash_ = detail::fingerprint_bytes(bytes).sha256;
        const auto policy = detail::parse_strict(bytes);
        detail::shape(policy, {"schema_version", "kind", "freshness_mode", "qualification_sha256", "metadata_sha256",
            "valid_from_unix_ms", "valid_until_unix_ms", "max_episode_events", "screen"});
        if (detail::integer(policy, "schema_version") != 1U ||
            detail::string(policy, "kind") != "conditional_basket_observation" ||
            detail::string(policy, "freshness_mode") != "contiguous_shared_stream") { detail::invalid("basket observation mode"); }
        qualification_hash_ = hash_field(policy, "qualification_sha256");
        metadata_hash_ = hash_field(policy, "metadata_sha256");
        if (metadata_hash_ != detail::fingerprint_bytes(metadata.canonical_json()).sha256) { detail::invalid("basket metadata binding"); }
        valid_from_ = static_cast<std::int64_t>(detail::integer(policy, "valid_from_unix_ms", maximum_wall_ms)) * nanos_per_milli;
        valid_until_ = static_cast<std::int64_t>(detail::integer(policy, "valid_until_unix_ms", maximum_wall_ms)) * nanos_per_milli;
        maximum_events_ = detail::integer(policy, "max_episode_events", 1'000'000U);
        if (valid_until_ <= valid_from_ || maximum_events_ == 0U) { detail::invalid("basket observation window/budget"); }
        parse_screen(metadata, policy.at("screen"));
    }

    void start(const bool live) override {
        emit({{"type", "basket_start"}, {"schema_version", 1}, {"policy_sha256", policy_hash_},
            {"qualification_sha256", qualification_hash_}, {"metadata_sha256", metadata_hash_},
            {"execution", live ? "live_conditional_observation_no_orders" : "replay_conditional_observation_no_orders"},
            {"freshness_mode", "contiguous_shared_stream"}, {"simulated_fills", false}, {"orders_sent", 0},
            {"fill_cost_model", "one_assumed_fill_per_consumed_price_level"}, {"funding_fill_step_centicontracts", 1}});
    }

    void before(const std::int64_t time, const market::MarketState&) override {
        if (time < 0 || (last_time_ && time < *last_time_)) { detail::invalid("basket observation clock regression"); }
        if (deadline_ && time >= *deadline_ && !expired_) { expire(*deadline_); }
    }

    void after(const ReplayFrame& frame, std::span<const opportunity::CandidateEvent>, const market::MarketState& state) override {
        before(frame.time_ns, state);
        if (finished_ || !frame.observed_wall_ns || *frame.observed_wall_ns < valid_from_) {
            detail::invalid("basket policy wall-clock binding");
        }
        if (!first_time_) {
            first_time_ = frame.time_ns;
            union_since_ = frame.time_ns;
            for (auto& basket : baskets_) { basket.status_since = frame.time_ns; }
        }
        last_time_ = frame.time_ns;
        ++records_;
        if (frame.trade) { ++trades_; }
        if (*frame.observed_wall_ns >= valid_until_) { if (!expired_) { expire(frame.time_ns); } }
        else if (!expired_) {
            const auto remaining = valid_until_ - *frame.observed_wall_ns;
            if (frame.time_ns > std::numeric_limits<std::int64_t>::max() - remaining) { detail::invalid("basket deadline overflow"); }
            const auto mapped = frame.time_ns + remaining;
            // A backwards local wall-clock correction must never extend the
            // previously accepted monotonic validity horizon.
            deadline_ = deadline_ ? std::min(*deadline_, mapped) : mapped;
        }
        if (expired_) { return; }
        const auto generation = state.connection_generation();
        if (!state.connected() || generation != generation_) {
            generation_ = generation;
            for (auto& market : markets_) { market.valid = false; }
            for (auto& basket : baskets_) { unavailable(basket, frame.time_ns, Status::unavailable); }
        }
        if (!state.connected()) { return; }
        if (!frame.market_id) { return; }
        const auto found = indexes_.find(*frame.market_id);
        if (found == indexes_.end()) { detail::invalid("basket update outside frozen cohort"); }
        auto& cache = markets_[found->second];
        if (!frame.applied) {
            // Shared WS sequence failures normally invalidate the connection.
            // Also fail closed if a caller supplies a per-book invalidation.
            const auto* book = state.find_book(cache.id);
            if (!book || book->state() != book::BookState::valid) {
                cache.valid = false;
                for (const auto index : cache.dependencies) { unavailable(baskets_[index], frame.time_ns, Status::unavailable); }
            }
            return;
        }
        update_cache(cache, frame.time_ns, state);
        ++book_updates_;
        std::uint64_t budget = total_budget_;
        for (const auto index : cache.dependencies) { evaluate(baskets_[index], frame.time_ns, budget); }
        // All dependencies observe one atomic book update. Updating this peak
        // during the loop could count a newly opened basket alongside a stale
        // positive basket which the same update is about to close.
        maximum_active_ = std::max(maximum_active_, active_);
    }

    void finish(const std::int64_t time, const market::MarketState& state) override {
        if (finished_) { return; }
        before(time, state);
        for (auto& basket : baskets_) {
            close(basket, time, "observation_end", true);
            account_time(basket, time);
        }
        account_union(time);
        last_time_ = time;
        finished_ = true;
    }

    void checkpoint(const std::int64_t time) override {
        emit({{"type", "basket_status"}, {"time_ns", time}, {"records", records_}, {"book_updates", book_updates_},
            {"basket_evaluations", evaluations_}, {"evaluated_quantities", quantities_}, {"episode_events", events_},
            {"policy_expired", expired_}, {"orders_sent", 0}, {"simulated_fills", false}});
    }

    std::optional<std::int64_t> next_event_time() const override { return expired_ || finished_ ? std::nullopt : deadline_; }

    void report(const ReplayPlan& plan) override {
        if (!finished_) { detail::invalid("basket observation not finished"); }
        Json rows = Json::array();
        for (const auto& basket : baskets_) {
            Json counts = Json::object(), durations = Json::object();
            for (std::size_t i = 0; i < status_names.size(); ++i) {
                counts[std::string{status_names[i]}] = basket.counts[i];
                durations[std::string{status_names[i]}] = basket.durations[i];
            }
            const auto unknown = basket.durations[static_cast<std::size_t>(Status::unavailable)] +
                basket.durations[static_cast<std::size_t>(Status::crossed)] + basket.durations[static_cast<std::size_t>(Status::expired)];
            std::uint64_t eligible = 0;
            for (const auto value : {Status::no_depth, Status::unfunded, Status::nonpositive, Status::positive}) {
                eligible += basket.durations[static_cast<std::size_t>(value)];
            }
            rows.push_back({{"basket_id", basket.id}, {"key", basket.key},
                {"market_ids", {markets_[basket.markets[0]].id, markets_[basket.markets[1]].id, markets_[basket.markets[2]].id}},
                {"eligible_ns", eligible}, {"no_data_ns", unknown},
                {"positive_conditional_ns", basket.durations[static_cast<std::size_t>(Status::positive)]},
                {"evaluations", basket.evaluations},
                {"evaluated_quantities", basket.quantities}, {"episodes_opened", basket.episodes},
                {"episodes_closed", basket.closed_episodes}, {"left_censored_episodes", basket.left_censored},
                {"right_censored_episodes", basket.right_censored}, {"status_counts", std::move(counts)},
                {"status_duration_ns", std::move(durations)},
                {"best_one_contract_diagnostic", basket.best_one ? witness_json(*basket.best_one, basket) : Json(nullptr)}});
        }
        emit({{"type", "basket_complete"}, {"schema_version", 1}, {"policy_sha256", policy_hash_},
            {"metadata_sha256", metadata_hash_}, {"qualification_sha256", qualification_hash_},
            {"manifest_sha256", plan.manifest_sha256}, {"plan_sha256", plan.plan_sha256},
            {"source_kind", plan.source_kind}, {"records", records_}, {"book_updates", book_updates_},
            {"public_trades", trades_}, {"basket_evaluations", evaluations_}, {"evaluated_quantities", quantities_},
            {"episode_events", events_}, {"episode_event_budget", maximum_events_}, {"event_budget_exceeded", false},
            {"maximum_simultaneous_positive_baskets", maximum_active_}, {"any_positive_basket_union_ns", positive_union_},
            {"episodes_are_independent_opportunities", false},
            {"incomplete", false}, {"policy_expired", expired_}, {"first_time_ns", first_time_ ? Json(*first_time_) : Json(nullptr)},
            {"last_time_ns", last_time_ ? Json(*last_time_) : Json(nullptr)}, {"baskets", std::move(rows)},
            {"freshness_mode", "contiguous_shared_stream"}, {"book_age_is_admission_filter", false},
            {"independent_quotes_share_liquidity", true}, {"joint_capital_allocation", false},
            {"simulated_fills", false}, {"orders_sent", 0}, {"realized_pnl_micro", nullptr},
            {"production_certificate", false}, {"model_classification", "conditional_common_scalar_observation_only"}});
    }

private:
    void parse_screen(const gateway::kalshi::MetadataSnapshot& metadata, const Json& screen) {
        detail::shape(screen, {"schema_version", "markets", "baskets", "as_of_ms", "max_age_ms", "max_skew_ms",
            "max_total_evaluations", "sizing", "fees", "books"});
        if (detail::integer(screen, "schema_version") != 1U || detail::integer(screen, "as_of_ms") != 0U ||
            !screen.at("books").is_array() || !screen.at("books").empty()) { detail::invalid("basket observation cannot seed REST books"); }
        // Retained solely for schema compatibility with the frozen REST screen.
        // They are explicitly not quiet-book age/skew admission thresholds.
        (void)detail::integer(screen, "max_age_ms");
        (void)detail::integer(screen, "max_skew_ms");
        total_budget_ = detail::integer(screen, "max_total_evaluations", 10'000'000U);
        const auto& sizing = screen.at("sizing");
        detail::shape(sizing, {"cap_centicontracts", "step_centicontracts", "available_cash_micro", "minimum_margin_micro", "max_evaluations"});
        const auto cap = static_cast<std::int64_t>(detail::integer(sizing, "cap_centicontracts", 10'000U));
        if (cap < 100 || cap % 100 != 0 || detail::integer(sizing, "step_centicontracts") != 100U) { detail::invalid("basket quantity grid"); }
        limits_ = {*core::Quantity::from_raw(cap), *core::Quantity::from_raw(100),
            *core::Cash::from_raw(static_cast<std::int64_t>(detail::integer(sizing, "available_cash_micro", 1'000'000'000'000'000U))),
            *core::Cash::from_raw(static_cast<std::int64_t>(detail::integer(sizing, "minimum_margin_micro", 1'000'000'000'000'000U))),
            detail::integer(sizing, "max_evaluations", 10'000'000U)};
        if (total_budget_ == 0U || limits_.max_evaluations == 0U) { detail::invalid("basket evaluation budget"); }
        const auto& rows = screen.at("markets");
        if (!rows.is_array() || rows.empty() || rows.size() > 64U || rows.size() != metadata.markets().size()) { detail::invalid("basket capture markets"); }
        markets_.reserve(rows.size()); indexes_.reserve(rows.size());
        for (const auto& row : rows) {
            detail::shape(row, {"id", "ticker"});
            const auto id = positive_id(row, "id");
            const auto ticker = metadata.markets().find(id);
            if (!ticker || *ticker != detail::string(row, "ticker") || !indexes_.emplace(id, markets_.size()).second) {
                detail::invalid("basket market binding");
            }
            CachedMarket cache; cache.id = id; markets_.push_back(std::move(cache));
        }
        const auto& fees = screen.at("fees");
        if (!fees.is_array() || fees.size() != markets_.size()) { detail::invalid("basket fee coverage"); }
        std::unordered_set<MarketId> fee_ids;
        for (const auto& row : fees) {
            detail::shape(row, {"market_id", "coefficient_ppm", "balance_quantum_micro"});
            const auto id = positive_id(row, "market_id");
            const auto found = indexes_.find(id);
            if (found == indexes_.end() || !fee_ids.insert(id).second) { detail::invalid("basket fee market"); }
            auto& fee = markets_[found->second].fee;
            fee.coefficient_ppm = static_cast<std::uint32_t>(detail::integer(row, "coefficient_ppm", 1'000'000U));
            fee.balance_quantum_micro = static_cast<std::uint32_t>(detail::integer(row, "balance_quantum_micro", 10'000U));
            if (fee.balance_quantum_micro != 100U && fee.balance_quantum_micro != 10'000U) { detail::invalid("basket balance precision"); }
        }
        const auto& basket_rows = screen.at("baskets");
        if (!basket_rows.is_array() || basket_rows.empty() || basket_rows.size() > 100U) { detail::invalid("basket observation count"); }
        std::unordered_set<std::uint32_t> ids;
        std::unordered_set<std::string> keys, triples;
        std::unordered_map<MarketId, std::array<std::uint64_t, 3U>> semantics;
        for (const auto& row : basket_rows) {
            detail::shape(row, {"id", "key", "lower_market_id", "upper_market_id", "range_market_id",
                "lower_threshold_cents", "upper_threshold_cents", "interval_lower_cents", "interval_upper_cents"});
            ObservedBasket basket; basket.id = positive_id(row, "id"); basket.key = detail::string(row, "key");
            const std::array<MarketId, 3U> market_ids{positive_id(row, "lower_market_id"), positive_id(row, "upper_market_id"), positive_id(row, "range_market_id")};
            const auto a = detail::integer(row, "lower_threshold_cents"), b = detail::integer(row, "upper_threshold_cents");
            const auto lower = detail::integer(row, "interval_lower_cents"), upper = detail::integer(row, "interval_upper_cents");
            if (!(a < lower && lower <= upper && upper <= b) || market_ids[0] == market_ids[1] ||
                market_ids[0] == market_ids[2] || market_ids[1] == market_ids[2]) { detail::invalid("basket conditional payoff geometry"); }
            const auto triple = std::to_string(market_ids[0]) + ":" + std::to_string(market_ids[1]) + ":" + std::to_string(market_ids[2]);
            if (!ids.insert(basket.id).second || !keys.insert(basket.key).second || !triples.insert(triple).second) { detail::invalid("duplicate observed basket"); }
            const std::array<std::array<std::uint64_t, 3U>, 3U> meanings{{{0, a, a}, {0, b, b}, {1, lower, upper}}};
            for (std::size_t i = 0; i < 3U; ++i) {
                const auto found = indexes_.find(market_ids[i]);
                if (found == indexes_.end()) { detail::invalid("basket leg market"); }
                basket.markets[i] = found->second;
                const auto [entry, inserted] = semantics.emplace(market_ids[i], meanings[i]);
                if (!inserted && entry->second != meanings[i]) { detail::invalid("inconsistent basket market semantics"); }
            }
            baskets_.push_back(std::move(basket));
        }
        std::sort(baskets_.begin(), baskets_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        for (std::size_t i = 0; i < baskets_.size(); ++i) {
            for (std::size_t leg = 0; leg < 3U; ++leg) {
                auto& market = markets_[baskets_[i].markets[leg]];
                market.needed[leg == 0U ? 0U : 1U] = true;
                market.dependencies.push_back(i);
            }
        }
        for (auto& market : markets_) {
            if (market.dependencies.empty()) { detail::invalid("unused basket capture market"); }
            for (std::size_t side = 0; side < 2U; ++side) {
                if (market.needed[side]) { market.depth[side].reserve(static_cast<std::size_t>(cap)); }
            }
        }
    }

    void update_cache(CachedMarket& cache, const std::int64_t time, const market::MarketState& state) {
        const auto* book = state.find_book(cache.id);
        cache.valid = book && book->state() == book::BookState::valid;
        cache.crossed = cache.valid && book->best_bid() && book->best_ask() && book->best_bid()->raw() > book->best_ask()->raw();
        cache.last_update = time;
        for (std::size_t side = 0; side < 2U; ++side) {
            if (!cache.needed[side]) { continue; }
            auto& depth = cache.depth[side]; depth.clear(); cache.available[side] = 0;
            if (!cache.valid || cache.crossed) { continue; }
            book->visit_levels(side == 0U ? book::Side::ask : book::Side::bid, [&](const book::Level level) {
                const auto take = std::min(level.quantity.raw(), limits_.cap.raw() - cache.available[side]);
                if (take > 0) {
                    depth.push_back({*core::Price::from_raw(side == 0U ? level.price.raw() : 10'000 - level.price.raw()), *core::Quantity::from_raw(take)});
                    cache.available[side] += take;
                }
                return cache.available[side] < limits_.cap.raw();
            });
        }
    }

    Witness witness(const core::SizedBasket& quote, const ObservedBasket& basket, const std::int64_t time) const {
        Witness result; result.quote = quote; result.time = time;
        auto oldest = time, newest = std::int64_t{0};
        for (std::size_t i = 0; i < 3U; ++i) {
            const auto& market = markets_[basket.markets[i]];
            oldest = std::min(oldest, market.last_update); newest = std::max(newest, market.last_update);
            result.available[i] = market.available[i == 0U ? 0U : 1U];
        }
        result.maximum_book_age = time - oldest; result.book_update_skew = newest - oldest;
        return result;
    }
    Json witness_json(const Witness& witness, const ObservedBasket& basket) const {
        const auto& quote = witness.quote;
        Json legs = Json::array(); std::int64_t debit = 0, reserve = 0, notional = 0;
        for (std::size_t i = 0; i < 3U; ++i) {
            const auto& leg = quote.legs[i];
            debit += leg.debit.raw(); reserve += leg.reservation.raw(); notional += leg.notional.raw();
            legs.push_back({{"market_id", markets_[basket.markets[i]].id}, {"outcome", i == 0U ? "yes" : "no"},
                {"limit_price_1e4", leg.limit.raw()}, {"notional_micro", leg.notional.raw()},
                {"debit_micro", leg.debit.raw()}, {"reservation_micro", leg.reservation.raw()}});
        }
        return {{"time_ns", witness.time}, {"quantity_centicontracts", quote.quantity.raw()}, {"net_margin_micro", quote.net_margin_micro},
            {"payout_floor_micro", quote.payout_floor.raw()}, {"debit_micro", debit}, {"reservation_micro", reserve},
            {"gross_margin_micro", quote.payout_floor.raw() - notional}, {"funded", reserve <= limits_.available.raw()},
            {"maximum_book_update_age_ns", witness.maximum_book_age}, {"book_update_skew_ns", witness.book_update_skew},
            {"visible_depth_capped_centicontracts", witness.available}, {"legs", std::move(legs)}};
    }
    void account_time(ObservedBasket& basket, const std::int64_t time) {
        if (!first_time_) { return; }
        if (time < basket.status_since) { detail::invalid("basket interval clock regression"); }
        basket.durations[static_cast<std::size_t>(basket.status)] += static_cast<std::uint64_t>(time - basket.status_since);
        basket.status_since = time;
    }
    void status(ObservedBasket& basket, const Status value, const std::int64_t time) {
        account_time(basket, time); basket.status = value; ++basket.counts[static_cast<std::size_t>(value)];
    }
    void event(Json value) {
        if (events_ == maximum_events_) { detail::invalid("basket episode event budget exceeded"); }
        ++events_; emit(value);
    }
    void close(ObservedBasket& basket, const std::int64_t time, const std::string_view reason, const bool censored) {
        if (!basket.active) { return; }
        event({{"type", "basket_episode_close"}, {"basket_id", basket.id}, {"episode", basket.episodes},
            {"time_ns", time}, {"opened_at_ns", basket.opened_at}, {"observed_duration_ns", time - basket.opened_at},
            {"reason", reason}, {"right_censored", censored}, {"left_censored", basket.opening_left_censored},
            {"evaluations", basket.episode_evaluations}, {"minimum_net_margin_micro", basket.episode_min_margin},
            {"peak_quote", witness_json(*basket.episode_peak, basket)}});
        account_union(time); --active_;
        basket.active = false; ++basket.closed_episodes; if (censored) { ++basket.right_censored; }
    }
    void unavailable(ObservedBasket& basket, const std::int64_t time, const Status reason) {
        close(basket, time, status_names[static_cast<std::size_t>(reason)], true);
        status(basket, reason, time); basket.eligible_before = false;
    }
    void expire(const std::int64_t time) {
        expired_ = true; deadline_.reset();
        for (auto& basket : baskets_) { unavailable(basket, time, Status::expired); }
    }
    void evaluate(ObservedBasket& basket, const std::int64_t time, std::uint64_t& budget) {
        ++basket.evaluations; ++evaluations_;
        for (const auto index : basket.markets) {
            if (!markets_[index].valid) { unavailable(basket, time, Status::unavailable); return; }
            if (markets_[index].crossed) { unavailable(basket, time, Status::crossed); return; }
        }
        if (budget == 0U) { detail::invalid("basket update evaluation budget exceeded"); }
        std::array<core::BuyDepth, 3U> depth;
        for (std::size_t i = 0; i < 3U; ++i) {
            const auto& market = markets_[basket.markets[i]];
            depth[i] = {market.depth[i == 0U ? 0U : 1U], market.fee};
        }
        auto bounded = limits_; bounded.max_evaluations = std::min(budget, limits_.max_evaluations);
        const auto result = core::size_buy_basket(depth, bounded);
        budget -= result.evaluated_quantities;
        basket.quantities += result.evaluated_quantities; quantities_ += result.evaluated_quantities;
        if (result.status == core::SizingStatus::search_budget_exceeded || result.status == core::SizingStatus::invalid_input ||
            result.status == core::SizingStatus::arithmetic_error) { detail::invalid("basket sizing incomplete or invalid"); }
        if (result.one_contract_diagnostic && (!basket.best_one ||
            result.one_contract_diagnostic->net_margin_micro > basket.best_one->quote.net_margin_micro)) {
            basket.best_one = witness(*result.one_contract_diagnostic, basket, time);
        }
        const auto classification = result.quote ? Status::positive : result.status == core::SizingStatus::no_depth ? Status::no_depth :
            result.status == core::SizingStatus::insufficient_cash ? Status::unfunded : Status::nonpositive;
        if (!result.quote) {
            close(basket, time, status_names[static_cast<std::size_t>(classification)], false);
        } else {
            const auto value = witness(*result.quote, basket, time);
            if (!basket.active) {
                account_union(time); ++active_;
                basket.active = true; ++basket.episodes; basket.opened_at = time;
                basket.opening_left_censored = !basket.eligible_before;
                if (basket.opening_left_censored) { ++basket.left_censored; }
                basket.episode_evaluations = 0; basket.episode_peak = value;
                basket.episode_min_margin = value.quote.net_margin_micro;
                event({{"type", "basket_episode_open"}, {"basket_id", basket.id}, {"key", basket.key},
                    {"episode", basket.episodes}, {"time_ns", time}, {"left_censored", basket.opening_left_censored},
                    {"quote", witness_json(value, basket)}});
            }
            ++basket.episode_evaluations;
            basket.episode_min_margin = std::min(basket.episode_min_margin, value.quote.net_margin_micro);
            if (value.quote.net_margin_micro > basket.episode_peak->quote.net_margin_micro) { basket.episode_peak = value; }
        }
        status(basket, classification, time); basket.eligible_before = true;
    }
    void emit(const Json& value) {
        output_ << value.dump() << '\n';
        if (!output_) { detail::invalid("basket observation output failed"); }
    }
    void account_union(const std::int64_t time) {
        if (!first_time_) { return; }
        if (time < union_since_) { detail::invalid("basket union clock regression"); }
        if (active_ != 0U) { positive_union_ += static_cast<std::uint64_t>(time - union_since_); }
        union_since_ = time;
    }

    std::ostream& output_;
    std::string policy_hash_, metadata_hash_, qualification_hash_;
    std::int64_t valid_from_{}, valid_until_{};
    std::uint64_t maximum_events_{}, total_budget_{}, records_{}, book_updates_{}, trades_{}, evaluations_{}, quantities_{}, events_{};
    std::uint64_t active_{}, maximum_active_{}, positive_union_{};
    std::int64_t union_since_{};
    core::SizingLimits limits_{*core::Quantity::from_raw(100), *core::Quantity::from_raw(100), *core::Cash::from_raw(0), *core::Cash::from_raw(0), 1U};
    std::vector<CachedMarket> markets_;
    std::unordered_map<MarketId, std::size_t> indexes_;
    std::vector<ObservedBasket> baskets_;
    std::optional<std::int64_t> first_time_, last_time_, deadline_;
    std::optional<market::ConnectionGeneration> generation_;
    bool expired_{}, finished_{};
};
}  // namespace

std::unique_ptr<BasketObservation> make_basket_observation(const gateway::kalshi::MetadataSnapshot& metadata,
    const std::filesystem::path& policy, std::ostream& output) {
    try { return std::make_unique<Observation>(metadata, policy, output); }
    catch (const Json::exception&) { detail::invalid("invalid basket observation JSON"); }
}

std::optional<ReplayError> run_basket_observation(const ReplayInput& input, const std::filesystem::path& policy, std::ostream& output) {
    try {
        if (!input.plan.ws_controller || !input.plan.shared_subscription) { detail::invalid("basket observation requires shared contiguous WS history"); }
        auto observer = make_basket_observation(input.session.metadata, policy, output);
        observer->start();
        const auto result = replay(input, *observer);
        if (const auto* error = std::get_if<ReplayError>(&result)) { return *error; }
        observer->report(input.plan);
        return std::nullopt;
    } catch (const ReplayError& error) { return error; }
}
}  // namespace eme::session
