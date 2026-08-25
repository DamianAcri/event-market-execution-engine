#include "journal_codec.hpp"

#include <bit>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace eme::journal::codec {
namespace {

constexpr std::size_t maximum_channel_bytes = 256U;
constexpr std::size_t maximum_payload_bytes = 16U * 1024U * 1024U;

template <typename Value>
void append_unsigned(std::vector<char>& bytes, const Value value) {
    static_assert(std::is_unsigned_v<Value>);
    for (std::size_t index = 0U; index < sizeof(Value); ++index) {
        const auto shift = static_cast<unsigned int>(index * 8U);
        bytes.push_back(static_cast<char>((value >> shift) & static_cast<Value>(0xffU)));
    }
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
            value |= byte << static_cast<unsigned int>(index * 8U);
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

[[nodiscard]] bool valid(const RawMarketRecord& record) {
    return record.schema_version == current_schema_version &&
           record.metadata_version != 0U && record.connection_generation != 0U &&
           !record.channel.empty() && record.channel.size() <= maximum_channel_bytes &&
           !record.payload.empty() && record.payload.size() <= maximum_payload_bytes;
}

}  // namespace

EncodeResult encode(const RawMarketRecord& record) {
    if (!valid(record)) {
        return JournalErrorCode::invalid_record;
    }
    std::vector<char> bytes;
    bytes.reserve(80U + record.channel.size() + record.payload.size());
    append_unsigned(bytes, record.schema_version);
    append_unsigned(bytes, record.metadata_version);
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

JournalReadResult decode(
    const std::span<const char> bytes,
    const std::uint64_t record_index) {
    RecordReader reader{bytes};
    const auto schema_version = reader.take_unsigned<std::uint32_t>();
    const auto metadata_version = reader.take_unsigned<std::uint64_t>();
    const auto generation = reader.take_unsigned<std::uint64_t>();
    const auto received_bits = reader.take_unsigned<std::uint64_t>();
    const auto observed_bits = reader.take_unsigned<std::uint64_t>();
    const auto sequence = reader.take_unsigned<std::uint64_t>();
    const auto flags = reader.take_unsigned<std::uint8_t>();
    if (!schema_version.has_value() || !metadata_version.has_value() ||
        !generation.has_value() || !received_bits.has_value() ||
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
        *metadata_version == 0U || *generation == 0U) {
        return JournalError{JournalErrorCode::invalid_record, record_index};
    }

    return RawMarketRecord{
        *schema_version,
        *metadata_version,
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

std::uint32_t checksum(const std::span<const char> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const char character : bytes) {
        crc ^= static_cast<std::uint32_t>(static_cast<unsigned char>(character));
        for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
            const auto mask = static_cast<std::uint32_t>(
                -static_cast<std::int32_t>(crc & 1U));
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

}  // namespace eme::journal::codec
