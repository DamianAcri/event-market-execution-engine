#include "eme/session/passive_probe.hpp"
#include "eme/core/passive_queue.hpp"
#include "session/session_files.hpp"
#include "test_support.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <sstream>

namespace {
using Json = nlohmann::json;
using I = std::int64_t;
namespace session = eme::session;
namespace market = eme::market;
namespace kalshi = eme::gateway::kalshi;
constexpr I ms = 1'000'000;

class Fixture {
public:
    Fixture() : meta{std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(
        R"({"schema_version":1,"metadata_version":7,"venue":"kalshi","markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],"constraints":[]})"))} {
        directory = std::filesystem::temp_directory_path() / ("eme-passive-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory)) { throw std::runtime_error("fixture directory"); }
        basket = Json::parse(R"({"schema_version":1,"kind":"conditional_basket_observation","freshness_mode":"contiguous_shared_stream",
            "qualification_sha256":"0000000000000000000000000000000000000000000000000000000000000000","metadata_sha256":"",
            "valid_from_unix_ms":1000,"valid_until_unix_ms":60000,"max_episode_events":100,
            "screen":{"schema_version":1,"as_of_ms":0,"max_age_ms":1,"max_skew_ms":1,"max_total_evaluations":100,
                "markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],
                "baskets":[{"id":7,"key":"range","lower_market_id":1,"upper_market_id":2,"range_market_id":3,
                    "lower_threshold_cents":10000,"upper_threshold_cents":20000,"interval_lower_cents":10001,"interval_upper_cents":20000}],
                "sizing":{"cap_centicontracts":100,"step_centicontracts":100,"available_cash_micro":10000000,"minimum_margin_micro":0,"max_evaluations":1},
                "fees":[{"market_id":1,"coefficient_ppm":0,"balance_quantum_micro":10000},{"market_id":2,"coefficient_ppm":0,"balance_quantum_micro":10000},{"market_id":3,"coefficient_ppm":0,"balance_quantum_micro":10000}],"books":[]}})");
        basket["metadata_sha256"] = session::detail::fingerprint_bytes(meta.canonical_json()).sha256;
        probe = {{"schema_version", 1}, {"kind", "passive_basket_probe"}, {"basket_policy_sha256", ""},
            {"entry_delay_ms", 1}, {"rest_ms", 20}, {"cancel_delay_ms", 1}, {"hedge_delays_ms", Json::array({1, 10, 100})},
            {"reconcile_ms", 5}, {"max_gap_ms", 1000}, {"trade_lookback_ms", 10000}, {"max_spread_1e4", 500},
            {"minimum_margin_micro", 1}, {"maker_coefficient_ppm", 0}, {"max_attempts", 10}, {"assumed_clock_error_ms", 0}};
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    void start(bool bad_hash = false) {
        const auto path = directory / "basket.json";
        { std::ofstream f{path, std::ios::binary}; f << basket.dump(); }
        probe["basket_policy_sha256"] = bad_hash ? std::string(64, '0') : session::detail::fingerprint_bytes(basket.dump()).sha256;
        { std::ofstream f{directory / "probe.json", std::ios::binary}; f << probe.dump(); }
        observer = session::make_passive_probe(meta, path, directory / "probe.json", output);
        if (!state.open_connection(1)) { throw std::runtime_error("fixture connection"); }
        tick(0);
    }
    void tick(I t) { observer->before(t, state); observer->after({++records, t, {}, false, {}, 1000 * ms + t}, {}, state); }
    void snapshot(I t, market::MarketId id, I bid, I ask, I quantity = 100) {
        observer->before(t, state);
        const market::BookSnapshot book{id, 1, 1, ++sequence, market::ReceiveTime{std::chrono::nanoseconds{t}},
            {eme::test::level(bid, quantity)}, {eme::test::level(ask, quantity)}};
        if (std::get<eme::book::BookUpdateResult>(state.apply(book)) != eme::book::BookUpdateResult::applied) { throw std::runtime_error("fixture book"); }
        observer->after({++records, t, id, true, {}, 1000 * ms + t}, {}, state);
    }
    void trade(I t, I q, std::string id, std::optional<bool> block = false, bool taker_yes = false, I exchange_ms = -1) {
        observer->before(t, state);
        market::PublicTrade trade{1, std::move(id), 4900, q,
            static_cast<std::uint64_t>(exchange_ms < 0 ? 1000 + t / ms : exchange_ms), taker_yes, block};
        observer->after({++records, t, 1, false, trade, 1000 * ms + t}, {}, state);
    }
    void books() { snapshot(ms, 1, 4900, 5000); snapshot(2 * ms, 2, 3000, 3100); snapshot(3 * ms, 3, 2000, 2100); }
    void signal() { books(); trade(4 * ms, 1, "prior"); snapshot(5 * ms, 1, 4900, 5000); tick(6 * ms); }
    Json finish(I time) {
        observer->finish(time, state); session::ReplayPlan p; p.source_kind = "synthetic"; observer->report(p);
        return lines().back();
    }
    std::vector<Json> lines() const {
        std::istringstream in{output.str()}; std::string line; std::vector<Json> result;
        while (std::getline(in, line)) { result.push_back(Json::parse(line)); }
        return result;
    }
    kalshi::MetadataSnapshot meta;
    market::MarketState state{market::SequenceScope::shared_stream};
    std::filesystem::path directory;
    Json basket, probe;
    std::ostringstream output;
    std::unique_ptr<session::PassiveProbe> observer;
    std::uint64_t records{}, sequence{};
};

void queue_cases(eme::test::Context& test) {
    for (bool favorable : {false, true}) {
        eme::core::PassiveQueue q{100, favorable, 5};
        q.depth(1, 0);
        test.expect(q.trade(2, 100, 100) == 0, "delta before matching trade must not double-count queue depletion");
        q.advance(10); test.expect(q.ahead() == 0 && q.trade(11, 40, 100) == 40, "only subsequent excess trade creates partial fill");
        eme::core::PassiveQueue reverse{100, favorable, 5};
        test.expect(reverse.trade(1, 60, 100) == 0, "trade consumes queue ahead first");
        reverse.depth(2, 40); reverse.advance(10);
        test.expect(reverse.ahead() == 40, "trade-before-delta reconciliation preserves remaining queue");
        eme::core::PassiveQueue cancellation{100, favorable, 5};
        cancellation.depth(1, 20); cancellation.advance(7);
        test.expect(cancellation.ahead() == (favorable ? 20 : 100), "unmatched reduction affects only favorable priority scenario");
        test.expect(cancellation.trade(8, 30, 100) == (favorable ? 10 : 0), "book reduction alone never creates a fill");
        eme::core::PassiveQueue additions{100, favorable, 5};
        additions.depth(1, 300); test.expect(additions.ahead() == 100, "new same-price liquidity joins behind resting order");
    }
}
void economic_cases(eme::test::Context& test) {
    Fixture f; f.start(); f.signal(); f.trade(7 * ms, 200, "fill");
    f.snapshot(12 * ms, 2, 1000, 1100); // Later hedge becomes 20 cents more expensive.
    f.tick(220 * ms); const auto report = f.finish(220 * ms);
    test.expect(report["attempts"] == 1 && report["scenarios"].size() == 6, "one funded entry with six separate sensitivity paths");
    const auto& fast = report["scenarios"][0]; const auto& delayed = report["scenarios"][2];
    test.expect(fast["conditional_margin_micro"] == 10000 && delayed["conditional_margin_micro"] == -190000,
        "hedge uses prices known at its due time; adverse later prices cannot leak into earlier hedge");
    test.expect(fast["cash_spent_micro"] == 1990000 && fast["cash_remaining_micro"] == 8010000,
        "complete basket debit is funded; conditional payout is never credited as available cash");
    test.expect(report["orders_sent"] == 0 && report["realized_pnl_micro"].is_null() && !report["own_fills_observed"].get<bool>(), "counterfactual results cannot report actual trading profit");
    Fixture partial; partial.start(); partial.signal(); partial.trade(7 * ms, 150, "partial"); partial.tick(300 * ms);
    const auto partial_report = partial.finish(300 * ms);
    test.expect(partial_report["scenarios"][0]["cash_spent_micro"] == 1000000 && partial_report["scenarios"][0]["completed_baskets"] == 1,
        "partial passive fill hedges only filled quantity after rest and cancellation delay, including rounding");
    Fixture unknown; unknown.start(); unknown.signal(); unknown.trade(7 * ms, 200, "unknown", {}); unknown.trade(8 * ms, 200, "block", true);
    unknown.trade(9 * ms, 200, "wrong-side", false, true); unknown.trade(10 * ms, 200, "stale", false, false, 1005);
    unknown.tick(300 * ms); const auto excluded = unknown.finish(300 * ms);
    test.expect(excluded["scenarios"][0]["modeled_fill_events"] == 0, "unknown/block, wrong-side and preactivation exchange trades cannot fill entry");
    Fixture duplicate; duplicate.start(); duplicate.signal(); duplicate.trade(7 * ms, 100, "same"); duplicate.trade(8 * ms, 100, "same"); duplicate.tick(300 * ms);
    test.expect(duplicate.finish(300 * ms)["scenarios"][0]["modeled_fill_events"] == 0, "duplicate public trade cannot consume priority twice");
    Fixture clock; clock.probe["assumed_clock_error_ms"] = 5; clock.start(); clock.signal();
    clock.trade(7 * ms, 200, "ambiguous", false, false, 1008);
    clock.trade(12 * ms, 200, "bounded-skew", false, false, 1015); clock.tick(300 * ms);
    const auto c = clock.finish(300 * ms);
    test.expect(c["scenarios"][0]["modeled_fill_events"] == 1 && c["receive_minus_exchange_ms_min"] == -3,
        "bounded exchange-clock skew is explicit; activation-ambiguous trades cannot consume queue or fill");
    Fixture badclock; badclock.probe["assumed_clock_error_ms"] = 5; badclock.start(); badclock.signal();
    badclock.trade(7 * ms, 200, "outside-clock-bound", false, false, 1020); badclock.tick(300 * ms);
    const auto bc = badclock.finish(300 * ms);
    test.expect(bc["scenarios"][0]["modeled_fill_events"] == 0 && bc["trade_exclusion_reasons"]["clock_bound_exceeded"] == 1,
        "trades beyond declared clock bound are excluded and reported");
}
void failure_cases(eme::test::Context& test) {
    Fixture missing; missing.start(); missing.books(); missing.tick(500 * ms);
    test.expect(missing.finish(500 * ms)["attempts"] == 0, "positive static quote without prior public-trade support does not start an order");
    Fixture eof; eof.start(); eof.signal(); eof.trade(7 * ms, 200, "fill"); const auto e = eof.finish(7 * ms);
    test.expect(e["scenarios"][0]["completed_baskets"] == 0 && e["scenarios"][0]["halted_on_residual"].get<bool>(), "EOF before hedge preserves unresolved inventory without future fills");
    Fixture gap; gap.start(); gap.signal(); gap.trade(7 * ms, 200, "fill"); gap.tick(2000 * ms); const auto g = gap.finish(2000 * ms);
    test.expect(g["continuity_failed"].get<bool>() && g["scenarios"][0]["completed_baskets"] == 0, "suspension gap censors before advancing stale-book hedge timers");
    Fixture disconnect; disconnect.start(); disconnect.signal(); disconnect.trade(7 * ms, 150, "partial");
    test.expect(disconnect.state.close_connection(1), "fixture disconnect"); disconnect.tick(8 * ms);
    test.expect(disconnect.finish(8 * ms)["scenarios"][0]["unresolved_inventory_centicontracts"] == 50, "disconnect keeps partial exposure and stops further entries");
    Fixture wall; wall.start(); wall.signal(); wall.trade(7 * ms, 200, "fill");
    wall.observer->before(9 * ms, wall.state);
    wall.observer->after({++wall.records, 9 * ms, {}, false, {}, 3000 * ms}, {}, wall.state);
    const auto w = wall.finish(9 * ms);
    test.expect(w["continuity_failed"].get<bool>() && w["scenarios"][0]["completed_baskets"] == 0,
        "wall-clock suspension is checked before hedge timers even when monotonic time barely advances");
    Fixture depth; depth.start(); depth.signal(); depth.trade(7 * ms, 200, "fill"); depth.snapshot(8 * ms, 3, 2000, 2100, 10); depth.tick(300 * ms);
    const auto d = depth.finish(300 * ms);
    test.expect(d["scenarios"][0]["halted_on_residual"].get<bool>() && d["scenarios"][0]["completed_baskets"] == 0, "partial hedge depth never becomes a complete portfolio");
    Fixture funds; funds.basket["screen"]["sizing"]["available_cash_micro"] = 100000; funds.start(); funds.signal();
    test.expect(funds.finish(7 * ms)["attempts"] == 0, "full conservative reservation gates entry size");
    Fixture binding; bool rejected = false; try { binding.start(true); } catch (const session::ReplayError&) { rejected = true; }
    test.expect(rejected, "probe is bound to exact validated basket policy");
}
} // namespace
int main() { eme::test::Context test; queue_cases(test); economic_cases(test); failure_cases(test); return test.result(); }
