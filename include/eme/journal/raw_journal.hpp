#pragma once

#include "eme/market/normalized_event.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace eme::journal {

inline constexpr std::uint32_t current_schema_version = 2U;
using WallTime =
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

struct RawMarketRecord final {
    std::uint32_t schema_version{current_schema_version};
    market::MetadataVersion metadata_version{};
    market::ConnectionGeneration connection_generation{};
    market::ReceiveTime received_at{};
    WallTime observed_at{};
    book::SequenceNumber sequence{};
    std::optional<std::int64_t> exchange_time_ns;
    std::string channel;
    std::string payload;

    friend bool operator==(const RawMarketRecord&, const RawMarketRecord&) = default;
};

enum class JournalErrorCode : std::uint8_t {
    open_failed,
    invalid_header,
    unsupported_version,
    io_error,
    invalid_record,
    truncated_record,
    record_too_large,
    checksum_mismatch,
};

[[nodiscard]] constexpr std::string_view to_string(const JournalErrorCode code) noexcept {
    switch (code) {
        case JournalErrorCode::open_failed:
            return "OPEN_FAILED";
        case JournalErrorCode::invalid_header:
            return "INVALID_HEADER";
        case JournalErrorCode::unsupported_version:
            return "UNSUPPORTED_VERSION";
        case JournalErrorCode::io_error:
            return "IO_ERROR";
        case JournalErrorCode::invalid_record:
            return "INVALID_RECORD";
        case JournalErrorCode::truncated_record:
            return "TRUNCATED_RECORD";
        case JournalErrorCode::record_too_large:
            return "RECORD_TOO_LARGE";
        case JournalErrorCode::checksum_mismatch:
            return "CHECKSUM_MISMATCH";
    }
    return "UNKNOWN";
}

struct JournalError final {
    JournalErrorCode code{};
    std::uint64_t record_index{};
};

struct EndOfJournal final {};
using JournalReadResult = std::variant<RawMarketRecord, EndOfJournal, JournalError>;

class RawJournalWriter;
class RawJournalReader;
using OpenWriterResult = std::variant<std::unique_ptr<RawJournalWriter>, JournalError>;
using OpenReaderResult = std::variant<std::unique_ptr<RawJournalReader>, JournalError>;

[[nodiscard]] OpenWriterResult open_raw_journal_writer(
    const std::filesystem::path& path);
[[nodiscard]] OpenReaderResult open_raw_journal_reader(
    const std::filesystem::path& path);

class RawJournalWriter final {
public:
    RawJournalWriter(const RawJournalWriter&) = delete;
    RawJournalWriter& operator=(const RawJournalWriter&) = delete;
    RawJournalWriter(RawJournalWriter&&) noexcept = default;
    RawJournalWriter& operator=(RawJournalWriter&&) noexcept = default;

    [[nodiscard]] std::optional<JournalError> append(const RawMarketRecord& record);
    [[nodiscard]] std::optional<JournalError> flush();
    [[nodiscard]] std::uint64_t records_written() const noexcept { return records_written_; }

private:
    friend OpenWriterResult open_raw_journal_writer(const std::filesystem::path& path);
    explicit RawJournalWriter(std::ofstream stream, std::uint64_t records_written)
        : stream_{std::move(stream)}, records_written_{records_written} {}

    std::ofstream stream_;
    std::uint64_t records_written_{};
};

class RawJournalReader final {
public:
    RawJournalReader(const RawJournalReader&) = delete;
    RawJournalReader& operator=(const RawJournalReader&) = delete;
    RawJournalReader(RawJournalReader&&) noexcept = default;
    RawJournalReader& operator=(RawJournalReader&&) noexcept = default;

    [[nodiscard]] JournalReadResult read_next();
    [[nodiscard]] std::uint64_t records_read() const noexcept { return records_read_; }

private:
    friend OpenReaderResult open_raw_journal_reader(const std::filesystem::path& path);
    explicit RawJournalReader(std::ifstream stream) : stream_{std::move(stream)} {}

    std::ifstream stream_;
    std::uint64_t records_read_{};
};

}  // namespace eme::journal
