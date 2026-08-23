#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace eme::core {
namespace detail {

template <std::int64_t Scale, std::size_t FractionDigits>
[[nodiscard]] constexpr std::optional<std::int64_t> parse_scaled_nonnegative(
    const std::string_view text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }

    const auto dot = text.find('.');
    if (dot != std::string_view::npos && text.find('.', dot + 1U) != std::string_view::npos) {
        return std::nullopt;
    }

    const auto whole_text = text.substr(0U, dot);
    if (whole_text.empty()) {
        return std::nullopt;
    }

    std::int64_t whole = 0;
    for (const char character : whole_text) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const auto digit = static_cast<std::int64_t>(character - '0');
        if (whole > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        whole = (whole * 10) + digit;
    }

    if (whole > std::numeric_limits<std::int64_t>::max() / Scale) {
        return std::nullopt;
    }

    std::int64_t fraction = 0;
    std::size_t fraction_size = 0U;
    if (dot != std::string_view::npos) {
        const auto fraction_text = text.substr(dot + 1U);
        if (fraction_text.empty() || fraction_text.size() > FractionDigits) {
            return std::nullopt;
        }
        fraction_size = fraction_text.size();
        for (const char character : fraction_text) {
            if (character < '0' || character > '9') {
                return std::nullopt;
            }
            fraction = (fraction * 10) + static_cast<std::int64_t>(character - '0');
        }
    }

    for (; fraction_size < FractionDigits; ++fraction_size) {
        fraction *= 10;
    }

    const auto scaled_whole = whole * Scale;
    if (scaled_whole > std::numeric_limits<std::int64_t>::max() - fraction) {
        return std::nullopt;
    }
    return scaled_whole + fraction;
}

}  // namespace detail

class Price final {
public:
    static constexpr std::int64_t scale = 10'000;

    [[nodiscard]] static constexpr std::optional<Price> from_raw(
        const std::int64_t raw) noexcept {
        if (raw < 0 || raw > scale) {
            return std::nullopt;
        }
        return Price{raw};
    }

    [[nodiscard]] static constexpr std::optional<Price> parse(
        const std::string_view text) noexcept {
        const auto raw = detail::parse_scaled_nonnegative<scale, 4U>(text);
        return raw.has_value() ? from_raw(*raw) : std::nullopt;
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }

    friend constexpr auto operator<=>(const Price&, const Price&) = default;

private:
    explicit constexpr Price(const std::int64_t raw) noexcept : raw_{raw} {}

    std::int64_t raw_{};
};

class Quantity final {
public:
    static constexpr std::int64_t scale = 100;

    [[nodiscard]] static constexpr std::optional<Quantity> from_raw(
        const std::int64_t raw) noexcept {
        if (raw < 0) {
            return std::nullopt;
        }
        return Quantity{raw};
    }

    [[nodiscard]] static constexpr std::optional<Quantity> parse(
        const std::string_view text) noexcept {
        const auto raw = detail::parse_scaled_nonnegative<scale, 2U>(text);
        return raw.has_value() ? from_raw(*raw) : std::nullopt;
    }

    [[nodiscard]] constexpr std::int64_t raw() const noexcept { return raw_; }

    friend constexpr auto operator<=>(const Quantity&, const Quantity&) = default;

private:
    explicit constexpr Quantity(const std::int64_t raw) noexcept : raw_{raw} {}

    std::int64_t raw_{};
};

}  // namespace eme::core
