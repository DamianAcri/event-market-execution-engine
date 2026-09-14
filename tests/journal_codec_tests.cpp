#include "journal/journal_codec.hpp"
#include "test_support.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace {

// Deliberately independent, bit-at-a-time CRC-32/ISO-HDLC reference.
std::uint32_t reference_crc(const std::span<const char> bytes) {
    std::uint32_t crc = 0xffffffffU;
    for (const char value : bytes) {
        crc ^= static_cast<unsigned char>(value);
        for (unsigned int bit = 0; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xedb88320U : crc >> 1U;
        }
    }
    return crc ^ 0xffffffffU;
}

void test_crc_compatibility(eme::test::Context& test) {
    using eme::journal::codec::checksum;
    test.expect(checksum({}) == 0U, "CRC of empty input is zero");
    constexpr std::string_view check = "123456789";
    test.expect(checksum(std::span{check.data(), check.size()}) == 0xcbf43926U,
                "CRC matches the standard check vector (not CRC-32C)");

    for (unsigned int value = 0; value < 256U; ++value) {
        const std::array bytes{std::bit_cast<char>(static_cast<unsigned char>(value))};
        test.expect(checksum(bytes) == reference_crc(bytes),
                    "every byte value matches the independent reference");
    }

    std::vector<char> bytes(65'552U);
    std::uint32_t seed = 0x51a7e123U;
    for (auto& value : bytes) {
        seed = seed * 1'664'525U + 1'013'904'223U;
        value = std::bit_cast<char>(static_cast<unsigned char>(seed >> 24U));
    }
    constexpr std::array<std::size_t, 20> sizes{
        0U, 1U, 2U, 3U, 7U, 8U, 9U, 15U, 16U, 17U,
        31U, 32U, 33U, 255U, 256U, 257U, 4095U, 4096U, 4097U, 65'536U};
    for (std::size_t offset = 0; offset < 16U; ++offset) {
        for (const auto size : sizes) {
            const auto slice = std::span<const char>{bytes}.subspan(offset, size);
            test.expect(checksum(slice) == reference_crc(slice),
                        "unaligned and boundary-sized buffers preserve the wire CRC");
        }
    }
}

void test_generated_records(eme::test::Context& test) {
    namespace journal = eme::journal;
    for (std::size_t size = 1U; size <= 513U; size += 16U) {
        journal::RawMarketRecord record;
        record.metadata_version = 7U;
        record.connection_generation = 3U;
        record.sequence = size;
        record.channel = "synthetic";
        record.payload.resize(size);
        for (std::size_t index = 0; index < size; ++index) {
            record.payload[index] = std::bit_cast<char>(
                static_cast<unsigned char>((index * 37U + size) & 0xffU));
        }
        if ((size & 32U) != 0U) {
            record.exchange_time_ns = -123;
        }
        const auto encoded = journal::codec::encode(record);
        const auto* bytes = std::get_if<std::vector<char>>(&encoded);
        test.expect(bytes != nullptr, "generated binary payload encodes");
        if (bytes == nullptr) {
            continue;
        }
        test.expect(journal::codec::checksum(*bytes) == reference_crc(*bytes),
                    "encoded journal payload is compatible with the old checksum");
        const auto decoded = journal::codec::decode(*bytes, size);
        const auto* result = std::get_if<journal::RawMarketRecord>(&decoded);
        test.expect(result != nullptr && *result == record,
                    "all generated record fields round trip exactly");
        for (std::size_t cut = 0; cut < bytes->size(); ++cut) {
            test.expect(std::holds_alternative<journal::JournalError>(
                            journal::codec::decode(std::span{*bytes}.first(cut), size)),
                        "every incomplete prefix is rejected before replay");
        }
    }
}

}  // namespace

int main() {
    eme::test::Context test;
    test_crc_compatibility(test);
    test_generated_records(test);
    return test.result();
}
