#pragma once

#include "eme/constraint/payoff.hpp"
#include "eme/gateway/kalshi/market_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace eme::gateway::kalshi {

inline constexpr std::size_t maximum_metadata_bytes = 4U * 1024U * 1024U;

enum class MetadataErrorCode : std::uint8_t {
    invalid_json,
    duplicate_field,
    invalid_shape,
    invalid_value,
    unsupported_version,
    duplicate_market,
    duplicate_constraint,
    unknown_market,
    input_too_large,
    nesting_too_deep,
};

struct MetadataError final {
    MetadataErrorCode code{};
    std::string field;
};

// Owns both registries. Keep the snapshot alive and unmoved while borrowing them.
// Loading creates a new object; it never modifies a registry in a running session.
class MetadataSnapshot final {
public:
    [[nodiscard]] const MarketRegistry& markets() const & noexcept { return markets_; }
    const MarketRegistry& markets() const && = delete;
    [[nodiscard]] const constraint::ConstraintRegistry& constraints() const & noexcept {
        return constraints_;
    }
    const constraint::ConstraintRegistry& constraints() const && = delete;
    [[nodiscard]] const std::string& canonical_json() const & noexcept { return canonical_; }
    const std::string& canonical_json() const && = delete;

private:
    explicit MetadataSnapshot(market::MetadataVersion version) : markets_{version} {}
    friend std::variant<MetadataSnapshot, MetadataError> parse_metadata_snapshot(
        std::string_view input);

    MarketRegistry markets_;
    constraint::ConstraintRegistry constraints_;
    std::string canonical_;
};

using MetadataResult = std::variant<MetadataSnapshot, MetadataError>;

[[nodiscard]] MetadataResult parse_metadata_snapshot(std::string_view input);
[[nodiscard]] std::string_view to_string(MetadataErrorCode code) noexcept;

}  // namespace eme::gateway::kalshi
