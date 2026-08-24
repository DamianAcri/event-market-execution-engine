#include "eme/core/fixed_point.hpp"
#include "test_support.hpp"

int main() {
    eme::test::Context test;

    const auto parsed_price = eme::core::Price::parse("0.6300");
    test.expect(parsed_price.has_value() && parsed_price->raw() == 6'300,
                "price parses four exact decimal places");

    const auto whole_dollar = eme::core::Price::parse("1");
    test.expect(whole_dollar.has_value() && whole_dollar->raw() == 10'000,
                "whole-dollar price is scaled exactly");
    test.expect(!eme::core::Price::parse("1.0001").has_value(),
                "price above the payout range is rejected");
    test.expect(!eme::core::Price::parse("0.12345").has_value(),
                "price with excess precision is rejected");
    test.expect(!eme::core::Price::parse("-0.1").has_value(),
                "negative price is rejected");

    const auto parsed_quantity = eme::core::Quantity::parse("42.50");
    test.expect(parsed_quantity.has_value() && parsed_quantity->raw() == 4'250,
                "fractional quantity is scaled exactly");
    test.expect(!eme::core::Quantity::parse("42.501").has_value(),
                "quantity with excess precision is rejected");

    const auto negative_delta = eme::core::QuantityDelta::parse("-54.00");
    test.expect(negative_delta.has_value() && negative_delta->raw() == -5'400,
                "signed quantity delta is parsed exactly");
    const auto positive_delta = eme::core::QuantityDelta::parse("1.25");
    test.expect(positive_delta.has_value() && positive_delta->raw() == 125,
                "positive quantity delta uses quantity scale");
    test.expect(!eme::core::QuantityDelta::parse("1.251").has_value(),
                "quantity delta with excess precision is rejected");

    return test.result();
}
