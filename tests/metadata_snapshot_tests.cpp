#include "eme/gateway/kalshi/metadata_snapshot.hpp"
#include "eme/gateway/kalshi/orderbook_processor.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace {

namespace kalshi = eme::gateway::kalshi;
namespace constraint = eme::constraint;
using Json = nlohmann::json;

[[nodiscard]] Json fixture() {
    return Json::parse(R"({
      "schema_version":1,"metadata_version":42,"venue":"kalshi",
      "markets":[{"id":3,"ticker":"C"},{"id":1,"ticker":"A"},{"id":2,"ticker":"B"}],
      "constraints":[
        {"id":9,"semantic_version":2,"key":"a-c","provenance":"reviewed rules v2",
         "relationship":{"type":"complement","left":1,"right":3}},
        {"id":7,"semantic_version":1,"key":"b-a","provenance":"reviewed rules v1",
         "relationship":{"type":"implication","antecedent":2,"consequent":1}}
      ]})");
}

void expect_error(eme::test::Context& test, const std::string& bytes,
                  const kalshi::MetadataErrorCode expected) {
    const auto result = kalshi::parse_metadata_snapshot(bytes);
    const auto* error = std::get_if<kalshi::MetadataError>(&result);
    test.expect(error != nullptr && error->code == expected,
                "invalid metadata fails with the expected structured error");
}

void test_canonical_and_lifetime(eme::test::Context& test) {
    auto document = fixture();
    auto result = kalshi::parse_metadata_snapshot(document.dump());
    const auto* snapshot = std::get_if<kalshi::MetadataSnapshot>(&result);
    test.expect(snapshot != nullptr, "valid snapshot loads from a temporary input buffer");
    if (snapshot == nullptr) {
        return;
    }
    const auto canonical = snapshot->canonical_json();
    test.expect(snapshot->markets().size() == 3U &&
                    snapshot->markets().metadata_version() == 42U &&
                    snapshot->markets().find("B") == 2U &&
                    snapshot->constraints().size() == 2U,
                "snapshot owns stable ticker IDs, version and compiled constraints");
    const auto dependencies = snapshot->constraints().dependencies(1U);
    test.expect(dependencies.size() == 2U && dependencies[0] == 7U && dependencies[1] == 9U,
                "dependency traversal follows stable constraint IDs");
    auto& markets = document["markets"];
    std::sort(markets.begin(), markets.end(), [](const Json& a, const Json& b) { return a["id"] < b["id"]; });
    do {
        for (int swap = 0; swap != 2; ++swap) {
            std::reverse(document["constraints"].begin(), document["constraints"].end());
            const auto reordered = kalshi::parse_metadata_snapshot(document.dump(2));
            const auto* loaded = std::get_if<kalshi::MetadataSnapshot>(&reordered);
            test.expect(loaded != nullptr && loaded->canonical_json() == canonical,
                        "all market and definition permutations serialize identically");
            if (loaded != nullptr) {
                const auto actual = loaded->constraints().dependencies(1U);
                test.expect(std::equal(actual.begin(), actual.end(), dependencies.begin(), dependencies.end()),
                            "all permutations retain dependency order");
            }
        }
    } while (std::next_permutation(markets.begin(), markets.end(),
        [](const Json& a, const Json& b) { return a["id"] < b["id"]; }));
    const auto roundtrip = kalshi::parse_metadata_snapshot(canonical);
    test.expect(std::get<kalshi::MetadataSnapshot>(roundtrip).canonical_json() == canonical,
                "canonical representation is idempotent");

    document["constraints"][0]["relationship"]["left"] = 999;
    test.expect(std::holds_alternative<kalshi::MetadataError>(
                    kalshi::parse_metadata_snapshot(document.dump())) &&
                    snapshot->canonical_json() == canonical,
                "a failed replacement leaves the previous snapshot untouched");

    kalshi::OrderBookProcessor processor{snapshot->markets()};
    test.expect(processor.open_connection(1U), "snapshot registry can drive the existing processor");
    eme::journal::RawMarketRecord record;
    record.metadata_version = 42U;
    record.connection_generation = 1U;
    record.sequence = 1U;
    record.payload = R"({"type":"orderbook_snapshot","sid":1,"seq":1,"msg":{"market_ticker":"A","yes_dollars_fp":[["0.4000","2.00"]],"no_dollars_fp":[["0.5000","2.00"]]}})";
    const auto processed = processor.process(record);
    const auto* update = std::get_if<eme::book::BookUpdateResult>(&processed);
    test.expect(update != nullptr && *update == eme::book::BookUpdateResult::applied,
                "loaded IDs and version are accepted by the processor");
    record.metadata_version = 43U;
    const auto mismatch = processor.process(record);
    test.expect(std::get<kalshi::ProcessingError>(mismatch) == kalshi::ProcessingError::metadata_version_mismatch,
                "a journal with a different metadata version is rejected");
}

void test_rejections(eme::test::Context& test) {
    using Code = kalshi::MetadataErrorCode;
    const auto valid = fixture();
    auto changed = valid;
    changed["schema_version"] = 2;
    expect_error(test, changed.dump(), Code::unsupported_version);
    for (const auto field : {"schema_version", "metadata_version", "markets", "constraints", "venue"}) {
        changed = valid;
        changed.erase(field);
        expect_error(test, changed.dump(), Code::invalid_shape);
    }
    for (const auto& value : std::array<Json, 8>{0, -1, 1.0, "1", true, nullptr, 18446744073709551616.0, Json::array()}) {
        changed = valid;
        changed["metadata_version"] = value;
        expect_error(test, changed.dump(), Code::invalid_value);
    }
    changed = valid;
    changed["metadata_version"] = std::numeric_limits<std::uint64_t>::max();
    changed["markets"][0]["id"] = std::numeric_limits<std::uint32_t>::max();
    changed["constraints"][0]["relationship"]["right"] = std::numeric_limits<std::uint32_t>::max();
    test.expect(std::holds_alternative<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(changed.dump())),
                "integer maxima survive serialization without floating point conversion");
    changed["markets"][0]["id"] = std::uint64_t{1} << 32U;
    expect_error(test, changed.dump(), Code::invalid_value);
    changed = valid;
    changed["unexpected"] = 1;
    expect_error(test, changed.dump(), Code::invalid_shape);
    changed = valid;
    changed["venue"] = "other";
    expect_error(test, changed.dump(), Code::invalid_value);
    changed = valid;
    changed["markets"] = Json::array();
    expect_error(test, changed.dump(), Code::invalid_value);
    changed = valid;
    changed["constraints"] = Json::array();
    test.expect(std::holds_alternative<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(changed.dump())),
                "market-data-only sessions may have no constraints");
    for (const auto field : {"id", "ticker"}) {
        changed = valid;
        changed["markets"][0][field] = changed["markets"][1][field];
        expect_error(test, changed.dump(), Code::duplicate_market);
    }
    changed = valid;
    changed["markets"].push_back(changed["markets"][0]);
    expect_error(test, changed.dump(), Code::duplicate_market);
    for (const auto field : {"id", "key"}) {
        changed = valid;
        changed["constraints"][0][field] = changed["constraints"][1][field];
        expect_error(test, changed.dump(), Code::duplicate_constraint);
    }
    changed = valid;
    changed["constraints"].push_back(changed["constraints"][0]);
    expect_error(test, changed.dump(), Code::duplicate_constraint);
    for (const auto& value : {std::string{}, std::string{"   "}, std::string{"bad\nsource"}, std::string(4097U, 'x')}) {
        changed = valid;
        changed["constraints"][0]["provenance"] = value;
        expect_error(test, changed.dump(), Code::invalid_value);
    }
    changed = valid;
    changed["markets"][0]["ticker"] = std::string{"A\0B", 3U};
    expect_error(test, changed.dump(), Code::invalid_value);
    changed = valid;
    changed["constraints"][0]["relationship"]["right"] = 99;
    expect_error(test, changed.dump(), Code::unknown_market);
    changed["constraints"][0]["relationship"]["right"] = 1;
    expect_error(test, changed.dump(), Code::invalid_value);
    changed["constraints"][0]["relationship"]["type"] = "invented";
    expect_error(test, changed.dump(), Code::invalid_value);
    expect_error(test, R"({"schema_version":1,"schema_version":2})", Code::duplicate_field);
    expect_error(test, R"({"markets":[{"id":1,"\u0069d":2}]})", Code::duplicate_field);
    expect_error(test, std::string(16U, '[') + "0" + std::string(16U, ']'), Code::nesting_too_deep);
    expect_error(test, std::string(kalshi::maximum_metadata_bytes + 1U, ' '), Code::input_too_large);
    const auto bytes = valid.dump();
    for (std::size_t length = 0U; length < bytes.size(); ++length) {
        expect_error(test, bytes.substr(0U, length), Code::invalid_json);
    }
    expect_error(test, bytes + " {}", Code::invalid_json);
    expect_error(test, std::string{"\xff", 1U}, Code::invalid_json);
}

void test_generated_payoffs(eme::test::Context& test) {
    for (std::uint32_t first = 1U; first <= 8U; ++first) {
        for (std::uint32_t second = 1U; second <= 8U; ++second) {
            if (first == second) { continue; }
            for (const bool implication : {false, true}) {
                auto document = fixture();
                document["markets"] = Json::array({{{"id", first}, {"ticker", "X"}}, {{"id", second}, {"ticker", "Y"}}});
                document["constraints"].erase(1U);
                document["constraints"][0]["relationship"] = implication
                    ? Json{{"type", "implication"}, {"antecedent", first}, {"consequent", second}}
                    : Json{{"type", "complement"}, {"left", first}, {"right", second}};
                const auto loaded = kalshi::parse_metadata_snapshot(document.dump());
                const auto* snapshot = std::get_if<kalshi::MetadataSnapshot>(&loaded);
                test.expect(snapshot != nullptr, "generated relationships load");
                if (snapshot == nullptr) { continue; }
                const auto* compiled = snapshot->constraints().find(9U);
                for (const std::int64_t quantity : {1, 99, 100, 250}) {
                    const auto constructed = constraint::construct_guaranteed_portfolio(*compiled, eme::test::quantity(quantity));
                    const auto* portfolio = std::get_if<constraint::GuaranteedPortfolio>(&constructed);
                    test.expect(portfolio != nullptr, "generated payoff portfolio constructs");
                    if (portfolio == nullptr) { continue; }
                    std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
                    std::size_t worlds = 0U;
                    for (const bool a : {false, true}) {
                        for (const bool b : {false, true}) {
                            if (!(implication ? (!a || b) : (a != b))) { continue; }
                            ++worlds;
                            std::int64_t payout = 0;
                            for (const auto& leg : portfolio->legs) {
                                const bool yes = leg.market_id == first ? a : b;
                                if ((leg.outcome == constraint::ContractOutcome::yes) == yes) {
                                    payout += quantity * 10'000;
                                }
                            }
                            minimum = std::min(minimum, payout);
                        }
                    }
                    test.expect(portfolio->minimum_payout.raw() == minimum && minimum == quantity * 10'000 &&
                                    portfolio->worlds_verified == worlds,
                                "loaded payoff agrees with independent exhaustive truth-table calculation");
                }
            }
        }
    }
}

}  // namespace

int main() {
    eme::test::Context test;
    test_canonical_and_lifetime(test);
    test_rejections(test);
    test_generated_payoffs(test);
    return test.result();
}
