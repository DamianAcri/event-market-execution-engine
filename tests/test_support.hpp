#pragma once

#include "eme/book/order_book.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace eme::test {

class Context final {
public:
    void expect(const bool condition, const std::string_view message) {
        ++checks_;
        if (!condition) {
            ++failures_;
            std::cerr << "FAIL: " << message << '\n';
        }
    }

    [[nodiscard]] int result() const {
        if (failures_ == 0) {
            std::cout << "PASS: " << checks_ << " checks\n";
            return 0;
        }
        std::cerr << "FAILED: " << failures_ << " of " << checks_ << " checks\n";
        return 1;
    }

private:
    int checks_{};
    int failures_{};
};

[[nodiscard]] inline core::Price price(const std::int64_t raw) {
    return *core::Price::from_raw(raw);
}

[[nodiscard]] inline core::Quantity quantity(const std::int64_t raw) {
    return *core::Quantity::from_raw(raw);
}

[[nodiscard]] inline core::QuantityDelta delta(const std::int64_t raw) {
    return core::QuantityDelta::from_raw(raw);
}

[[nodiscard]] inline book::Level level(
    const std::int64_t price_raw,
    const std::int64_t quantity_raw) {
    return {price(price_raw), quantity(quantity_raw)};
}

}  // namespace eme::test
