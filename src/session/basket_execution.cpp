#include "eme/session/basket_execution.hpp"
#include "eme/session/basket_observation.hpp"
#include "study_json.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <tuple>
#include <unordered_map>

namespace eme::session {
namespace {
using detail::Json;
using I = std::int64_t;
using Id = market::MarketId;
constexpr I ms = 1'000'000;
struct Physical {
    I price{}, quantity{};
};
struct Cached {
    core::FeePolicy fee;
    std::array<std::vector<Physical>, 2> sides; // bid descending, ask ascending, YES coordinates
    bool valid{};
};
struct Basket {
    std::uint32_t id{};
    std::array<Id, 3> ids{};
};
using Key = std::tuple<Id, bool, I>; // market, physical ask side, YES price
struct Fill {
    I quantity{}, cash{}, limit{};
    Json levels = Json::array();
};
enum class Stage { buy_arrival, buy_response, sell_arrival, sell_response };
struct Attempt {
    Basket basket;
    I quantity{}, spent{}, received{}, due{}, limit{}, reserve{};
    std::size_t leg{};
    Stage stage{Stage::buy_arrival};
    std::array<I, 3> holdings{};
    std::string reason;
};
struct Scenario {
    I delay{}, cash{}, spent{}, received{}, floor{}, complete_cost{}, unwind_net{}, reserved{},
        peak_encumbered{};
    std::optional<Attempt> attempt;
    std::map<Key, I> consumed;
    std::uint64_t attempts{}, completed{}, exited{}, empty{}, busy{}, rejected{}, budget_aborts{}, censored{},
        final_budget_breaches{};
    bool halted{};
};

class Study final : public BasketExecutionStudy {
  public:
    Study(const gateway::kalshi::MetadataSnapshot &metadata, const std::filesystem::path &basket_path,
          const std::filesystem::path &path, std::ostream &out)
        : out_{out} {
        const auto basket_bytes = detail::read_text(basket_path, 32U * 1024U * 1024U);
        const auto policy_bytes = detail::read_text(path);
        hash_ = detail::fingerprint_bytes(policy_bytes).sha256;
        basket_hash_ = detail::fingerprint_bytes(basket_bytes).sha256;
        const auto p = detail::parse_strict(policy_bytes), b = detail::parse_strict(basket_bytes);
        detail::shape(p, {"schema_version", "kind", "basket_policy_sha256", "arrival_and_response_ms",
                          "minimum_margin_micro", "max_attempts"});
        if (detail::integer(p, "schema_version", 1) != 1 ||
            p.at("kind") != "chronological_basket_execution" ||
            p.at("basket_policy_sha256") != basket_hash_) {
            detail::invalid("execution policy binding");
        }
        margin_ = static_cast<I>(detail::integer(p, "minimum_margin_micro", 1'000'000));
        max_attempts_ = detail::integer(p, "max_attempts", 1000);
        if (!margin_ || !max_attempts_) {
            detail::invalid("execution limits");
        }
        if (!p.at("arrival_and_response_ms").is_array() || p.at("arrival_and_response_ms").empty() ||
            p.at("arrival_and_response_ms").size() > 6) {
            detail::invalid("delay grid");
        }
        // Validate original rules, fees, bounds and metadata using the same control.
        control_ = make_basket_observation(metadata, basket_path, out_,
                                           [this](const BasketEpisode &event) { signals_.push_back(event); });
        from_ = b.at("valid_from_unix_ms").get<I>() * ms;
        until_ = b.at("valid_until_unix_ms").get<I>() * ms;
        const auto &screen = b.at("screen");
        capital_ = screen.at("sizing").at("available_cash_micro").get<I>();
        cap_ = screen.at("sizing").at("cap_centicontracts").get<I>();
        for (const auto &f : screen.at("fees")) {
            markets_[f.at("market_id").get<Id>()].fee = {f.at("coefficient_ppm").get<std::uint32_t>(),
                                                         f.at("balance_quantum_micro").get<std::uint32_t>()};
        }
        for (const auto &r : screen.at("baskets")) {
            const auto id = r.at("id").get<std::uint32_t>();
            baskets_[id] = {id,
                            {r.at("lower_market_id").get<Id>(), r.at("upper_market_id").get<Id>(),
                             r.at("range_market_id").get<Id>()}};
        }
        std::set<I> distinct;
        for (const auto &delay : p.at("arrival_and_response_ms")) {
            if (!delay.is_number_unsigned()) {
                detail::invalid("delay type");
            }
            const auto d = delay.get<I>();
            if (d < 1 || d > 1000 || !distinct.insert(d).second) {
                detail::invalid("delay bound");
            }
            Scenario s;
            s.delay = d * ms;
            s.cash = capital_;
            scenarios_.push_back(std::move(s));
        }
        control_->start();
        emit({{"type", "basket_execution_start"},
              {"policy", p},
              {"policy_sha256", hash_},
              {"orders_sent", 0},
              {"liquidity_model", "cumulative_physical_level_deductions_no_replenishment_credit"},
              {"execution", "offline_chronological_counterfactual"},
              {"one_way_and_response_delay_verified", false}});
    }
    void before(I time, const market::MarketState &state) override {
        if (time < 0 || (last_time_ && time < *last_time_)) {
            detail::invalid("execution clock regression");
        }
        if (last_time_ && time - *last_time_ > 15'000'000'000LL) {
            break_continuity(time, "record_gap");
        }
        control_->before(time, state);
    }
    void after(const ReplayFrame &frame, std::span<const opportunity::CandidateEvent> events,
               const market::MarketState &state) override {
        ++records_;
        if (!frame.observed_wall_ns || *frame.observed_wall_ns < from_) {
            detail::invalid("execution wall binding");
        }
        if (last_wall_ && (*frame.observed_wall_ns < *last_wall_ ||
                           *frame.observed_wall_ns - *last_wall_ > 15'000'000'000LL)) {
            break_continuity(frame.time_ns, "wall_clock_discontinuity");
        }
        if (*frame.observed_wall_ns >= until_) {
            expired_ = true;
            for (std::size_t i = 0; i < scenarios_.size(); ++i) {
                censor(i, frame.time_ns, "policy_expired");
            }
        }
        const bool disconnect =
            !state.connected() || (generation_ && generation_ != state.connection_generation());
        if (disconnect) {
            for (std::size_t i = 0; i < scenarios_.size(); ++i) {
                censor(i, frame.time_ns, "connection_change");
            }
            for (auto &[id, m] : markets_) {
                (void)id;
                m.valid = false;
            }
        }
        if (frame.market_id && markets_.contains(*frame.market_id)) {
            const auto *book = state.find_book(*frame.market_id);
            if (!book || book->state() != book::BookState::valid ||
                (book->best_bid() && book->best_ask() && book->best_bid()->raw() > book->best_ask()->raw())) {
                markets_.at(*frame.market_id).valid = false;
                for (std::size_t i = 0; i < scenarios_.size(); ++i) {
                    censor(i, frame.time_ns, "invalid_book");
                }
            }
        }
        // Validate both clocks BEFORE advancing orders. Cached books still come
        // from earlier records, so an update cannot price an earlier arrival.
        if (!broken_ && !expired_ && !disconnect) {
            for (std::size_t i = 0; i < scenarios_.size(); ++i) {
                advance(i, frame.time_ns);
            }
        }
        if (frame.applied && frame.market_id && markets_.contains(*frame.market_id)) {
            auto &m = markets_.at(*frame.market_id);
            const auto *book = state.find_book(*frame.market_id);
            if (book && book->state() == book::BookState::valid &&
                !(book->best_bid() && book->best_ask() &&
                  book->best_bid()->raw() > book->best_ask()->raw())) {
                m.valid = true;
                for (std::size_t side = 0; side < 2; ++side) {
                    m.sides[side].clear();
                    book->visit_levels(side == 0 ? book::Side::bid : book::Side::ask, [&](book::Level l) {
                        m.sides[side].push_back({l.price.raw(), l.quantity.raw()});
                        return true;
                    });
                }
            }
        }
        signals_.clear();
        control_->after(frame, events, state);
        episodes_ += signals_.size();
        if (!broken_ && !expired_ && state.connected() && !signals_.empty()) {
            for (std::size_t i = 0; i < scenarios_.size(); ++i) {
                decide(i, frame.time_ns);
            }
        }
        last_time_ = frame.time_ns;
        last_wall_ = frame.observed_wall_ns;
        generation_ = state.connection_generation();
    }
    void finish(I time, const market::MarketState &state) override {
        // Never execute arrivals or acknowledgements beyond actual observation.
        for (std::size_t i = 0; i < scenarios_.size(); ++i) {
            censor(i, time, "end_of_observation");
        }
        control_->finish(time, state);
        finished_ = true;
    }
    void report(const ReplayPlan &plan) override {
        if (!finished_) {
            detail::invalid("execution report before finish");
        }
        control_->report(plan);
        Json rows = Json::array();
        for (std::size_t i = 0; i < scenarios_.size(); ++i) {
            const auto &s = scenarios_[i];
            Json inventory = Json::array();
            bool exposure = false;
            if (s.attempt) {
                for (std::size_t leg = 0; leg < 3; ++leg) {
                    if (s.attempt->holdings[leg]) {
                        exposure = true;
                        inventory.push_back({{"market_id", s.attempt->basket.ids[leg]},
                                             {"outcome", leg == 0 ? "yes" : "no"},
                                             {"quantity_centicontracts", s.attempt->holdings[leg]}});
                    }
                }
            }
            rows.push_back(
                {{"scenario", i},
                 {"arrival_delay_ms", s.delay / ms},
                 {"response_delay_ms", s.delay / ms},
                 {"attempts", s.attempts},
                 {"completed_baskets", s.completed},
                 {"fully_unwound_attempts", s.exited},
                 {"empty_attempts", s.empty},
                 {"episode_onsets_skipped_busy", s.busy},
                 {"episode_onsets_rejected", s.rejected},
                 {"budget_aborts", s.budget_aborts},
                 {"censored_attempts", s.censored},
                 {"completed_budget_breaches", s.final_budget_breaches},
                 {"halted", s.halted},
                 {"cash_micro", s.cash},
                 {"unreleased_reservation_micro", s.reserved},
                 {"buy_debits_micro", s.spent},
                 {"sell_credits_micro", s.received},
                 {"completed_conditional_floor_micro", s.floor},
                 {"completed_basket_cost_micro", s.complete_cost},
                 {"modeled_unwind_pnl_micro", s.unwind_net},
                 {"residual_inventory", inventory},
                 {"conditional_net_micro",
                  (!exposure && !s.attempt) ? Json(s.cash + s.floor - capital_) : Json(nullptr)},
                 {"conditional_stress_net_micro", s.cash + s.floor - capital_ - s.reserved},
                 {"stress_assumptions", "zero value for residuals and full loss of pending buy reservation"},
                 {"peak_cash_encumbered_micro", s.peak_encumbered},
                 {"realized_pnl_micro", nullptr},
                 {"cash_identity_verified", s.cash == capital_ - s.spent + s.received}});
        }
        emit({{"type", "basket_execution_complete"},
              {"policy_sha256", hash_},
              {"basket_policy_sha256", basket_hash_},
              {"manifest_sha256", plan.manifest_sha256},
              {"records", records_},
              {"continuous_control_episode_onsets", episodes_},
              {"scenarios", rows},
              {"orders_sent", 0},
              {"scenario_results_are_additive", false},
              {"model_calibrated", false},
              {"continuity_failed", broken_},
              {"policy_expired", expired_},
              {"initial_capital_micro", capital_},
              {"actual_fills_observed", false}});
    }

  private:
    std::vector<core::BuyLevel> levels(const Scenario &s, Id id, bool yes, bool sell = false) const {
        std::vector<core::BuyLevel> result;
        const auto &m = markets_.at(id);
        if (!m.valid) {
            return result;
        }
        const bool ask = yes != sell;
        I collected = 0;
        for (const auto &l : m.sides[ask ? 1U : 0U]) {
            const auto used = s.consumed.find({id, ask, l.price});
            const I q = std::max<I>(0, l.quantity - (used == s.consumed.end() ? 0 : used->second));
            const I take = std::min(q, cap_ - collected);
            if (!take) {
                continue;
            }
            result.push_back(
                {*core::Price::from_raw(yes ? l.price : 10000 - l.price), *core::Quantity::from_raw(take)});
            collected += take;
            if (collected == cap_) {
                break;
            }
        }
        return result;
    }
    Fill fill(Scenario &s, Id id, bool yes, bool sell, I desired, I limit, bool apply) {
        Fill result;
        core::FeeAccumulator accumulator;
        for (const auto &l : levels(s, id, yes, sell)) {
            const I p = l.price.raw();
            if ((!sell && p > limit) || (sell && p < limit)) {
                break;
            }
            const I q = std::min(desired - result.quantity, l.quantity.raw());
            if (!q) {
                break;
            }
            I cash = 0;
            if (sell) {
                const auto f = core::credit_sell_fill(*core::Quantity::from_raw(q), l.price,
                                                      markets_.at(id).fee, accumulator);
                if (!f) {
                    detail::invalid("sell cost");
                }
                cash = f->credit.raw();
            } else {
                const auto f = core::charge_buy_fill(*core::Quantity::from_raw(q), l.price,
                                                     markets_.at(id).fee, accumulator);
                if (!f) {
                    detail::invalid("buy cost");
                }
                cash = f->debit.raw();
            }
            result.quantity += q;
            result.cash += cash;
            result.limit = p;
            result.levels.push_back({{"price_1e4", p}, {"quantity_centicontracts", q}, {"cash_micro", cash}});
            if (apply) {
                s.consumed[{id, yes != sell, yes ? p : 10000 - p}] += q;
            }
        }
        return result;
    }
    void decide(std::size_t index, I time) {
        auto &s = scenarios_[index];
        if (s.halted || s.attempts >= max_attempts_) {
            return;
        }
        if (s.attempt) {
            s.busy += signals_.size();
            return;
        }
        std::optional<core::SizedBasket> best;
        std::optional<Basket> basket;
        for (const auto &signal : signals_) {
            if (signal.left_censored) {
                ++s.rejected;
                continue;
            }
            const auto &b = baskets_.at(signal.basket_id);
            std::array<std::vector<core::BuyLevel>, 3> owned;
            std::array<core::BuyDepth, 3> depth;
            for (std::size_t leg = 0; leg < 3; ++leg) {
                owned[leg] = levels(s, b.ids[leg], leg == 0);
                depth[leg] = {owned[leg], markets_.at(b.ids[leg]).fee};
            }
            const core::SizingLimits bounds{*core::Quantity::from_raw(cap_), *core::Quantity::from_raw(100),
                                            *core::Cash::from_raw(s.cash), *core::Cash::from_raw(margin_),
                                            100};
            const auto result = core::size_buy_basket(depth, bounds);
            if (result.status == core::SizingStatus::invalid_input ||
                result.status == core::SizingStatus::arithmetic_error ||
                result.status == core::SizingStatus::search_budget_exceeded) {
                detail::invalid("execution sizing");
            }
            if (!result.quote) {
                ++s.rejected;
                continue;
            }
            if (!best || result.quote->net_margin_micro > best->net_margin_micro ||
                (result.quote->net_margin_micro == best->net_margin_micro && b.id < basket->id)) {
                best = result.quote;
                basket = b;
            }
        }
        if (!best) {
            return;
        }
        ++s.attempts;
        Attempt a;
        a.basket = *basket;
        a.quantity = best->quantity.raw();
        s.attempt = a;
        emit({{"type", "basket_execution_entry"},
              {"scenario", index},
              {"attempt", s.attempts},
              {"time_ns", time},
              {"basket_id", basket->id},
              {"quantity_centicontracts", a.quantity},
              {"quote_margin_micro", best->net_margin_micro}});
        send_buy(index, time);
    }
    I later(I time, I delay) const {
        if (time > std::numeric_limits<I>::max() - delay) {
            detail::invalid("event time overflow");
        }
        return time + delay;
    }
    void send_buy(std::size_t index, I time) {
        auto &s = scenarios_[index];
        auto &a = *s.attempt;
        I projected = a.spent;
        Fill current;
        for (std::size_t leg = a.leg; leg < 3; ++leg) {
            const auto f = fill(s, a.basket.ids[leg], leg == 0, false, a.quantity, 10000, false);
            if (f.quantity != a.quantity) {
                begin_exit(index, time, "remaining_depth_missing");
                return;
            }
            projected += f.cash;
            if (leg == a.leg) {
                current = f;
            }
        }
        if (projected > a.quantity * 20000 - margin_) {
            ++s.budget_aborts;
            begin_exit(index, time, "basket_cost_budget");
            return;
        }
        const auto reserve = core::buy_reservation(
            *core::Quantity::from_raw(a.quantity), *core::Quantity::from_raw(1),
            *core::Price::from_raw(current.limit), markets_.at(a.basket.ids[a.leg]).fee);
        if (!reserve || reserve->raw() > s.cash) {
            begin_exit(index, time, "funding_guard");
            return;
        }
        a.limit = current.limit;
        a.reserve = reserve->raw();
        s.reserved = a.reserve;
        s.peak_encumbered = std::max(s.peak_encumbered, capital_ - s.cash + s.reserved);
        a.stage = Stage::buy_arrival;
        a.due = later(time, s.delay);
        emit({{"type", "basket_execution_submit"},
              {"scenario", index},
              {"attempt", s.attempts},
              {"time_ns", time},
              {"arrival_ns", a.due},
              {"leg", a.leg},
              {"sell", false},
              {"quantity_centicontracts", a.quantity},
              {"limit_price_1e4", a.limit},
              {"projected_basket_cost_micro", projected}});
    }
    void begin_exit(std::size_t index, I time, const std::string &reason) {
        auto &s = scenarios_[index];
        auto &a = *s.attempt;
        a.reason = reason;
        a.leg = 0;
        s.reserved = 0;
        emit({{"type", "basket_execution_abort"},
              {"scenario", index},
              {"attempt", s.attempts},
              {"time_ns", time},
              {"reason", reason},
              {"holdings_centicontracts", a.holdings}});
        send_exit(index, time);
    }
    void send_exit(std::size_t index, I time) {
        auto &s = scenarios_[index];
        auto &a = *s.attempt;
        while (a.leg < 3 && !a.holdings[a.leg]) {
            ++a.leg;
        }
        if (a.leg == 3) {
            const bool residual =
                std::any_of(a.holdings.begin(), a.holdings.end(), [](I q) { return q != 0; });
            emit({{"type", "basket_execution_exit_complete"},
                  {"scenario", index},
                  {"attempt", s.attempts},
                  {"time_ns", time},
                  {"remaining_centicontracts", a.holdings},
                  {"net_cash_micro", a.received - a.spent},
                  {"residual", residual}});
            if (residual) {
                s.halted = true;
                return;
            }
            if (a.spent) {
                ++s.exited;
                s.unwind_net += a.received - a.spent;
            } else {
                ++s.empty;
            }
            s.attempt.reset();
            return;
        }
        a.stage = Stage::sell_arrival;
        a.due = later(time, s.delay);
        emit({{"type", "basket_execution_submit"},
              {"scenario", index},
              {"attempt", s.attempts},
              {"time_ns", time},
              {"arrival_ns", a.due},
              {"leg", a.leg},
              {"sell", true},
              {"quantity_centicontracts", a.holdings[a.leg]},
              {"limit_price_1e4", 0}});
    }
    void advance(std::size_t index, I time) {
        auto &s = scenarios_[index];
        while (s.attempt && !s.halted && s.attempt->due <= time) {
            auto &a = *s.attempt;
            const I at = a.due;
            if (a.stage == Stage::buy_arrival || a.stage == Stage::sell_arrival) {
                const bool sell = a.stage == Stage::sell_arrival;
                const auto f = fill(s, a.basket.ids[a.leg], a.leg == 0, sell,
                                    sell ? a.holdings[a.leg] : a.quantity, sell ? 0 : a.limit, true);
                if (sell) {
                    a.holdings[a.leg] -= f.quantity;
                    s.cash += f.cash;
                    s.received += f.cash;
                    a.received += f.cash;
                } else {
                    if (f.cash > a.reserve || f.cash > s.cash) {
                        detail::invalid("execution funding invariant");
                    }
                    a.holdings[a.leg] += f.quantity;
                    s.cash -= f.cash;
                    s.spent += f.cash;
                    a.spent += f.cash;
                    s.reserved = a.reserve - f.cash;
                }
                emit({{"type", "basket_execution_fill"},
                      {"scenario", index},
                      {"attempt", s.attempts},
                      {"time_ns", at},
                      {"leg", a.leg},
                      {"sell", sell},
                      {"quantity_centicontracts", f.quantity},
                      {"cash_micro", f.cash},
                      {"levels", f.levels},
                      {"own_fill_observed", false}});
                a.stage = sell ? Stage::sell_response : Stage::buy_response;
                a.due = later(at, s.delay);
            } else if (a.stage == Stage::buy_response) {
                s.reserved = 0;
                if (a.holdings[a.leg] != a.quantity) {
                    begin_exit(index, at, "partial_buy");
                } else if (++a.leg == 3) {
                    const I floor = a.quantity * 20000;
                    const I margin = floor - a.spent;
                    s.floor += floor;
                    s.complete_cost += a.spent;
                    ++s.completed;
                    if (margin < margin_) {
                        ++s.final_budget_breaches;
                    }
                    emit({{"type", "basket_execution_held"},
                          {"scenario", index},
                          {"attempt", s.attempts},
                          {"time_ns", at},
                          {"quantity_centicontracts", a.quantity},
                          {"debit_micro", a.spent},
                          {"conditional_floor_micro", floor},
                          {"conditional_margin_micro", margin}});
                    s.attempt.reset();
                } else {
                    send_buy(index, at);
                }
            } else {
                ++a.leg;
                send_exit(index, at);
            }
            if (s.cash != capital_ - s.spent + s.received || s.cash < 0 || s.reserved < 0 ||
                s.reserved > s.cash) {
                detail::invalid("execution cash ledger");
            }
        }
    }
    void censor(std::size_t i, I time, const std::string &reason) {
        auto &s = scenarios_[i];
        if (!s.attempt || s.halted) {
            return;
        }
        ++s.censored;
        s.halted = true;
        emit({{"type", "basket_execution_censored"},
              {"scenario", i},
              {"attempt", s.attempts},
              {"time_ns", time},
              {"reason", reason},
              {"holdings_centicontracts", s.attempt->holdings},
              {"unreleased_reservation_micro", s.reserved}});
    }
    void break_continuity(I time, const std::string &reason) {
        broken_ = true;
        for (std::size_t i = 0; i < scenarios_.size(); ++i) {
            censor(i, time, reason);
        }
    }
    void emit(const Json &row) {
        if (++trace_ > 100000) {
            detail::invalid("execution trace budget");
        }
        out_ << row.dump() << '\n';
        if (!out_) {
            detail::invalid("execution trace write");
        }
    }
    std::ostream &out_;
    std::unique_ptr<BasketObservation> control_;
    std::unordered_map<Id, Cached> markets_;
    std::map<std::uint32_t, Basket> baskets_;
    std::vector<Scenario> scenarios_;
    std::vector<BasketEpisode> signals_;
    I capital_{}, cap_{}, margin_{}, from_{}, until_{};
    std::uint64_t max_attempts_{}, records_{}, episodes_{}, trace_{};
    std::optional<I> last_time_, last_wall_;
    std::optional<market::ConnectionGeneration> generation_;
    std::string hash_, basket_hash_;
    bool broken_{}, expired_{}, finished_{};
};
} // namespace
std::unique_ptr<BasketExecutionStudy> make_basket_execution_study(const gateway::kalshi::MetadataSnapshot &m,
                                                                  const std::filesystem::path &b,
                                                                  const std::filesystem::path &p,
                                                                  std::ostream &out) {
    try {
        return std::make_unique<Study>(m, b, p, out);
    } catch (const Json::exception &) {
        detail::invalid("invalid basket execution JSON");
    }
}
std::optional<ReplayError> run_basket_execution_study(const ReplayInput &input,
                                                      const std::filesystem::path &b,
                                                      const std::filesystem::path &p, std::ostream &out) {
    try {
        if (!input.plan.ws_controller || !input.plan.shared_subscription) {
            detail::invalid("execution requires shared observed replay");
        }
        auto study = make_basket_execution_study(input.session.metadata, b, p, out);
        const auto r = replay(input, *study);
        if (const auto *e = std::get_if<ReplayError>(&r)) {
            return *e;
        }
        study->report(input.plan);
        return std::nullopt;
    } catch (const ReplayError &e) {
        return e;
    } catch (const std::exception &) {
        return ReplayError{"basket execution study failed", 0};
    }
}
} // namespace eme::session
