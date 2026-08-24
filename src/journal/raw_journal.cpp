#include "eme/journal/raw_journal.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace eme::journal {
namespace {

constexpr std::array<char, 8U> file_magic{'E', 'M', 'E', 'J', 'N', 'L', '0', '1'};
constexpr std::uint32_t file_format_version = 1U;
constexpr std::size_t maximum_channel_bytes = 256U;
constexpr std::size_t maximum_payload_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_record_bytes = maximum_payload_bytes + maximum_channel_bytes + 64U;
constexpr std::size_t minimum_record_bytes = 45U;

template <typename Value>
void append_unsigned(std::vector<char>& bytes, const Value value) {
    static_assert(std::is_unsigned_v<Value>);
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes.push_back(static_cast<char>((value >> shift) & static_cast<Value>(0xffU)));
    }
}

template <typename Value>
void write_unsigned(std::ostream& stream, const Value value) {
    std::vector<char> bytes;
    bytes.reserve(sizeof(Value));
    append_unsigned(bytes, value);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

class RecordReader final {
public:
    explicit RecordReader(const std::span<const char> bytes) : bytes_{bytes} {}

    template <typename Value>
    [[nodiscard]] std::optional<Value> take_unsigned() {
        static_assert(std::is_unsigned_v<Value>);
        if (remaining() < sizeof(Value)) {
            return std::nullopt;
        }
        Value value{};
        for (std::size_t index = 0U; index < sizeof(Value); ++index) {
            const auto byte = static_cast<Value>(
                static_cast<unsigned char>(bytes_[offset_ + index]));
            const auto shift = static_cast<unsigned int>(index * 8U);
            value |= byte << shift;
        }
        offset_ += sizeof(Value);
        return value;
    }

    [[nodiscard]] std::optional<std::string> take_string(const std::size_t size) {
        if (remaining() < size) {
            return std::nullopt;
        }
        std::string value{bytes_.data() + offset_, size};
        offset_ += size;
        return value;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

private:
    std::span<const char> bytes_;
    std::size_t offset_{};
};

[[nodiscard]] std::optional<std::uint32_t> read_u32(std::istream& stream) {
    std::array<char, sizeof(std::uint32_t)> bytes{};
    stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return std::nullopt;
    }
    RecordReader reader{bytes};
    return reader.take_unsigned<std::uint32_t>();
}

[[nodiscard]] std::optional<JournalError> validate_header(std::istream& stream) {
    std::array<char, file_magic.size()> magic{};
    stream.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (stream.gcount() != static_cast<std::streamsize>(magic.size()) || magic != file_magic) {
        return JournalError{JournalErrorCode::invalid_header, 0U};
    }
    const auto version = read_u32(stream);
    if (!version.has_value()) {
        return JournalError{JournalErrorCode::invalid_header, 0U};
    }
    if (*version != file_format_version) {
        return JournalError{JournalErrorCode::unsupported_version, 0U};
    }
    return std::nullopt;
}

[[nodiscard]] bool record_is_valid(const RawMarketRecord& record) {
    return record.schema_version == current_schema_version &&
           record.connection_generation != 0U && !record.channel.empty() &&
           record.channel.size() <= maximum_channel_bytes && !record.payload.empty() &&
           record.payload.size() <= maximum_payload_bytes;
}

[[nodiscard]] std::vector<char> encode_record(const RawMarketRecord& record) {
    std::vector<char> bytes;
    bytes.reserve(64U + record.channel.size() + record.payload.size());
    append_unsigned(bytes, record.schema_version);
    append_unsigned(bytes, record.connection_generation);
    append_unsigned(
        bytes,
        std::bit_cast<std::uint64_t>(record.received_at.time_since_epoch().count()));
    append_unsigned(
        bytes,
        std::bit_cast<std::uint64_t>(record.observed_at.time_since_epoch().count()));
    append_unsigned(bytes, record.sequence);
    append_unsigned(
        bytes,
        static_cast<std::uint8_t>(record.exchange_time_ns.has_value() ? 1U : 0U));
    if (record.exchange_time_ns.has_value()) {
        append_unsigned(bytes, std::bit_cast<std::uint64_t>(*record.exchange_time_ns));
    }
    append_unsigned(bytes, static_cast<std::uint32_t>(record.channel.size()));
    append_unsigned(bytes, static_cast<std::uint32_t>(record.payload.size()));
    bytes.insert(bytes.end(), record.channel.begin(), record.channel.end());
    bytes.insert(bytes.end(), record.payload.begin(), record.payload.end());
    return bytes;
}

[[nodiscard]] JournalReadResult decode_record(
    const std::span<const char> bytes,
    const std::uint64_t record_index) {
    RecordReader reader{bytes};
    const auto schema_version = reader.take_unsigned<std::uint32_t>();
    const auto generation = reader.take_unsigned<std::uint64_t>();
    const auto received_bits = reader.take_unsigned<std::uint64_t>();
    const auto observed_bits = reader.take_unsigned<std::uint64_t>();
    const auto sequence = reader.take_unsigned<std::uint64_t>();
    const auto flags = reader.take_unsigned<std::uint8_t>();
    if (!schema_version.has_value() || !generation.has_value() || !received_bits.has_value() ||
        !observed_bits.has_value() || !sequence.has_value() || !flags.has_value()) {
        return JournalError{JournalErrorCode::truncated_record, record_index};
    }
    if (*schema_version != current_schema_version) {
        return JournalError{JournalErrorCode::unsupported_version, record_index};
    }
    if (*flags > 1U) {
        return JournalError{JournalErrorCode::invalid_record, record_index};
    }

    std::optional<std::int64_t> exchange_time;
    if (*flags == 1U) {
        const auto exchange_bits = reader.take_unsigned<std::uint64_t>();
        if (!exchange_bits.has_value()) {
            return JournalError{JournalErrorCode::truncated_record, record_index};
        }
        exchange_time = std::bit_cast<std::int64_t>(*exchange_bits);
    }

    const auto channel_size = reader.take_unsigned<std::uint32_t>();
    const auto payload_size = reader.take_unsigned<std::uint32_t>();
    if (!channel_size.has_value() || !payload_size.has_value()) {
        return JournalError{JournalErrorCode::truncated_record, record_index};
    }
    if (*channel_size == 0U || *channel_size > maximum_channel_bytes ||
        *payload_size == 0U || *payload_size > maximum_payload_bytes ||
        reader.remaining() != static_cast<std::size_t>(*channel_size) +
                                  static_cast<std::size_t>(*payload_size)) {
        return JournalError{JournalErrorCode::invalid_record, record_index};
    }

    auto channel = reader.take_string(*channel_size);
    auto payload = reader.take_string(*payload_size);
    if (!channel.has_value() || !payload.has_value() || reader.remaining() != 0U ||
        *generation == 0U) {
        return JournalError{JournalErrorCode::invalid_record, record_index};
    }

    return RawMarketRecord{
        *schema_version,
        *generation,
        market::ReceiveTime{std::chrono::nanoseconds{
            std::bit_cast<std::int64_t>(*received_bits)}},
        WallTime{std::chrono::nanoseconds{
            std::bit_cast<std::int64_t>(*observed_bits)}},
        *sequence,
        exchange_time,
        std::move(*channel),
        std::move(*payload),
    };
}

}  // namespace

RawJournalWriter::RawJournalWriter(const std::filesystem::path& path) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        initialization_error_ = JournalError{JournalErrorCode::open_failed, 0U};
        return;
    }

    bool write_header = true;
    if (exists) {
        const auto file_size = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error) {
            initialization_error_ = JournalError{JournalErrorCode::open_failed, 0U};
            return;
        }
        write_header = file_size == 0U;
        if (!write_header) {
            RawJournalReader existing{path};
            if (!existing.ready()) {
                initialization_error_ = existing.initialization_error();
                return;
            }
            while (true) {
                const auto next = existing.read_next();
                if (std::holds_alternative<EndOfJournal>(next)) {
                    break;
                }
                if (const auto* error = std::get_if<JournalError>(&next); error != nullptr) {
                    initialization_error_ = *error;
                    return;
                }
            }
            records_written_ = existing.records_read();
        }
    }

    stream_.open(path, std::ios::binary | std::ios::app);
    if (!stream_) {
        initialization_error_ = JournalError{JournalErrorCode::open_failed, 0U};
        return;
    }
    if (write_header) {
        stream_.write(file_magic.data(), static_cast<std::streamsize>(file_magic.size()));
        write_unsigned(stream_, file_format_version);
        if (!stream_) {
            initialization_error_ = JournalError{JournalErrorCode::io_error, 0U};
        }
    }
}

std::optional<JournalError> RawJournalWriter::append(const RawMarketRecord& record) {
    if (initialization_error_.has_value()) {
        return initialization_error_;
    }
    if (!record_is_valid(record)) {
        return JournalError{JournalErrorCode::invalid_record, records_written_};
    }

    const auto encoded = encode_record(record);
    if (encoded.size() > maximum_record_bytes ||
        encoded.size() > std::numeric_limits<std::uint32_t>::max()) {
        return JournalError{JournalErrorCode::record_too_large, records_written_};
    }
    write_unsigned(stream_, static_cast<std::uint32_t>(encoded.size()));
    stream_.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!stream_) {
        return JournalError{JournalErrorCode::io_error, records_written_};
    }
    ++records_written_;
    return std::nullopt;
}

std::optional<JournalError> RawJournalWriter::flush() {
    if (initialization_error_.has_value()) {
        return initialization_error_;
    }
    stream_.flush();
    return stream_ ? std::nullopt
                   : std::optional{JournalError{JournalErrorCode::io_error, records_written_}};
}

RawJournalReader::RawJournalReader(const std::filesystem::path& path)
    : stream_{path, std::ios::binary} {
    if (!stream_) {
        initialization_error_ = JournalError{JournalErrorCode::open_failed, 0U};
        return;
    }
    initialization_error_ = validate_header(stream_);
}

JournalReadResult RawJournalReader::read_next() {
    if (initialization_error_.has_value()) {
        return *initialization_error_;
    }

    std::array<char, sizeof(std::uint32_t)> size_bytes{};
    stream_.read(size_bytes.data(), static_cast<std::streamsize>(size_bytes.size()));
    if (stream_.gcount() == 0 && stream_.eof()) {
        return EndOfJournal{};
    }
    if (stream_.gcount() != static_cast<std::streamsize>(size_bytes.size())) {
        return JournalError{JournalErrorCode::truncated_record, records_read_};
    }
    RecordReader size_reader{size_bytes};
    const auto record_size = size_reader.take_unsigned<std::uint32_t>();
    if (!record_size.has_value() || *record_size < minimum_record_bytes) {
        return JournalError{JournalErrorCode::invalid_record, records_read_};
    }
    if (*record_size > maximum_record_bytes) {
        return JournalError{JournalErrorCode::record_too_large, records_read_};
    }

    std::vector<char> bytes(*record_size);
    stream_.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (stream_.gcount() != static_cast<std::streamsize>(bytes.size())) {
        return JournalError{JournalErrorCode::truncated_record, records_read_};
    }

    auto decoded = decode_record(bytes, records_read_);
    if (std::holds_alternative<RawMarketRecord>(decoded)) {
        ++records_read_;
    }
    return decoded;
}

}  // namespace eme::journal
