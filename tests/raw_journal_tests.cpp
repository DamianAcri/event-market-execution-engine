#include "eme/journal/raw_journal.hpp"
#include "test_support.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {

namespace journal = eme::journal;

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

[[nodiscard]] journal::RawMarketRecord record(
    const eme::market::ConnectionGeneration generation,
    const eme::book::SequenceNumber sequence,
    std::string payload,
    const std::optional<std::int64_t> exchange_time = std::nullopt) {
    return {
        journal::current_schema_version,
        11U,
        generation,
        eme::market::ReceiveTime{std::chrono::nanoseconds{
            static_cast<std::int64_t>(100U + sequence)}},
        journal::WallTime{std::chrono::nanoseconds{
            static_cast<std::int64_t>(200U + sequence)}},
        sequence,
        exchange_time,
        "orderbook_delta",
        std::move(payload),
    };
}

void expect_record(
    eme::test::Context& test,
    const journal::JournalReadResult& result,
    const eme::market::ConnectionGeneration generation,
    const eme::book::SequenceNumber sequence,
    const std::string& payload,
    const std::optional<std::int64_t> exchange_time) {
    const auto* decoded = std::get_if<journal::RawMarketRecord>(&result);
    test.expect(decoded != nullptr, "journal entry decodes as a raw market record");
    if (decoded == nullptr) {
        return;
    }
    test.expect(decoded->schema_version == journal::current_schema_version &&
                    decoded->metadata_version == 11U &&
                    decoded->connection_generation == generation &&
                    decoded->sequence == sequence,
                "journal preserves schema, metadata, generation, and sequence");
    test.expect(decoded->received_at.time_since_epoch().count() ==
                        static_cast<std::int64_t>(100 + sequence) &&
                    decoded->observed_at.time_since_epoch().count() ==
                        static_cast<std::int64_t>(200 + sequence),
                "journal preserves monotonic and wall-clock timestamps");
    test.expect(decoded->channel == "orderbook_delta" && decoded->payload == payload &&
                    decoded->exchange_time_ns == exchange_time,
                "journal preserves channel, payload, and optional exchange timestamp");
}

void test_round_trip_and_append(eme::test::Context& test) {
    TemporaryFile file{"round-trip"};
    const std::string binary_payload{"line1\nline2\0tail", 16U};

    {
        auto open = journal::open_raw_journal_writer(file.path());
        auto* writer_handle = std::get_if<std::unique_ptr<journal::RawJournalWriter>>(&open);
        test.expect(writer_handle != nullptr, "factory creates a valid journal writer");
        if (writer_handle == nullptr) {
            return;
        }
        auto& writer = **writer_handle;
        test.expect(writer.append(record(0U, 1U, "{}")).has_value(),
                    "writer rejects a record without a connection generation");
        test.expect(!writer.append(record(3U, 10U, "{\"seq\":10}", 999)).has_value() &&
                        !writer.append(record(3U, 11U, binary_payload)).has_value() &&
                        writer.records_written() == 2U && !writer.flush().has_value(),
                    "writer durably appends and counts valid records");
    }

    {
        auto open = journal::open_raw_journal_reader(file.path());
        auto* reader_handle = std::get_if<std::unique_ptr<journal::RawJournalReader>>(&open);
        test.expect(reader_handle != nullptr, "factory returns a validated reader");
        if (reader_handle == nullptr) {
            return;
        }
        auto& reader = **reader_handle;
        expect_record(test, reader.read_next(), 3U, 10U, "{\"seq\":10}", 999);
        expect_record(test, reader.read_next(), 3U, 11U, binary_payload, std::nullopt);
        test.expect(std::holds_alternative<journal::EndOfJournal>(reader.read_next()) &&
                        reader.records_read() == 2U,
                    "reader reaches a clean end and reports its count");
    }

    auto reopened = journal::open_raw_journal_writer(file.path());
    auto* writer_handle =
        std::get_if<std::unique_ptr<journal::RawJournalWriter>>(&reopened);
    test.expect(writer_handle != nullptr && (*writer_handle)->records_written() == 2U,
                "reopened writer validates and counts the existing journal");
    if (writer_handle != nullptr) {
        test.expect(!(*writer_handle)
                         ->append(record(4U, 1U, "{\"reconnected\":true}"))
                         .has_value() &&
                        !(*writer_handle)->flush().has_value(),
                    "reopened writer appends without replacing earlier records");
    }
}

void test_corruption_detection(eme::test::Context& test) {
    TemporaryFile invalid_header{"invalid-header"};
    {
        std::ofstream output{invalid_header.path(), std::ios::binary};
        output << "not-a-journal";
    }
    const auto invalid_reader = journal::open_raw_journal_reader(invalid_header.path());
    const auto invalid_writer = journal::open_raw_journal_writer(invalid_header.path());
    test.expect(std::get<journal::JournalError>(invalid_reader).code ==
                        journal::JournalErrorCode::invalid_header &&
                    std::get<journal::JournalError>(invalid_writer).code ==
                        journal::JournalErrorCode::invalid_header,
                "open factories cannot produce zombie objects for incompatible files");

    TemporaryFile truncated{"truncated"};
    {
        auto open = journal::open_raw_journal_writer(truncated.path());
        auto& writer = **std::get_if<std::unique_ptr<journal::RawJournalWriter>>(&open);
        test.expect(!writer.append(record(1U, 1U, "payload")).has_value() &&
                        !writer.flush().has_value(),
                    "valid record is prepared for truncation test");
    }
    const auto original_size = std::filesystem::file_size(truncated.path());
    std::filesystem::resize_file(truncated.path(), original_size - 1U);
    auto truncated_open = journal::open_raw_journal_reader(truncated.path());
    auto& truncated_reader =
        **std::get_if<std::unique_ptr<journal::RawJournalReader>>(&truncated_open);
    const auto truncated_result = truncated_reader.read_next();
    test.expect(std::get<journal::JournalError>(truncated_result).code ==
                        journal::JournalErrorCode::truncated_record &&
                    std::get<journal::JournalError>(
                        journal::open_raw_journal_writer(truncated.path()))
                            .code == journal::JournalErrorCode::truncated_record,
                "reader and append factory reject a truncated record");

    TemporaryFile altered{"checksum"};
    {
        auto open = journal::open_raw_journal_writer(altered.path());
        auto& writer = **std::get_if<std::unique_ptr<journal::RawJournalWriter>>(&open);
        static_cast<void>(writer.append(record(1U, 7U, "payload")));
        static_cast<void>(writer.flush());
    }
    {
        std::fstream file{altered.path(), std::ios::binary | std::ios::in | std::ios::out};
        file.seekg(-5, std::ios::end);
        char byte{};
        file.read(&byte, 1);
        byte = static_cast<char>(static_cast<unsigned char>(byte) ^ 0x01U);
        file.seekp(-5, std::ios::end);
        file.write(&byte, 1);
    }
    auto altered_open = journal::open_raw_journal_reader(altered.path());
    auto& altered_reader =
        **std::get_if<std::unique_ptr<journal::RawJournalReader>>(&altered_open);
    test.expect(std::get<journal::JournalError>(altered_reader.read_next()).code ==
                    journal::JournalErrorCode::checksum_mismatch,
                "CRC detects a same-length payload bit flip before replay");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_round_trip_and_append(test);
    test_corruption_detection(test);
    return test.result();
}
