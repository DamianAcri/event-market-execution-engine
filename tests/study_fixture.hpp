#pragma once
#include "eme/session/execution_study.hpp"
#include "eme/session/async_capture.hpp"
#include "session/study_json.hpp"
#include "test_support.hpp"

#include <chrono>
#include <fstream>
#include <sstream>

namespace eme::test::study {
using Json = nlohmann::json;
namespace session = eme::session;

const Json metadata = Json::parse(R"({"schema_version":1,"metadata_version":11,"venue":"kalshi",
  "markets":[{"id":1,"ticker":"A"},{"id":2,"ticker":"B"}],"constraints":[
  {"id":7,"semantic_version":1,"key":"a-implies-b","provenance":"Synthetic arithmetic fixture only",
   "relationship":{"type":"implication","antecedent":1,"consequent":2}}]})");

inline Json policy() {
    return Json::parse(R"({"schema_version":1,"strategy":"one_attempt_per_constraint_v1",
      "fee_provenance":"Synthetic scenario: general 0.07 coefficient, direct-account precision; not venue fee discovery",
      "capital_micro_usd":100000000,"operating_cost_micro_usd":0,
      "quantity_cap_centicontracts":500,"quantity_step_centicontracts":100,"min_margin_micro_usd":0,
      "max_book_age_ns":100000,"leg_latency_ns":[0,0],"reject_legs":[false,false],
      "available_liquidity_bps":10000,"fees":[
      {"market_id":1,"coefficient_ppm":70000,"balance_quantum_micro":100},
      {"market_id":2,"coefficient_ppm":70000,"balance_quantum_micro":100}]})");
}
inline Json record(const std::int64_t time, const std::string& ticker, const std::uint64_t sequence, Json payload) {
    payload["msg"]["market_ticker"] = ticker;
    payload["sid"] = ticker == "A" ? 1U : 2U;
    payload["seq"] = sequence;
    return {{"generation", 1U}, {"time_ns", time}, {"observed_at_ns", 1'800'000'000'000'000'000LL + time},
            {"sequence", sequence}, {"payload", payload}};
}
inline Json capture() {
    const auto a = record(1000, "A", 1U, Json::parse(R"({"type":"orderbook_snapshot","msg":{
        "yes_dollars_fp":[["0.7000","5.00"]],"no_dollars_fp":[["0.7500","5.00"]]}})"));
    const auto b = record(2000, "B", 1U, Json::parse(R"({"type":"orderbook_snapshot","msg":{
        "yes_dollars_fp":[["0.5500","5.00"]],"no_dollars_fp":[["0.6000","2.00"],["0.6500","3.00"]]}})"));
    const auto remove = record(3000, "B", 2U, Json::parse(R"({"type":"orderbook_delta","msg":{
        "price_dollars":"0.6000","delta_fp":"-2.00","side":"no"}})"));
    const auto remove2 = record(4000, "B", 3U, Json::parse(R"({"type":"orderbook_delta","msg":{
        "price_dollars":"0.6500","delta_fp":"-3.00","side":"no"}})"));
    return {{"schema_version", 1U}, {"source_kind", "synthetic"}, {"provenance", "Synthetic depth disappears after the signal; no observed trading evidence"},
        {"use_yes_price", true}, {"controls", Json::array({{{"before_record", 0U}, {"time_ns", 0U}, {"action", "open"}, {"target", 1U}},
            {{"before_record", 4U}, {"time_ns", 10'000U}, {"action", "close"}, {"target", 1U}}})},
        {"records", Json::array({a, b, remove, remove2})}};
}
inline void write(const std::filesystem::path& path, const Json& json) {
    std::ofstream file{path}; file << json.dump(2) << '\n';
    if (!file) { throw std::runtime_error{"write fixture"}; }
}
class Fixture final {
public:
    explicit Fixture(std::filesystem::path path = {}) : keep_{!path.empty()} {
        root = path.empty() ? std::filesystem::temp_directory_path() / ("eme-study-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) : std::move(path);
        if (!std::filesystem::create_directory(root)) { throw std::runtime_error{"fixture directory exists"}; }
    }
    ~Fixture() { if (!keep_) { std::error_code ignored; std::filesystem::remove_all(root, ignored); } }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    session::ReplayInput build(const Json& data = capture(), const Json& meta = metadata) {
        write(root / "metadata.json", meta);
        write(root / "capture.json", data);
        const auto directory = root / ("session-" + std::to_string(index_++));
        if (const auto failure = session::import_capture(root / "metadata.json", root / "capture.json", directory)) {
            throw std::runtime_error{failure->reason};
        }
        auto loaded = session::load_replay(directory, directory / "replay.json");
        if (const auto* failure = std::get_if<session::ReplayError>(&loaded)) { throw std::runtime_error{failure->reason}; }
        return std::get<session::ReplayInput>(std::move(loaded));
    }
    std::pair<Json, std::string> run(const session::ReplayInput& input, const Json& parameters = policy()) {
        write(root / "policy.json", parameters);
        std::ostringstream out;
        if (const auto failure = session::run_execution_study(input, root / "policy.json", out)) { throw std::runtime_error{failure->reason}; }
        std::istringstream lines{out.str()};
        Json last;
        for (std::string line; std::getline(lines, line);) { last = Json::parse(line); }
        if (last["type"] != "study_complete") { throw std::runtime_error{"no complete study"}; }
        return {last, out.str()};
    }
    std::filesystem::path root;
private:
    bool keep_{};
    std::size_t index_{};
};
}  // namespace
