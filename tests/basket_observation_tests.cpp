#include "eme/session/basket_observation.hpp"
#include "session/session_files.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
using Json = nlohmann::json;
namespace session = eme::session;
namespace market = eme::market;
namespace kalshi = eme::gateway::kalshi;
constexpr std::int64_t ms = 1'000'000;

kalshi::MetadataSnapshot metadata() {
    return std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(R"({"schema_version":1,"metadata_version":7,"venue":"kalshi",
        "markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],"constraints":[]})"));
}
Json policy() {
    auto value = Json::parse(R"({"schema_version":1,"kind":"conditional_basket_observation","freshness_mode":"contiguous_shared_stream",
        "qualification_sha256":"0000000000000000000000000000000000000000000000000000000000000000",
        "metadata_sha256":"","valid_from_unix_ms":1000,"valid_until_unix_ms":2000,"max_episode_events":100,
        "screen":{"schema_version":1,"as_of_ms":0,"max_age_ms":1,"max_skew_ms":1,"max_total_evaluations":100,
        "markets":[{"id":1,"ticker":"LOW"},{"id":2,"ticker":"HIGH"},{"id":3,"ticker":"RANGE"}],
        "baskets":[{"id":7,"key":"range","lower_market_id":1,"upper_market_id":2,"range_market_id":3,
            "lower_threshold_cents":10000,"upper_threshold_cents":20000,"interval_lower_cents":10001,"interval_upper_cents":20000}],
        "sizing":{"cap_centicontracts":1000,"step_centicontracts":100,"available_cash_micro":1000000000,"minimum_margin_micro":0,"max_evaluations":10},
        "fees":[{"market_id":1,"coefficient_ppm":70000,"balance_quantum_micro":10000},{"market_id":2,"coefficient_ppm":70000,"balance_quantum_micro":10000},{"market_id":3,"coefficient_ppm":70000,"balance_quantum_micro":10000}],"books":[]}})");
    const auto snapshot = metadata();
    value["metadata_sha256"] = session::detail::fingerprint_bytes(snapshot.canonical_json()).sha256;
    return value;
}

class Fixture final {
public:
    explicit Fixture(const Json& input = policy()) : meta{std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(
        Json{{"schema_version", 1}, {"metadata_version", 7}, {"venue", "kalshi"}, {"markets", input["screen"]["markets"]}, {"constraints", Json::array()}}.dump()))} {
        directory = std::filesystem::temp_directory_path() / ("eme-basket-observer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(directory)) { throw std::runtime_error("fixture directory"); }
        const auto path = directory / "policy.json";
        std::ofstream file{path}; file << input.dump(); file.close();
        try {
            observer = session::make_basket_observation(meta, path, output); observer->start();
            if (!state.open_connection(generation)) { throw std::runtime_error("fixture open"); }
            tick(0);
        } catch (...) { std::error_code ignored; std::filesystem::remove_all(directory, ignored); throw; }
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    void tick(const std::int64_t time) {
        observer->before(time, state);
        observer->after({records++, time, {}, false, {}, 1000 * ms + time}, {}, state);
    }
    void snapshot(const std::int64_t time, const market::MarketId id, const std::int64_t bid, const std::int64_t ask) {
        observer->before(time, state);
        const market::BookSnapshot value{id, generation, 1U, ++sequence, market::ReceiveTime{std::chrono::nanoseconds{time}},
            {eme::test::level(bid, 1000)}, {eme::test::level(ask, 1000)}};
        if (std::get<eme::book::BookUpdateResult>(state.apply(value)) != eme::book::BookUpdateResult::applied) { throw std::runtime_error("fixture snapshot"); }
        observer->after({records++, time, id, true, {}, 1000 * ms + time}, {}, state);
    }
    void positive(const std::int64_t begin = ms) {
        snapshot(begin, 1, 100, 9800); snapshot(begin + ms, 2, 200, 9900); snapshot(begin + 2 * ms, 3, 9900, 10000);
    }
    std::vector<Json> lines() const {
        std::istringstream stream{output.str()}; std::string line; std::vector<Json> result;
        while (std::getline(stream, line)) { result.push_back(Json::parse(line)); }
        return result;
    }
    Json finish(const std::int64_t time) {
        observer->finish(time, state); session::ReplayPlan plan; plan.source_kind = "synthetic"; observer->report(plan);
        return lines().back();
    }
    kalshi::MetadataSnapshot meta;
    market::MarketState state{market::SequenceScope::shared_stream};
    std::filesystem::path directory;
    std::ostringstream output;
    std::unique_ptr<session::BasketObservation> observer;
    std::uint64_t records{}, sequence{}, generation{1};
};

void episodes(eme::test::Context& test) {
    Fixture f; f.positive(); f.tick(100 * ms); f.snapshot(110 * ms, 1, 100, 9800);
    f.snapshot(120 * ms, 3, 5000, 10000); f.snapshot(130 * ms, 3, 9900, 10000);
    test.expect(f.state.close_connection(1), "disconnect applied"); f.tick(140 * ms);
    f.generation = 2; f.sequence = 0;
    test.expect(f.state.open_connection(2), "reconnect generation applied"); f.tick(150 * ms);
    for (const auto id : {1U, 2U, 3U}) { test.expect(f.state.begin_recovery(id), "reconnect requires new snapshots"); }
    f.positive(151 * ms);
    const auto report = f.finish(160 * ms); const auto& basket = report["baskets"][0];
    test.expect(basket["episodes_opened"] == 3 && basket["episodes_closed"] == 3 &&
        basket["left_censored_episodes"] == 2 && basket["right_censored_episodes"] == 2,
        "startup/reconnect positive segments are left censored; disconnect/end are right censored");
    test.expect(basket["positive_conditional_ns"] == 134 * ms && basket["eligible_ns"] == 144 * ms && basket["no_data_ns"] == 16 * ms,
        "positive and eligible times are duration weighted, including quiet valid books");
    test.expect(report["any_positive_basket_union_ns"] == 134 * ms && report["maximum_simultaneous_positive_baskets"] == 1,
        "global positive time is a union rather than sum of independent basket margins");
    test.expect(basket["best_one_contract_diagnostic"]["net_margin_micro"] == 0 && basket["evaluations"] == 9,
        "quiet ping does not reevaluate and one-contract break-even does not hide larger profitable size");
    test.expect(report["orders_sent"] == 0 && !report["simulated_fills"].get<bool>() && report["realized_pnl_micro"].is_null() &&
        !report["episodes_are_independent_opportunities"].get<bool>(), "observation never reports trades, simulated fills or revenue");
    const auto lines = f.lines();
    const auto first_close = std::find_if(lines.begin(), lines.end(), [](const auto& row) { return row["type"] == "basket_episode_close"; });
    test.expect(first_close != lines.end() && (*first_close)["reason"] == "no_positive_margin" && !(*first_close)["right_censored"].template get<bool>(),
        "observed loss of positive margin is distinguished from missing information");
    test.expect(report["episode_events"] == 6, "positive quote updates retain one episode and emit no per-tick log");
}

void horizon_and_failure(eme::test::Context& test) {
    Fixture f; f.positive();
    test.expect(f.observer->next_event_time() == 1000 * ms, "wall validity is mapped to a recorded monotonic deadline");
    f.observer->before(2000 * ms, f.state);
    const auto report = f.finish(2000 * ms);
    test.expect(report["policy_expired"].get<bool>() && report["baskets"][0]["positive_conditional_ns"] == 997 * ms &&
        report["baskets"][0]["right_censored_episodes"] == 1 && !f.observer->next_event_time(),
        "silent stream horizon closes at the deadline, not at the later callback");
    auto changed = policy(); changed["max_episode_events"] = 1;
    Fixture limited{changed}; limited.positive(); bool rejected = false;
    try { (void)limited.finish(10 * ms); } catch (const session::ReplayError&) { rejected = true; }
    test.expect(rejected && limited.output.str().find("basket_complete") == std::string::npos,
        "episode event exhaustion cannot emit a complete/truncated-as-valid study");
    changed = policy(); changed["screen"]["sizing"]["max_evaluations"] = 1;
    Fixture budget{changed}; rejected = false;
    try { budget.positive(); } catch (const session::ReplayError&) { rejected = true; }
    test.expect(rejected, "incomplete quantity search fails instead of emitting partial optimum");
}

void policy_validation(eme::test::Context& test) {
    const auto reject = [&](Json value, const char* message) {
        bool rejected = false; try { Fixture f{value}; } catch (const session::ReplayError&) { rejected = true; }
        test.expect(rejected, message);
    };
    auto changed = policy(); changed["freshness_mode"] = "old_book_timeout"; reject(changed, "freshness interpretation is explicit and fixed");
    changed = policy(); changed["metadata_sha256"] = std::string(64, '0'); reject(changed, "metadata hash binding enforced");
    changed = policy(); changed["qualification_sha256"] = "bad"; reject(changed, "qualification fingerprint must be well formed");
    changed = policy(); changed["valid_until_unix_ms"] = 1000; reject(changed, "empty validity window rejected");
    changed = policy(); changed["valid_from_unix_ms"] = 1001; reject(changed, "future fee policy cannot be applied to earlier journal timestamps");
    changed = policy(); changed["screen"]["books"].push_back(Json::object()); reject(changed, "REST book seeding is forbidden");
    changed = policy(); changed["screen"]["markets"][0]["ticker"] = "OTHER"; reject(changed, "market ticker binding enforced");
    changed = policy(); changed["screen"]["baskets"][0]["lower_threshold_cents"] = 10001; reject(changed, "strict payoff boundary validated");
    changed = policy(); changed["screen"]["fees"].erase(0U); reject(changed, "fee coverage must be complete");
    changed = policy(); changed["screen"]["fees"][0]["balance_quantum_micro"] = 1; reject(changed, "unsupported fee precision rejected");
    changed = policy(); changed["max_episode_events"] = 0; reject(changed, "unbounded or empty event policy forbidden");
}

void overlapping_baskets(eme::test::Context& test) {
    auto input = policy();
    input["screen"]["markets"].push_back({{"id", 4}, {"ticker", "RANGE2"}});
    auto fee = input["screen"]["fees"][2]; fee["market_id"] = 4; input["screen"]["fees"].push_back(fee);
    auto basket = input["screen"]["baskets"][0]; basket["id"] = 8; basket["key"] = "range2";
    basket["range_market_id"] = 4; basket["interval_upper_cents"] = 19999; input["screen"]["baskets"].push_back(basket);
    const auto meta = std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(
        Json{{"schema_version", 1}, {"metadata_version", 7}, {"venue", "kalshi"}, {"markets", input["screen"]["markets"]}, {"constraints", Json::array()}}.dump()));
    input["metadata_sha256"] = session::detail::fingerprint_bytes(meta.canonical_json()).sha256;
    Fixture f{input}; f.positive(); f.snapshot(4 * ms, 4, 9900, 10000); f.snapshot(10 * ms, 3, 5000, 10000);
    const auto report = f.finish(20 * ms);
    test.expect(report["basket_evaluations"] == 7 && report["baskets"][0]["evaluations"] == 4 && report["baskets"][1]["evaluations"] == 3,
        "only baskets depending on the changed market are reevaluated");
    test.expect(report["maximum_simultaneous_positive_baskets"] == 2 && report["any_positive_basket_union_ns"] == 17 * ms &&
        report["baskets"][0]["positive_conditional_ns"] == 7 * ms && report["baskets"][1]["positive_conditional_ns"] == 16 * ms,
        "overlapping positive basket durations never become double-counted global opportunity time");
    test.expect(report["independent_quotes_share_liquidity"].get<bool>() && !report["joint_capital_allocation"].get<bool>() &&
        report["baskets"][0]["market_ids"] == Json::array({1, 2, 3}) && report["baskets"][1]["market_ids"] == Json::array({1, 2, 4}),
        "overlapping quote funding and depth remain independently priced and auditable");
}

void atomic_dependency_transition(eme::test::Context& test) {
    auto input = policy();
    input["screen"]["markets"].push_back({{"id", 4}, {"ticker", "LOWER"}});
    input["screen"]["markets"].push_back({{"id", 5}, {"ticker", "RANGE0"}});
    for (const auto id : {4, 5}) {
        auto fee = input["screen"]["fees"][0]; fee["market_id"] = id; input["screen"]["fees"].push_back(fee);
    }
    for (auto& fee : input["screen"]["fees"]) { fee["coefficient_ppm"] = 0; }
    auto basket = input["screen"]["baskets"][0];
    input["screen"]["baskets"][0]["id"] = 8;
    basket["key"] = "lower-range"; basket["lower_market_id"] = 4; basket["upper_market_id"] = 1; basket["range_market_id"] = 5;
    basket["lower_threshold_cents"] = 0; basket["upper_threshold_cents"] = 10000;
    basket["interval_lower_cents"] = 1; basket["interval_upper_cents"] = 10000;
    input["screen"]["baskets"].push_back(basket);
    const auto meta = std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(
        Json{{"schema_version", 1}, {"metadata_version", 7}, {"venue", "kalshi"}, {"markets", input["screen"]["markets"]}, {"constraints", Json::array()}}.dump()));
    input["metadata_sha256"] = session::detail::fingerprint_bytes(meta.canonical_json()).sha256;
    Fixture f{input}; f.snapshot(ms, 1, 4000, 6000); f.snapshot(2 * ms, 2, 5000, 6000);
    f.snapshot(3 * ms, 3, 3000, 4000); f.snapshot(4 * ms, 4, 5000, 6000); f.snapshot(5 * ms, 5, 1000, 2000);
    f.snapshot(10 * ms, 1, 8000, 9000);
    const auto report = f.finish(20 * ms);
    test.expect(report["maximum_simultaneous_positive_baskets"] == 1 && report["any_positive_basket_union_ns"] == 17 * ms,
        "a single book update opens one basket and closes another without a fictitious simultaneous peak");
}
}  // namespace

int main() {
    eme::test::Context test; episodes(test); horizon_and_failure(test); policy_validation(test); overlapping_baskets(test);
    atomic_dependency_transition(test); return test.result();
}
