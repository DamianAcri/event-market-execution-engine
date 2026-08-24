#include "eme/book/order_book.hpp"
#include "eme/gateway/kalshi/orderbook_normalizer.hpp"
#include "test_support.hpp"

#include <variant>

namespace {

namespace kalshi = eme::gateway::kalshi;

void test_snapshot(eme::test::Context& test) {
    const kalshi::WireOrderBookSnapshot wire{
        7U,
        2U,
        2U,
        eme::market::ReceiveTime{},
        {{"0.0800", "300.00"}, {"0.2200", "333.00"}},
        {{"0.4600", "20.00"}, {"0.4400", "146.00"}},
    };

    const auto result = kalshi::normalize_orderbook_snapshot(wire);
    const auto* normalized = std::get_if<eme::market::BookSnapshot>(&result);
    test.expect(normalized != nullptr, "unified YES-scale snapshot normalizes");
    if (normalized == nullptr) {
        return;
    }

    eme::book::OrderBook book;
    test.expect(book.apply_snapshot(
                    normalized->stream_id,
                    normalized->sequence,
                    normalized->bids,
                    normalized->asks) == eme::book::BookUpdateResult::applied,
                "normalized snapshot applies to the venue-neutral book");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 2'200,
                "YES level remains a bid");
    test.expect(book.best_ask().has_value() && book.best_ask()->raw() == 4'400,
                "NO level already expressed as YES price becomes an ask");
    test.expect(book.quantity_at(eme::book::Side::ask, eme::test::price(4'400)).raw() ==
                    14'600,
                "NO-side quantity is preserved");

    auto zero_quantity = wire;
    zero_quantity.yes_bids = {{"0.5000", "0.00"}};
    const auto invalid = kalshi::normalize_orderbook_snapshot(zero_quantity);
    const auto* error = std::get_if<kalshi::NormalizationError>(&invalid);
    test.expect(error != nullptr && *error == kalshi::NormalizationError::zero_quantity,
                "zero-quantity snapshot level is rejected");
}

void test_delta(eme::test::Context& test) {
    const kalshi::WireOrderBookDelta yes_wire{
        7U,
        2U,
        3U,
        eme::market::ReceiveTime{},
        kalshi::OutcomeSide::yes,
        "0.960",
        "-54.00",
    };
    const auto yes_result = kalshi::normalize_orderbook_delta(yes_wire);
    const auto* yes_delta = std::get_if<eme::market::BookDelta>(&yes_result);
    test.expect(yes_delta != nullptr, "YES delta normalizes");
    if (yes_delta != nullptr) {
        test.expect(yes_delta->side == eme::book::Side::bid &&
                        yes_delta->price.raw() == 9'600,
                    "YES delta maps to a normalized bid");
        test.expect(yes_delta->quantity_delta.raw() == -5'400,
                    "signed fixed-point delta is preserved");
    }

    auto no_wire = yes_wire;
    no_wire.outcome_side = kalshi::OutcomeSide::no;
    no_wire.price_dollars = "0.7000";
    no_wire.quantity_delta_fp = "10.00";
    const auto no_result = kalshi::normalize_orderbook_delta(no_wire);
    const auto* no_delta = std::get_if<eme::market::BookDelta>(&no_result);
    test.expect(no_delta != nullptr && no_delta->side == eme::book::Side::ask &&
                    no_delta->price.raw() == 7'000,
                "NO delta retains its YES-scale ask price");

    auto invalid_wire = yes_wire;
    invalid_wire.price_dollars = "1.0001";
    const auto invalid_price = kalshi::normalize_orderbook_delta(invalid_wire);
    const auto* price_error = std::get_if<kalshi::NormalizationError>(&invalid_price);
    test.expect(price_error != nullptr &&
                    *price_error == kalshi::NormalizationError::invalid_price,
                "out-of-range wire price is rejected");

    invalid_wire.price_dollars = "0.5000";
    invalid_wire.quantity_delta_fp = "1.001";
    const auto invalid_quantity = kalshi::normalize_orderbook_delta(invalid_wire);
    const auto* quantity_error = std::get_if<kalshi::NormalizationError>(&invalid_quantity);
    test.expect(quantity_error != nullptr &&
                    *quantity_error == kalshi::NormalizationError::invalid_quantity,
                "wire delta with excess precision is rejected");

    invalid_wire.quantity_delta_fp = "0.00";
    const auto zero_delta = kalshi::normalize_orderbook_delta(invalid_wire);
    const auto* zero_error = std::get_if<kalshi::NormalizationError>(&zero_delta);
    test.expect(zero_error != nullptr && *zero_error == kalshi::NormalizationError::zero_delta,
                "zero wire delta is rejected");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_snapshot(test);
    test_delta(test);
    return test.result();
}
