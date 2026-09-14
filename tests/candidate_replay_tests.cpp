#include "eme/gateway/kalshi/orderbook_processor.hpp"
#include "eme/opportunity/candidate_tracker.hpp"
#include "eme/session/capture_session.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace {
namespace op = eme::opportunity;
namespace kalshi = eme::gateway::kalshi;
namespace session = eme::session;

class Fixture final {
public:
    Fixture() {
        path = std::filesystem::temp_directory_path() / ("eme-candidate-replay-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        auto parsed = kalshi::parse_metadata_snapshot(R"({"schema_version":1,"metadata_version":11,"venue":"kalshi",
          "markets":[{"id":1,"ticker":"B"},{"id":2,"ticker":"A"}],"constraints":[
          {"id":7,"semantic_version":1,"key":"a-implies-b","provenance":"synthetic test",
           "relationship":{"type":"implication","antecedent":2,"consequent":1}}]})");
        auto created = session::create_session(path, std::get<kalshi::MetadataSnapshot>(parsed));
        auto& writer = *std::get<std::unique_ptr<session::SessionWriter>>(created);
        const auto append = [&writer](const std::uint64_t seq, const char* channel, const char* payload) {
            if (writer.append({eme::journal::current_schema_version, 11U, 1U, {}, {}, seq, std::nullopt, channel, payload})) {
                throw std::runtime_error{"fixture journal append rejected"};
            }
        };
        // Wire fixtures use the gateway contract: subscription use_yes_price=true.
        append(1U, "orderbook_snapshot", R"({"type":"orderbook_snapshot","sid":1,"seq":1,
          "msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","2.50"]],"no_dollars_fp":[["0.7500","2.50"]]}})");
        append(1U, "orderbook_snapshot", R"({"type":"orderbook_snapshot","sid":2,"seq":1,
          "msg":{"market_ticker":"B","yes_dollars_fp":[["0.5500","4.00"]],"no_dollars_fp":[["0.6000","4.00"]]}})");
        append(2U, "orderbook_delta", R"({"type":"orderbook_delta","sid":2,"seq":2,
          "msg":{"market_ticker":"B","price_dollars":"0.6000","delta_fp":"1.00","side":"no"}})");
        append(2U, "orderbook_delta", R"({"type":"orderbook_delta","sid":1,"seq":2,
          "msg":{"market_ticker":"A","price_dollars":"0.7000","delta_fp":"-0.50","side":"yes"}})");
        append(4U, "orderbook_delta", R"({"type":"orderbook_delta","sid":1,"seq":4,
          "msg":{"market_ticker":"A","price_dollars":"0.7000","delta_fp":"1.00","side":"yes"}})");
        append(10U, "orderbook_snapshot", R"({"type":"orderbook_snapshot","sid":1,"seq":10,
          "msg":{"market_ticker":"A","yes_dollars_fp":[["0.7000","2.50"]],"no_dollars_fp":[["0.7500","2.50"]]}})");
        if (!std::holds_alternative<session::SessionManifest>(writer.finalize())) {
            throw std::runtime_error{"fixture session finalization rejected"};
        }
    }
    ~Fixture() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    std::filesystem::path path;
};

std::vector<op::CandidateEvent> replay(const std::filesystem::path& path, const bool full_scan) {
    auto verified = session::verify_session(path);
    const auto& metadata = std::get<session::VerifiedSession>(verified).metadata;
    kalshi::OrderBookProcessor processor{metadata.markets()};
    op::CandidateTracker tracker{metadata.markets().metadata_version(), metadata.constraints()};
    // This fixture supplies explicit controller actions. Raw journals do not
    // yet persist them; never infer automatic recovery from a later snapshot.
    (void)processor.open_connection(1U);
    std::vector<op::CandidateEvent> output;
    auto opened = eme::journal::open_raw_journal_reader(path / session::journal_filename);
    auto& reader = *std::get<std::unique_ptr<eme::journal::RawJournalReader>>(opened);
    constexpr std::array<eme::market::MarketId, 6U> affected{2U, 1U, 1U, 2U, 2U, 2U};
    for (std::size_t index = 0U; index < affected.size(); ++index) {
        if (index == 5U && !processor.begin_recovery(2U)) {
            throw std::runtime_error{"fixture recovery failed"};
        }
        const auto next = reader.read_next();
        const auto result = processor.process(std::get<eme::journal::RawMarketRecord>(next));
        const auto expected = index == 4U ? eme::book::BookUpdateResult::sequence_gap : eme::book::BookUpdateResult::applied;
        const auto* update = std::get_if<eme::book::BookUpdateResult>(&result);
        if (!update || *update != expected) { throw std::runtime_error{"unexpected fixture processing result"}; }
        const auto events = full_scan ? tracker.refresh_all(processor.state()) : tracker.refresh(affected[index], processor.state());
        output.insert(output.end(), events.begin(), events.end());
    }
    if (!std::holds_alternative<eme::journal::EndOfJournal>(reader.read_next())) {
        throw std::runtime_error{"unexpected extra fixture record"};
    }
    (void)processor.close_connection(1U);
    const auto final = tracker.refresh_all(processor.state());
    output.insert(output.end(), final.begin(), final.end());
    return output;
}
}  // namespace

int main() {
    try {
        Fixture fixture;
        eme::test::Context test;
        const auto first = replay(fixture.path, false);
        test.expect(first == replay(fixture.path, false) && first == replay(fixture.path, true),
            "verified-session replay repeats exact events and agrees with full scan");
        test.expect(first.size() == 5U, "open, update, gap invalidation, recovery open, disconnect invalidation");
        if (first.size() == 5U) {
            test.expect(first[0U].quote->acquisition_cost.raw() == 2'250'000 &&
                first[0U].quote->gross_margin.raw() == 250'000 &&
                first[1U].quote->acquisition_cost.raw() == 1'800'000 &&
                first[1U].quote->gross_margin.raw() == 200'000,
                "wire NO prices and fractional quantities match independent cash examples");
            test.expect(first[0U].kind == op::CandidateEventKind::opened &&
                first[1U].kind == op::CandidateEventKind::updated &&
                first[2U].reason == op::InvalidationReason::book_unavailable &&
                first[3U].kind == op::CandidateEventKind::opened &&
                first[4U].reason == op::InvalidationReason::disconnected,
                "recovery and rejection follow the shared market-state path");
            for (const auto& event : first) {
                test.expect(event.id == first[0U].id, "quote changes and recovery preserve identity");
            }
        }
        return test.result();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
