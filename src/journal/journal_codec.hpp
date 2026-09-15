#pragma once

#include "eme/journal/raw_journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace eme::journal::codec {

inline constexpr std::array<char, 8U> file_magic{
    'E', 'M', 'E', 'J', 'N', 'L', '0', '2'};
inline constexpr std::uint32_t file_format_version = 2U;
inline constexpr std::size_t maximum_record_bytes =
    (16U * 1024U * 1024U) + 256U + 80U;
inline constexpr std::size_t minimum_record_bytes = 53U;

using EncodeResult = std::variant<std::vector<char>, JournalErrorCode>;

[[nodiscard]] EncodeResult encode(const RawMarketRecord& record);
// Reuse caller-owned storage; invalid input leaves the buffer unchanged.
[[nodiscard]] std::optional<JournalErrorCode> encode_into(
    const RawMarketRecord& record, std::vector<char>& bytes);
[[nodiscard]] JournalReadResult decode(
    std::span<const char> bytes,
    std::uint64_t record_index);
[[nodiscard]] std::uint32_t checksum(std::span<const char> bytes) noexcept;

}  // namespace eme::journal::codec
