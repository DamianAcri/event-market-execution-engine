#include "eme/session/basket_execution.hpp"
#include "session/session_files.hpp"
#include "test_support.hpp"
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
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
    Fixture()
        : meta{std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(
              R"({"schema_version":1,"metadata_version":7,"venue":"kalshi","markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],"constraints":[]})"))} {
        directory = std::filesystem::temp_directory_path() /
                    ("eme-basket-execution-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory)) {
            throw std::runtime_error("fixture directory");
        }
        basket = Json::parse(
            R"({"schema_version":1,"kind":"conditional_basket_observation","freshness_mode":"contiguous_shared_stream",
            "qualification_sha256":"0000000000000000000000000000000000000000000000000000000000000000","metadata_sha256":"",
            "valid_from_unix_ms":1000,"valid_until_unix_ms":60000,"max_episode_events":100,
            "screen":{"schema_version":1,"as_of_ms":0,"max_age_ms":1,"max_skew_ms":1,"max_total_evaluations":100,
                "markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],
                "baskets":[{"id":7,"key":"range","lower_market_id":1,"upper_market_id":2,"range_market_id":3,
                    "lower_threshold_cents":10000,"upper_threshold_cents":20000,"interval_lower_cents":10001,"interval_upper_cents":20000}],
                "sizing":{"cap_centicontracts":100,"step_centicontracts":100,"available_cash_micro":10000000,"minimum_margin_micro":0,"max_evaluations":1},
                "fees":[{"market_id":1,"coefficient_ppm":0,"balance_quantum_micro":10000},{"market_id":2,"coefficient_ppm":0,"balance_quantum_micro":10000},{"market_id":3,"coefficient_ppm":0,"balance_quantum_micro":10000}],"books":[]}})");
        basket["metadata_sha256"] = session::detail::fingerprint_bytes(meta.canonical_json()).sha256;
        probe = {{"schema_version", 1},        {"kind", "chronological_basket_execution"},
                 {"basket_policy_sha256", ""}, {"arrival_and_response_ms", Json::array({1})},
                 {"minimum_margin_micro", 1},  {"max_attempts", 10}};
    }
    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void start(bool bad_hash = false) {
        const auto path = directory / "basket.json";
        {
            std::ofstream f{path, std::ios::binary};
            f << basket.dump();
        }
        probe["basket_policy_sha256"] =
            bad_hash ? std::string(64, '0') : session::detail::fingerprint_bytes(basket.dump()).sha256;
        {
            std::ofstream f{directory / "probe.json", std::ios::binary};
            f << probe.dump();
        }
        observer = session::make_basket_execution_study(meta, path, directory / "probe.json", output);
        if (!state.open_connection(1)) {
            throw std::runtime_error("fixture connection");
        }
        tick(0);
    }
    void tick(I t) {
        observer->before(t, state);
        observer->after({++records, t, {}, false, {}, 1000 * ms + t}, {}, state);
    }
    void snapshot(I t, market::MarketId id, I bid, I ask, I quantity = 100, I bid_quantity = -1) {
        observer->before(t, state);
        const market::BookSnapshot book{id,
                                        1,
                                        1,
                                        ++sequence,
                                        market::ReceiveTime{std::chrono::nanoseconds{t}},
                                        {eme::test::level(bid, bid_quantity < 0 ? quantity : bid_quantity)},
                                        {eme::test::level(ask, quantity)}};
        if (std::get<eme::book::BookUpdateResult>(state.apply(book)) !=
            eme::book::BookUpdateResult::applied) {
            throw std::runtime_error("fixture book");
        }
        observer->after({++records, t, id, true, {}, 1000 * ms + t}, {}, state);
    }
    void signal() {
        snapshot(ms, 1, 4900, 5000);
        snapshot(2 * ms, 2, 3000, 3100);
        snapshot(3 * ms, 3, 2000, 2100);
        snapshot(4 * ms, 3, 2100, 2200);
    }
    Json finish(I time) {
        observer->finish(time, state);
        session::ReplayPlan p;
        p.source_kind = "synthetic";
        observer->report(p);
        return lines().back();
    }
    std::vector<Json> lines() const {
        std::istringstream in{output.str()};
        std::string line;
        std::vector<Json> result;
        while (std::getline(in, line)) {
            result.push_back(Json::parse(line));
        }
        return result;
    }
    kalshi::MetadataSnapshot meta;
    market::MarketState state{market::SequenceScope::shared_stream};
    std::filesystem::path directory;
    Json basket, probe;
    std::ostringstream output;
    std::unique_ptr<session::BasketExecutionStudy> observer;
    std::uint64_t records{}, sequence{};
};

const Json &scenario(const Json &report) { return report.at("scenarios").at(0); }
void success_and_funding(eme::test::Context &test) {
    Fixture f;
    f.start();
    f.signal();
    f.tick(11 * ms);
    const auto r = f.finish(11 * ms);
    const auto &s = scenario(r);
    test.expect(r["continuous_control_episode_onsets"] == 1 && s["completed_baskets"] == 1,
                "continuous onset drives one complete chronological entry");
    test.expect(s["cash_micro"] == 8010000 && s["conditional_net_micro"] == 10000,
                "completed payout stays locked; only conditional equity includes floor");
    std::vector<I> times;
    for (const auto &e : f.lines()) {
        if (e["type"] == "basket_execution_fill") {
            times.push_back(e["time_ns"].get<I>());
        }
    }
    test.expect(times == std::vector<I>{5 * ms, 7 * ms, 9 * ms},
                "sequential buys wait for acknowledgments: d, 3d, 5d");
    test.expect(s["cash_identity_verified"] == true && s["realized_pnl_micro"].is_null() &&
                    r["orders_sent"] == 0,
                "ledger and no-real-order contract");
    Fixture funds;
    funds.basket["screen"]["sizing"]["available_cash_micro"] = 6000000;
    funds.start();
    funds.signal();
    funds.tick(11 * ms);
    funds.snapshot(12 * ms, 3, 2000, 2100, 200);
    funds.snapshot(13 * ms, 1, 4900, 5000, 200);
    funds.snapshot(14 * ms, 2, 3000, 3100, 200);
    funds.snapshot(15 * ms, 3, 2100, 2200, 200);
    funds.tick(25 * ms);
    const auto fr = funds.finish(25 * ms);
    test.expect(fr["continuous_control_episode_onsets"] == 2 && scenario(fr)["attempts"] == 1,
                "locked capital cannot fund a second gross reservation");
}
void execution_failures(eme::test::Context &test) {
    Fixture budget;
    budget.start();
    budget.signal();
    budget.snapshot(5 * ms + ms / 2, 2, 1000, 1100);
    budget.tick(20 * ms);
    const auto b = budget.finish(20 * ms);
    const auto &bs = scenario(b);
    test.expect(bs["budget_aborts"] == 1 && bs["completed_baskets"] == 0 && bs["fully_unwound_attempts"] == 1,
                "adverse remaining price triggers paid unwind rather than pretend positive completion");
    test.expect(bs["conditional_net_micro"] == -10000 && bs["modeled_unwind_pnl_micro"] == -10000,
                "sell spread loss affects total equity and is not ignored");
    Fixture partial;
    partial.start();
    partial.signal();
    partial.snapshot(4 * ms + ms / 2, 1, 4900, 5000, 40);
    partial.tick(20 * ms);
    const auto p = partial.finish(20 * ms);
    test.expect(scenario(p)["fully_unwound_attempts"] == 1 && scenario(p)["residual_inventory"].empty(),
                "partial buy sells only acquired quantity");
    Fixture residual;
    residual.start();
    residual.signal();
    residual.snapshot(4 * ms + ms / 2, 1, 4900, 5000, 40, 10);
    residual.tick(20 * ms);
    const auto rr = residual.finish(20 * ms);
    const auto &rs = scenario(rr);
    test.expect(rs["halted"] == true && rs["residual_inventory"][0]["quantity_centicontracts"] == 30 &&
                    rs["conditional_net_micro"].is_null(),
                "partial exit preserves inventory, halts, and cannot report resolved profit");
    test.expect(rs["cash_identity_verified"] == true, "partial exit conserves exact cash");
    Fixture limit;
    limit.start();
    limit.signal();
    limit.snapshot(6 * ms + ms / 2, 2, 1000, 1100);
    limit.tick(20 * ms);
    const auto lr = limit.finish(20 * ms);
    test.expect(scenario(lr)["fully_unwound_attempts"] == 1 && scenario(lr)["buy_debits_micro"] == 500000,
                "IOC cannot cross a worse price after submission");
}
void liquidity_and_busy(eme::test::Context &test) {
    Fixture f;
    f.start();
    f.signal();
    f.tick(11 * ms);
    f.snapshot(12 * ms, 3, 2000, 2100);
    f.snapshot(13 * ms, 3, 2100, 2200);
    f.tick(25 * ms);
    const auto r = f.finish(25 * ms);
    test.expect(r["continuous_control_episode_onsets"] == 2 && scenario(r)["attempts"] == 1,
                "own consumed liquidity is not replenished by repeated historical snapshots");
    Fixture busy;
    busy.start();
    busy.signal();
    busy.snapshot(4 * ms + ms / 4, 3, 2000, 2100);
    busy.snapshot(4 * ms + ms / 2, 3, 2100, 2200);
    busy.tick(20 * ms);
    const auto br = busy.finish(20 * ms);
    test.expect(br["continuous_control_episode_onsets"] == 2 &&
                    scenario(br)["episode_onsets_skipped_busy"] == 1 && scenario(br)["attempts"] == 1,
                "continuous aggressive control remains active while an attempt is busy");
    Fixture left;
    left.start();
    left.snapshot(ms, 1, 4900, 5000);
    left.snapshot(2 * ms, 2, 3000, 3100);
    left.snapshot(3 * ms, 3, 2100, 2200);
    left.tick(20 * ms);
    const auto l = left.finish(20 * ms);
    test.expect(l["continuous_control_episode_onsets"] == 1 && scenario(l)["attempts"] == 0,
                "left-censored initial quote is observed but not a new known onset");
}
void within_limit_cost_and_expiry(eme::test::Context &test) {
    Fixture f;
    for (auto &fee : f.basket["screen"]["fees"]) {
        fee["balance_quantum_micro"] = 100;
    }
    f.start();
    f.snapshot(ms, 1, 4900, 5000);
    f.snapshot(2 * ms, 2, 3000, 3100);
    f.snapshot(3 * ms, 3, 2000, 2100);
    const I time = 4 * ms;
    f.observer->before(time, f.state);
    const market::BookSnapshot multi{3,
                                     1,
                                     1,
                                     ++f.sequence,
                                     market::ReceiveTime{std::chrono::nanoseconds{time}},
                                     {eme::test::level(2100, 50), eme::test::level(2000, 50)},
                                     {eme::test::level(2200, 100)}};
    const auto update = f.state.apply(multi);
    test.expect(std::get<eme::book::BookUpdateResult>(update) == eme::book::BookUpdateResult::applied,
                "multi-level fixture");
    f.observer->after({++f.records, time, 3, true, {}, 1000 * ms + time}, {}, f.state);
    f.tick(8 * ms);
    f.snapshot(8 * ms + ms / 2, 3, 2000, 2100);
    f.tick(11 * ms);
    const auto r = f.finish(11 * ms);
    test.expect(scenario(r)["completed_budget_breaches"] == 1 && scenario(r)["conditional_net_micro"] == 0,
                "within-limit depth changes may breach projected basket budget; actual cost is retained");
    Fixture expired;
    expired.basket["valid_until_unix_ms"] = 1005;
    expired.start();
    expired.signal();
    expired.tick(6 * ms);
    const auto e = expired.finish(6 * ms);
    test.expect(e["policy_expired"] == true && e["continuity_failed"] == false &&
                    scenario(e)["buy_debits_micro"] == 0,
                "expiry is distinct from continuity failure and censors pending arrivals");
}
void missing_observation(eme::test::Context &test) {
    Fixture eof;
    eof.start();
    eof.signal();
    eof.tick(5 * ms);
    const auto e = eof.finish(5 * ms);
    test.expect(scenario(e)["completed_baskets"] == 0 && scenario(e)["conditional_net_micro"].is_null() &&
                    scenario(e)["residual_inventory"][0]["quantity_centicontracts"] == 100,
                "EOF does not execute future acknowledgment or remaining legs");
    for (bool wall_gap : {false, true}) {
        Fixture gap;
        gap.start();
        gap.signal();
        const I t = wall_gap ? 6 * ms : 20000 * ms;
        gap.observer->before(t, gap.state);
        gap.observer->after({++gap.records, t, {}, false, {}, 21000 * ms}, {}, gap.state);
        const auto g = gap.finish(t);
        test.expect(g["continuity_failed"] == true && scenario(g)["buy_debits_micro"] == 0,
                    "clock gaps censor before stale-book timer execution");
    }
    Fixture closed;
    closed.start();
    closed.signal();
    test.expect(closed.state.close_connection(1), "fixture close");
    closed.tick(6 * ms);
    const auto c = closed.finish(6 * ms);
    test.expect(scenario(c)["buy_debits_micro"] == 0 && scenario(c)["censored_attempts"] == 1,
                "disconnect cannot fill pending orders");
    Fixture bound;
    bool bad = false;
    try {
        bound.start(true);
    } catch (const session::ReplayError &) {
        bad = true;
    }
    test.expect(bad, "policy binding rejects changed basket");
}
} // namespace
int main() {
    eme::test::Context t;
    success_and_funding(t);
    execution_failures(t);
    liquidity_and_busy(t);
    within_limit_cost_and_expiry(t);
    missing_observation(t);
    return t.result();
}
