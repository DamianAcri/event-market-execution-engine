#include "eme/journal/raw_journal.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {

class TemporaryFile final {
public:
    explicit TemporaryFile(const std::string& label) {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("eme-" + label + "-" + std::to_string(unique) + ".journal");
    }

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] eme::journal::RawMarketRecord record(
    const eme::market::ConnectionGeneration generation,
    const eme::book::SequenceNumber sequence,
    std::string payload,
    const std::optional<std::int64_t> exchange_time = std::nullopt) {
    return {
        eme::journal::current_schema_version,
        generation,
        eme::market::ReceiveTime{std::chrono::nanoseconds{
            static_cast<std::int64_t>(100U + sequence)}},
        eme::journal::WallTime{std::chrono::nanoseconds{
            static_cast<std::int64_t>(200U + sequence)}},
        sequence,
        exchange_time,
        "orderbook_delta",
        std::move(payload),
    };
}

void expect_record(
    eme::test::Context& test,
    const eme::journal::JournalReadResult& result,
    const eme::market::ConnectionGeneration generation,
    const eme::book::SequenceNumber sequence,
    const std::string& payload,
    const std::optional<std::int64_t> exchange_time) {
    const auto* decoded = std::get_if<eme::journal::RawMarketRecord>(&result);
    test.expect(decoded != nullptr, "journal entry decodes as a raw market record");
    if (decoded == nullptr) {
        return;
    }
    test.expect(decoded->schema_version == eme::journal::current_schema_version &&
                    decoded->connection_generation == generation &&
                    decoded->sequence == sequence,
                "journal preserves schema, generation, and sequence");
    test.expect(decoded->received_at.time_since_epoch().count() ==
                        static_cast<std::int64_t>(100 + sequence) &&
                    decoded->observed_at.time_since_epoch().count() ==
                        static_cast<std::int64_t>(200 + sequence),
                "journal preserves monotonic and wall-clock timestamps");
    test.expect(decoded->channel == "orderbook_delta" && decoded->payload == payload,
                "journal preserves channel and raw payload byte-for-byte");
    test.expect(decoded->exchange_time_ns == exchange_time,
                "journal preserves optional exchange timestamp");
}

void test_round_trip_and_append(eme::test::Context& test) {
    TemporaryFile file{"round-trip"};
    const std::string binary_payload{"line1\nline2\0tail", 16U};

    {
        eme::journal::RawJournalWriter writer{file.path()};
        test.expect(writer.ready(), "new journal writer creates a valid file");

        auto invalid = record(0U, 1U, "{}");
        const auto invalid_result = writer.append(invalid);
        test.expect(invalid_result.has_value() &&
                        invalid_result->code == eme::journal::JournalErrorCode::invalid_record,
                    "writer rejects a record without a connection generation");

        test.expect(!writer.append(record(3U, 10U, "{\"seq\":10}", 999)).has_value(),
                    "writer appends first valid record");
        test.expect(!writer.append(record(3U, 11U, binary_payload)).has_value(),
                    "writer accepts multiline and embedded-null payload bytes");
        test.expect(writer.records_written() == 2U,
                    "writer counts only successfully appended records");
        test.expect(!writer.flush().has_value(), "journal flush succeeds");
    }

    {
        eme::journal::RawJournalReader reader{file.path()};
        test.expect(reader.ready(), "journal reader validates file header");
        expect_record(test, reader.read_next(), 3U, 10U, "{\"seq\":10}", 999);
        expect_record(test, reader.read_next(), 3U, 11U, binary_payload, std::nullopt);
        test.expect(std::holds_alternative<eme::journal::EndOfJournal>(reader.read_next()),
                    "reader reports an explicit clean end of journal");
        test.expect(reader.records_read() == 2U, "reader reports decoded record count");
    }

    {
        eme::journal::RawJournalWriter writer{file.path()};
        test.expect(writer.ready(), "writer reopens an existing compatible journal");
        test.expect(writer.records_written() == 2U,
                    "reopened writer recovers the existing record count");
        test.expect(!writer.append(record(4U, 1U, "{\"reconnected\":true}")).has_value(),
                    "reopened writer appends without replacing earlier records");
        test.expect(!writer.flush().has_value(), "appended journal flush succeeds");
    }

    eme::journal::RawJournalReader replay{file.path()};
    test.expect(replay.ready(), "appended journal remains readable");
    static_cast<void>(replay.read_next());
    static_cast<void>(replay.read_next());
    expect_record(test, replay.read_next(), 4U, 1U, "{\"reconnected\":true}", std::nullopt);
    test.expect(std::holds_alternative<eme::journal::EndOfJournal>(replay.read_next()),
                "sequential replay reaches clean end after appended record");
}

void test_corruption_detection(eme::test::Context& test) {
    TemporaryFile invalid_header{"invalid-header"};
    {
        std::ofstream output{invalid_header.path(), std::ios::binary};
        output << "not-a-journal";
    }
    eme::journal::RawJournalReader invalid_reader{invalid_header.path()};
    test.expect(!invalid_reader.ready() && invalid_reader.initialization_error().has_value() &&
                    invalid_reader.initialization_error()->code ==
                        eme::journal::JournalErrorCode::invalid_header,
                "reader rejects an invalid journal header");
    eme::journal::RawJournalWriter invalid_writer{invalid_header.path()};
    test.expect(!invalid_writer.ready() && invalid_writer.initialization_error().has_value() &&
                    invalid_writer.initialization_error()->code ==
                        eme::journal::JournalErrorCode::invalid_header,
                "writer refuses to append to an incompatible file");

    TemporaryFile truncated{"truncated"};
    {
        eme::journal::RawJournalWriter writer{truncated.path()};
        test.expect(writer.ready() && !writer.append(record(1U, 1U, "payload")).has_value() &&
                        !writer.flush().has_value(),
                    "valid record is prepared for truncation test");
    }
    const auto original_size = std::filesystem::file_size(truncated.path());
    std::filesystem::resize_file(truncated.path(), original_size - 1U);
    eme::journal::RawJournalReader truncated_reader{truncated.path()};
    test.expect(truncated_reader.ready(), "truncated journal still has a valid header");
    const auto truncated_result = truncated_reader.read_next();
    const auto* error = std::get_if<eme::journal::JournalError>(&truncated_result);
    test.expect(error != nullptr &&
                    error->code == eme::journal::JournalErrorCode::truncated_record &&
                    error->record_index == 0U,
                "reader detects truncated record and reports its index");
    eme::journal::RawJournalWriter truncated_writer{truncated.path()};
    test.expect(!truncated_writer.ready() &&
                    truncated_writer.initialization_error().has_value() &&
                    truncated_writer.initialization_error()->code ==
                        eme::journal::JournalErrorCode::truncated_record,
                "writer refuses to append after a truncated record");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_round_trip_and_append(test);
    test_corruption_detection(test);
    return test.result();
}
