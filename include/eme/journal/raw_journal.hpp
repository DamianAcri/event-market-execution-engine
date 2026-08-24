#pragma once

#include "eme/market/normalized_event.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace eme::journal {

inline constexpr std::uint32_t current_schema_version = 1U;
using WallTime =
    std::chrono::time_point<std::chrono::system_clock, std::chrono::nanoseconds>;

struct RawMarketRecord final {
    std::uint32_t schema_version{current_schema_version};
    market::ConnectionGeneration connection_generation{};
    market::ReceiveTime received_at{};
    WallTime observed_at{};
    book::SequenceNumber sequence{};
    std::optional<std::int64_t> exchange_time_ns;
    std::string channel;
    std::string payload;
};

enum class JournalErrorCode : std::uint8_t {
    open_failed,
    invalid_header,
    unsupported_version,
    io_error,
    invalid_record,
    truncated_record,
    record_too_large,
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
    }
    return "UNKNOWN";
}

struct JournalError final {
    JournalErrorCode code{};
    std::uint64_t record_index{};
};

struct EndOfJournal final {};
using JournalReadResult = std::variant<RawMarketRecord, EndOfJournal, JournalError>;

class RawJournalWriter final {
public:
    explicit RawJournalWriter(const std::filesystem::path& path);

    RawJournalWriter(const RawJournalWriter&) = delete;
    RawJournalWriter& operator=(const RawJournalWriter&) = delete;
    RawJournalWriter(RawJournalWriter&&) noexcept = default;
    RawJournalWriter& operator=(RawJournalWriter&&) noexcept = default;

    [[nodiscard]] bool ready() const noexcept { return !initialization_error_.has_value(); }
    [[nodiscard]] std::optional<JournalError> initialization_error() const noexcept {
        return initialization_error_;
    }
    [[nodiscard]] std::optional<JournalError> append(const RawMarketRecord& record);
    [[nodiscard]] std::optional<JournalError> flush();
    [[nodiscard]] std::uint64_t records_written() const noexcept { return records_written_; }

private:
    std::ofstream stream_;
    std::optional<JournalError> initialization_error_;
    std::uint64_t records_written_{};
};

class RawJournalReader final {
public:
    explicit RawJournalReader(const std::filesystem::path& path);

    RawJournalReader(const RawJournalReader&) = delete;
    RawJournalReader& operator=(const RawJournalReader&) = delete;
    RawJournalReader(RawJournalReader&&) noexcept = default;
    RawJournalReader& operator=(RawJournalReader&&) noexcept = default;

    [[nodiscard]] bool ready() const noexcept { return !initialization_error_.has_value(); }
    [[nodiscard]] std::optional<JournalError> initialization_error() const noexcept {
        return initialization_error_;
    }
    [[nodiscard]] JournalReadResult read_next();
    [[nodiscard]] std::uint64_t records_read() const noexcept { return records_read_; }

private:
    std::ifstream stream_;
    std::optional<JournalError> initialization_error_;
    std::uint64_t records_read_{};
};

}  // namespace eme::journal
