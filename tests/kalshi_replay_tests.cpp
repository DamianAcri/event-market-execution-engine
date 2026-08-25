#include "eme/gateway/kalshi/orderbook_processor.hpp"
#include "eme/journal/raw_journal.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {

namespace kalshi = eme::gateway::kalshi;

class TemporaryJournal final {
public:
    TemporaryJournal() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("eme-kalshi-replay-" + std::to_string(unique) + ".journal");
    }

    ~TemporaryJournal() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryJournal(const TemporaryJournal&) = delete;
    TemporaryJournal& operator=(const TemporaryJournal&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

struct TerminalBook final {
    std::int64_t best_bid{};
    std::int64_t best_ask{};
    std::int64_t bid_quantity{};
    eme::book::SequenceNumber sequence{};

    friend bool operator==(const TerminalBook&, const TerminalBook&) = default;
};

[[nodiscard]] eme::journal::RawMarketRecord journal_record(
    const eme::book::SequenceNumber sequence,
    std::string payload) {
    return {
        eme::journal::current_schema_version,
        11U,
        1U,
        eme::market::ReceiveTime{std::chrono::nanoseconds{
            static_cast<std::int64_t>(1'000U + sequence)}},
        eme::journal::WallTime{std::chrono::nanoseconds{
            static_cast<std::int64_t>(2'000U + sequence)}},
        sequence,
        std::nullopt,
        "orderbook_delta",
        std::move(payload),
    };
}

[[nodiscard]] std::optional<TerminalBook> replay(
    eme::test::Context& test,
    const std::filesystem::path& path) {
    kalshi::MarketRegistry markets{11U};
    constexpr eme::market::MarketId market_id = 42U;
    if (markets.register_market(market_id, "TEST-MARKET") !=
        kalshi::MarketRegistrationResult::registered) {
        return std::nullopt;
    }

    kalshi::OrderBookProcessor processor{markets};
    if (!processor.open_connection(1U)) {
        return std::nullopt;
    }

    auto open = eme::journal::open_raw_journal_reader(path);
    auto* reader_handle =
        std::get_if<std::unique_ptr<eme::journal::RawJournalReader>>(&open);
    if (reader_handle == nullptr) {
        return std::nullopt;
    }
    auto& reader = **reader_handle;

    std::size_t processed = 0U;
    while (true) {
        const auto next = reader.read_next();
        if (std::holds_alternative<eme::journal::EndOfJournal>(next)) {
            break;
        }
        const auto* record = std::get_if<eme::journal::RawMarketRecord>(&next);
        if (record == nullptr) {
            return std::nullopt;
        }
        const auto result = processor.process(*record);
        const auto* update = std::get_if<eme::book::BookUpdateResult>(&result);
        if (update == nullptr || *update != eme::book::BookUpdateResult::applied) {
            return std::nullopt;
        }
        ++processed;
    }
    test.expect(processed == 3U, "replay processes every journal record exactly once");

    const auto* book = processor.state().find_book(market_id);
    if (book == nullptr || !book->best_bid().has_value() ||
        !book->best_ask().has_value() || !book->last_sequence().has_value()) {
        return std::nullopt;
    }
    return TerminalBook{
        book->best_bid()->raw(),
        book->best_ask()->raw(),
        book->quantity_at(eme::book::Side::bid, eme::test::price(4'500)).raw(),
        *book->last_sequence(),
    };
}

void test_deterministic_raw_replay(eme::test::Context& test) {
    TemporaryJournal journal;
    {
        auto open = eme::journal::open_raw_journal_writer(journal.path());
        auto* writer_handle =
            std::get_if<std::unique_ptr<eme::journal::RawJournalWriter>>(&open);
        test.expect(writer_handle != nullptr, "raw replay journal opens");
        if (writer_handle == nullptr) {
            return;
        }
        auto& writer = **writer_handle;
        constexpr auto snapshot = R"json({
          "type":"orderbook_snapshot","sid":2,"seq":10,
          "msg":{"market_ticker":"TEST-MARKET",
          "yes_dollars_fp":[["0.4000","1.00"]],
          "no_dollars_fp":[["0.6000","2.00"]]}})json";
        constexpr auto bid_delta = R"json({
          "type":"orderbook_delta","sid":2,"seq":11,
          "msg":{"market_ticker":"TEST-MARKET","price_dollars":"0.4500",
          "delta_fp":"3.50","side":"yes"}})json";
        constexpr auto ask_delta = R"json({
          "type":"orderbook_delta","sid":2,"seq":12,
          "msg":{"market_ticker":"TEST-MARKET","price_dollars":"0.5500",
          "delta_fp":"1.25","side":"no"}})json";

        test.expect(!writer.append(journal_record(10U, snapshot)).has_value() &&
                        !writer.append(journal_record(11U, bid_delta)).has_value() &&
                        !writer.append(journal_record(12U, ask_delta)).has_value() &&
                        !writer.flush().has_value(),
                    "snapshot and deltas are durably appended in arrival order");
    }

    const auto first = replay(test, journal.path());
    const auto second = replay(test, journal.path());
    test.expect(first.has_value() && second.has_value(),
                "both independent replay passes reconstruct a terminal book");
    if (!first.has_value() || !second.has_value()) {
        return;
    }
    test.expect(*first == *second,
                "identical raw journal produces identical terminal market state");
    test.expect(first->best_bid == 4'500 && first->best_ask == 5'500 &&
                    first->bid_quantity == 350 && first->sequence == 12U,
                "replayed terminal book matches the expected executable state");
}

void test_record_envelope_is_authoritative(eme::test::Context& test) {
    kalshi::MarketRegistry markets{11U};
    static_cast<void>(markets.register_market(42U, "TEST-MARKET"));
    kalshi::OrderBookProcessor processor{markets};
    test.expect(processor.open_connection(1U), "processor opens for envelope checks");

    constexpr auto snapshot = R"json({
      "type":"orderbook_snapshot","sid":2,"seq":10,
      "msg":{"market_ticker":"TEST-MARKET",
      "yes_dollars_fp":[["0.4000","1.00"]],"no_dollars_fp":[]}})json";
    auto wrong_schema = journal_record(10U, snapshot);
    wrong_schema.schema_version = 1U;
    test.expect(std::get<kalshi::ProcessingError>(processor.process(wrong_schema)) ==
                    kalshi::ProcessingError::schema_version_mismatch,
                "processor rejects records from an unsupported schema");

    auto wrong_metadata = journal_record(10U, snapshot);
    wrong_metadata.metadata_version = 10U;
    test.expect(std::get<kalshi::ProcessingError>(processor.process(wrong_metadata)) ==
                    kalshi::ProcessingError::metadata_version_mismatch,
                "replay rejects a record produced under different market metadata");

    auto wrong_sequence = journal_record(11U, snapshot);
    test.expect(std::get<kalshi::ProcessingError>(processor.process(wrong_sequence)) ==
                        kalshi::ProcessingError::sequence_mismatch &&
                    processor.state().book_count() == 0U,
                "outer and payload sequence disagreement fails before state mutation");
    const auto valid = processor.process(journal_record(10U, snapshot));
    test.expect(std::get<eme::book::BookUpdateResult>(valid) ==
                        eme::book::BookUpdateResult::applied &&
                    processor.state().valid_book_count() == 1U,
                "matching envelope and payload have one authoritative sequence");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_deterministic_raw_replay(test);
    test_record_envelope_is_authoritative(test);
    return test.result();
}
