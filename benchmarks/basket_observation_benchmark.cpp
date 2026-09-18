#include "eme/core/basket_sizing.hpp"
#include "eme/session/basket_observation.hpp"
#include "session/study_json.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
namespace core = eme::core;
namespace market = eme::market;
namespace session = eme::session;
using Json = nlohmann::json;
constexpr std::int64_t wall_start = 1'800'000'000'000'000'000LL;
constexpr std::int64_t step_ns = 1'000'000;
constexpr std::int64_t cap = 10'000;

core::Price price(const std::int64_t value) { return *core::Price::from_raw(value); }
core::Quantity quantity(const std::int64_t value) { return *core::Quantity::from_raw(value); }

void require(const bool condition, const char* message) {
    if (!condition) { throw std::runtime_error{message}; }
}

class Fixture final {
public:
    explicit Fixture(const std::size_t baskets) {
        root = std::filesystem::temp_directory_path() / ("eme-basket-observer-bench-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(root), "temporary directory already exists");
        const std::size_t uppers = baskets == 1U ? 1U : 5U;
        const std::size_t ranges = baskets == 1U ? 1U : 20U;
        const auto market_count = 1U + uppers + ranges;
        market_rows = Json::array();
        Json fees = Json::array(), rows = Json::array();
        for (std::size_t i = 0; i < market_count; ++i) {
            const auto id = static_cast<market::MarketId>(i + 1U);
            market_rows.push_back({{"id", id}, {"ticker", "SYNTHETIC-" + std::to_string(id)}});
            fees.push_back({{"market_id", id}, {"coefficient_ppm", 70'000U}, {"balance_quantum_micro", 10'000U}});
        }
        first_range = static_cast<market::MarketId>(uppers + 2U);
        for (std::size_t range = 0; range < ranges; ++range) {
            for (std::size_t upper = 0; upper < uppers; ++upper) {
                const auto id = static_cast<std::uint32_t>(rows.size() + 1U);
                rows.push_back({{"id", id}, {"key", "synthetic-" + std::to_string(id)},
                    {"lower_market_id", 1U}, {"upper_market_id", upper + 2U},
                    {"range_market_id", uppers + 2U + range}, {"lower_threshold_cents", 10'000U},
                    {"upper_threshold_cents", 20'000U + upper * 1'000U},
                    {"interval_lower_cents", 11'000U + range}, {"interval_upper_cents", 12'000U + range}});
            }
        }
        auto parsed = eme::gateway::kalshi::parse_metadata_snapshot(Json{{"schema_version", 1U},
            {"metadata_version", 1U}, {"venue", "kalshi"}, {"markets", market_rows}, {"constraints", Json::array()}}.dump());
        metadata = std::get<eme::gateway::kalshi::MetadataSnapshot>(std::move(parsed));
        screen = {{"schema_version", 1U}, {"markets", market_rows}, {"baskets", rows}, {"books", Json::array()},
            {"fees", fees}, {"as_of_ms", 0U}, {"max_age_ms", 0U}, {"max_skew_ms", 0U},
            {"max_total_evaluations", baskets * 100U},
            {"sizing", {{"cap_centicontracts", cap}, {"step_centicontracts", 100U},
                {"available_cash_micro", 10'000'000'000ULL}, {"minimum_margin_micro", 0U}, {"max_evaluations", 100U}}}};
        policy = {{"schema_version", 1U}, {"kind", "conditional_basket_observation"},
            {"freshness_mode", "contiguous_shared_stream"}, {"qualification_sha256", std::string(64U, 'a')},
            {"metadata_sha256", session::detail::fingerprint_bytes(metadata->canonical_json()).sha256},
            {"valid_from_unix_ms", wall_start / 1'000'000}, {"valid_until_unix_ms", wall_start / 1'000'000 + 3'600'000},
            {"max_episode_events", 1000U}, {"screen", screen}};
        std::ofstream file{root / "policy.json"}; file << policy.dump() << '\n';
        require(static_cast<bool>(file), "write policy");
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    std::filesystem::path root;
    std::optional<eme::gateway::kalshi::MetadataSnapshot> metadata;
    Json market_rows, screen, policy;
    market::MarketId first_range{};
};

Json final_report(const std::string& output) {
    Json last;
    std::istringstream stream{output};
    for (std::string line; std::getline(stream, line);) { last = Json::parse(line); }
    require(last.value("type", "") == "basket_complete", "missing complete observer report");
    return last;
}

std::vector<core::SizedBasket> full_rescreen(const Fixture& fixture, const market::MarketState& state) {
    const core::SizingLimits limits{quantity(cap), quantity(100), *core::Cash::from_raw(10'000'000'000LL),
        *core::Cash::from_raw(0), 100U};
    std::vector<core::SizedBasket> result;
    for (const auto& basket : fixture.screen.at("baskets")) {
        std::array<std::vector<core::BuyLevel>, 3U> storage;
        std::array<core::BuyDepth, 3U> depth;
        const std::array<std::string_view, 3U> fields{"lower_market_id", "upper_market_id", "range_market_id"};
        for (std::size_t leg = 0; leg < 3U; ++leg) {
            const auto* book = state.find_book(basket.at(fields[leg]).get<market::MarketId>());
            require(book != nullptr, "full-scan missing market");
            book->visit_levels(leg == 0U ? eme::book::Side::ask : eme::book::Side::bid, [&](const eme::book::Level level) {
                storage[leg].push_back({price(leg == 0U ? level.price.raw() : 10'000 - level.price.raw()), level.quantity});
                return true;
            });
            depth[leg] = {storage[leg], {70'000U, 10'000U}};
        }
        const auto sized = core::size_buy_basket(depth, limits);
        require(sized.status == core::SizingStatus::optimal && sized.quote.has_value(), "fixture lost positive conditional quote");
        result.push_back(*sized.quote);
    }
    return result;
}

struct Run final { double mean_ns{}, maximum_ns{}; Json economic_result; };

Run run(const Fixture& fixture, const std::size_t count, const bool trade_only, const bool common_lower) {
    market::MarketState state;
    require(state.open_connection(1U), "open synthetic generation");
    std::ostringstream output;
    auto observer = session::make_basket_observation(*fixture.metadata, fixture.root / "policy.json", output);
    observer->start();
    std::int64_t time = 0;
    std::uint64_t ordinal = 0;
    std::vector<std::uint64_t> sequences(fixture.market_rows.size() + 1U, 1U);
    for (const auto& row : fixture.market_rows) {
        const auto id = row.at("id").get<market::MarketId>();
        time += step_ns;
        observer->before(time, state);
        market::BookSnapshot snapshot{id, 1U, id, 1U, market::ReceiveTime{std::chrono::nanoseconds{time}}, {}, {}};
        for (std::int64_t level = 0; level < 8; ++level) {
            snapshot.bids.push_back({price(5000 - level * 10), quantity(2000)});
            snapshot.asks.push_back({price(5100 + level * 10), quantity(2000)});
        }
        require(std::get<eme::book::BookUpdateResult>(state.apply(snapshot)) == eme::book::BookUpdateResult::applied,
                "synthetic snapshot rejected");
        observer->after({ordinal++, time, id, true, {}, wall_start + time}, {}, state);
    }
    const auto initial = full_rescreen(fixture, state);
    const auto changed = common_lower ? 1U : fixture.first_range;
    auto positive_variant = initial;
    if (!trade_only) {
        market::BookDelta add{changed, 1U, changed, ++sequences[changed], {},
            common_lower ? eme::book::Side::ask : eme::book::Side::bid,
            price(common_lower ? 5100 : 5000), core::QuantityDelta::from_raw(100)};
        require(std::get<eme::book::BookUpdateResult>(state.apply(add)) == eme::book::BookUpdateResult::applied, "variant update rejected");
        positive_variant = full_rescreen(fixture, state);
        add.sequence = ++sequences[changed]; add.quantity_delta = core::QuantityDelta::from_raw(-100);
        require(std::get<eme::book::BookUpdateResult>(state.apply(add)) == eme::book::BookUpdateResult::applied, "variant reset rejected");
    }
    double elapsed = 0, maximum = 0;
    for (std::size_t index = 0; index < count; ++index) {
        time += step_ns;
        session::ReplayFrame frame{ordinal++, time, changed, !trade_only, {}, wall_start + time};
        if (trade_only) {
            frame.trade = market::PublicTrade{changed, "synthetic-trade", 5000, 100,
                static_cast<std::uint64_t>((wall_start + time) / 1'000'000), true, false};
        } else {
            const market::BookDelta delta{changed, 1U, changed, ++sequences[changed],
                market::ReceiveTime{std::chrono::nanoseconds{time}}, common_lower ? eme::book::Side::ask : eme::book::Side::bid,
                price(common_lower ? 5100 : 5000), core::QuantityDelta::from_raw(index % 2U == 0U ? 100 : -100)};
            require(std::get<eme::book::BookUpdateResult>(state.apply(delta)) == eme::book::BookUpdateResult::applied,
                    "synthetic benchmark update rejected");
        }
        const auto begin = std::chrono::steady_clock::now();
        observer->before(time, state);
        observer->after(frame, {}, state);
        const auto duration = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
        elapsed += duration; maximum = std::max(maximum, duration);
    }
    observer->finish(time + step_ns, state);
    session::ReplayPlan plan; plan.source_kind = "synthetic";
    observer->report(plan);
    auto report = final_report(output.str());
    const auto baskets = fixture.screen.at("baskets").size();
    const auto fanout = trade_only ? 0U : common_lower ? baskets : baskets == 1U ? 1U : 5U;
    require(report.at("basket_evaluations").get<std::uint64_t>() == baskets * 3U + fanout * count,
            "targeted dependency evaluation count differs from declared workload");
    require(report.at("evaluated_quantities").get<std::uint64_t>() == (baskets + fanout * count) * 100U,
            "quantity evaluations differ from fully funded 100-contract grid");
    require(report.at("public_trades").get<std::uint64_t>() == (trade_only ? count : 0U), "trade count mismatch");
    require(report.at("orders_sent") == 0U && report.at("simulated_fills") == false, "benchmark changed execution mode");
    std::istringstream lines{output.str()};
    std::size_t closed = 0;
    for (std::string line; std::getline(lines, line);) {
        const auto value = Json::parse(line);
        if (value.value("type", "") != "basket_episode_close") { continue; }
        const auto id = value.at("basket_id").get<std::size_t>() - 1U;
        const auto& peak = value.at("peak_quote");
        require(peak.at("net_margin_micro").get<std::int64_t>() ==
            std::max(initial[id].net_margin_micro, positive_variant[id].net_margin_micro),
            "dependency observer differs from untimed full rescreen reference");
        require(value.at("right_censored") == true && value.at("left_censored") == true,
                "fixture boundary episodes must be censored");
        ++closed;
    }
    require(closed == baskets, "positive quote updates incorrectly fragmented episodes");
    return {elapsed / static_cast<double>(count), maximum, std::move(report)};
}
}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
        const bool smoke = argc == 2 && std::string_view{argv[1]} == "--smoke";
        if (argc != 1 && !smoke) { std::cerr << "Usage: eme_basket_observation_benchmarks [--smoke]\n"; return 2; }
        const std::size_t samples = smoke ? 1U : 9U, batch = smoke ? 4U : 200U;
        Json results = Json::array();
        for (const auto& [name, baskets, trade, lower] : {
            std::tuple{"single_basket_book", 1U, false, true},
            std::tuple{"hundred_baskets_range_fanout_5", 100U, false, false},
            std::tuple{"hundred_baskets_lower_fanout_100", 100U, false, true},
            std::tuple{"hundred_baskets_public_trade", 100U, true, false}}) {
            Fixture fixture{baskets};
            const auto expected = run(fixture, batch, trade, lower).economic_result;
            std::vector<double> means, maxima;
            for (std::size_t sample = 0; sample < samples; ++sample) {
                const auto actual = run(fixture, batch, trade, lower);
                require(actual.economic_result == expected, "observer results are not deterministic across identical runs");
                means.push_back(actual.mean_ns); maxima.push_back(actual.maximum_ns);
            }
            auto sorted = means; std::sort(sorted.begin(), sorted.end());
            results.push_back({{"scenario", name}, {"baskets", baskets}, {"markets", fixture.market_rows.size()},
                {"input_levels_per_side", 8U}, {"cap_contracts", 100U}, {"batch_callbacks", batch},
                {"samples", samples}, {"warmups", 1U}, {"batch_mean_ns", means}, {"batch_maximum_callback_ns", maxima},
                {"median_batch_mean_ns", sorted[sorted.size() / 2U]},
                {"maximum_callback_ns", *std::max_element(maxima.begin(), maxima.end())},
                {"basket_evaluations_including_setup", expected.at("basket_evaluations")},
                {"evaluated_quantities_including_setup", expected.at("evaluated_quantities")},
                {"full_rescreen_reference_parity", true},
                {"policy_sha256", expected.at("policy_sha256")}, {"metadata_sha256", expected.at("metadata_sha256")}});
        }
        std::cout << Json{{"schema_version", 1U}, {"fixture", "conditional_basket_callbacks_v1"},
            {"measurement_scope", "before+after callbacks only; book mutation, setup, full reference rescreen and report excluded"},
            {"interpretation", "Synthetic callback workload measurements, not network/exchange latency or a before-after speedup"},
            {"clock_overhead_subtracted", false}, {"shared_liquidity", true}, {"results", results}}.dump(2) << '\n';
        return std::cout ? 0 : 1;
    } catch (const session::ReplayError& error) { std::cerr << error.reason << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
