#include "eme/journal/raw_journal.hpp"

#include "journal_codec.hpp"

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace eme::journal {
namespace {

template <typename Value>
void write_unsigned(std::ostream& stream, const Value value) {
    static_assert(std::is_unsigned_v<Value>);
    std::array<char, sizeof(Value)> bytes{};
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        bytes[index] = static_cast<char>(
            (value >> static_cast<unsigned int>(index * 8U)) &
            static_cast<Value>(0xffU));
    }
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

template <typename Value>
[[nodiscard]] std::optional<Value> read_unsigned(std::istream& stream) {
    static_assert(std::is_unsigned_v<Value>);
    std::array<char, sizeof(Value)> bytes{};
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::nullopt;
    }
    Value value{};
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        const auto byte = static_cast<Value>(static_cast<unsigned char>(bytes[index]));
        value |= byte << static_cast<unsigned int>(index * 8U);
    }
    return value;
}

[[nodiscard]] std::optional<JournalError> validate_header(std::istream& stream) {
    std::array<char, codec::file_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (stream.gcount() != static_cast<std::streamsize>(magic.size()) ||
        magic != codec::file_magic) {
        return JournalError{JournalErrorCode::invalid_header, 0U};
    }
    const auto version = read_unsigned<std::uint32_t>(stream);
    if (!version.has_value()) {
        return JournalError{JournalErrorCode::invalid_header, 0U};
    }
    return *version == codec::file_format_version
               ? std::nullopt
               : std::optional{JournalError{JournalErrorCode::unsupported_version, 0U}};
}

}  // namespace

OpenReaderResult open_raw_journal_reader(const std::filesystem::path& path) {
    std::ifstream stream{path, std::ios::binary};
    if (!stream) {
        return JournalError{JournalErrorCode::open_failed, 0U};
    }
    if (const auto error = validate_header(stream); error.has_value()) {
        return *error;
    }
    return std::unique_ptr<RawJournalReader>{new RawJournalReader{std::move(stream)}};
}

OpenWriterResult open_raw_journal_writer(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return JournalError{JournalErrorCode::open_failed, 0U};
    }

    bool write_header = true;
    std::uint64_t records_written = 0U;
    if (exists) {
        const auto size = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error) {
            return JournalError{JournalErrorCode::open_failed, 0U};
        }
        write_header = size == 0U;
        if (!write_header) {
            auto existing_result = open_raw_journal_reader(path);
            if (const auto* error = std::get_if<JournalError>(&existing_result);
                error != nullptr) {
                return *error;
            }
            auto existing = std::move(
                std::get<std::unique_ptr<RawJournalReader>>(existing_result));
            while (true) {
                const auto next = existing->read_next();
                if (std::holds_alternative<EndOfJournal>(next)) {
                    break;
                }
                if (const auto* error = std::get_if<JournalError>(&next);
                    error != nullptr) {
                    return *error;
                }
            }
            records_written = existing->records_read();
        }
    }

    std::ofstream stream{path, std::ios::binary | std::ios::app};
    if (!stream) {
        return JournalError{JournalErrorCode::open_failed, 0U};
    }
    if (write_header) {
        stream.write(
            codec::file_magic.data(),
            static_cast<std::streamsize>(codec::file_magic.size()));
        write_unsigned(stream, codec::file_format_version);
        if (!stream) {
            return JournalError{JournalErrorCode::io_error, 0U};
        }
    }
    return std::unique_ptr<RawJournalWriter>{
        new RawJournalWriter{std::move(stream), records_written}};
}

std::optional<JournalError> RawJournalWriter::append(const RawMarketRecord& record) {
    auto encoded_result = codec::encode(record);
    if (const auto* error = std::get_if<JournalErrorCode>(&encoded_result);
        error != nullptr) {
        return JournalError{*error, records_written_};
    }
    const auto& encoded = std::get<std::vector<char>>(encoded_result);
    if (encoded.size() > codec::maximum_record_bytes ||
        encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
        return JournalError{JournalErrorCode::record_too_large, records_written_};
    }

    write_unsigned(stream_, static_cast<std::uint32_t>(encoded.size()));
    stream_.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    write_unsigned(stream_, codec::checksum(encoded));
    if (!stream_) {
        return JournalError{JournalErrorCode::io_error, records_written_};
    }
    ++records_written_;
    return std::nullopt;
}

std::optional<JournalError> RawJournalWriter::flush() {
    stream_.flush();
    return stream_ ? std::nullopt
                   : std::optional{JournalError{JournalErrorCode::io_error, records_written_}};
}

JournalReadResult RawJournalReader::read_next() {
    std::array<char, sizeof(std::uint32_t)> size_bytes{};
    stream_.read(size_bytes.data(), static_cast<std::streamsize>(size_bytes.size()));
    if (stream_.gcount() == 0 && stream_.eof()) {
        return EndOfJournal{};
    }
    if (stream_.gcount() != static_cast<std::streamsize>(size_bytes.size())) {
        return JournalError{JournalErrorCode::truncated_record, records_read_};
    }

    std::uint32_t record_size{};
    for (std::size_t index = 0U; index < size_bytes.size(); ++index) {
        record_size |= static_cast<std::uint32_t>(
                           static_cast<unsigned char>(size_bytes[index]))
                       << static_cast<unsigned int>(index * 8U);
    }
    if (record_size < codec::minimum_record_bytes) {
        return JournalError{JournalErrorCode::invalid_record, records_read_};
    }
    if (record_size > codec::maximum_record_bytes) {
        return JournalError{JournalErrorCode::record_too_large, records_read_};
    }

    std::vector<char> bytes(record_size);
    stream_.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return JournalError{JournalErrorCode::truncated_record, records_read_};
    }
    const auto recorded_checksum = read_unsigned<std::uint32_t>(stream_);
    if (!recorded_checksum.has_value()) {
        return JournalError{JournalErrorCode::truncated_record, records_read_};
    }
    if (*recorded_checksum != codec::checksum(bytes)) {
        return JournalError{JournalErrorCode::checksum_mismatch, records_read_};
    }

    auto decoded = codec::decode(bytes, records_read_);
    if (std::holds_alternative<RawMarketRecord>(decoded)) {
        ++records_read_;
    }
    return decoded;
}

}  // namespace eme::journal
