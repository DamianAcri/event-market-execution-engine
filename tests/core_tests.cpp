#include "eme/book/order_book.hpp"
#include "eme/core/fixed_point.hpp"
#include "eme/gateway/kalshi/orderbook_normalizer.hpp"
#include "eme/market/normalized_event.hpp"

#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

class TestContext final {
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

[[nodiscard]] eme::core::Price price(const std::int64_t raw) {
    return *eme::core::Price::from_raw(raw);
}

[[nodiscard]] eme::core::Quantity quantity(const std::int64_t raw) {
    return *eme::core::Quantity::from_raw(raw);
}

[[nodiscard]] eme::book::Level level(
    const std::int64_t price_raw,
    const std::int64_t quantity_raw) {
    return {price(price_raw), quantity(quantity_raw)};
}

void test_fixed_point(TestContext& test) {
    const auto parsed_price = eme::core::Price::parse("0.6300");
    test.expect(parsed_price.has_value() && parsed_price->raw() == 6'300,
                "price parses four exact decimal places");

    const auto whole_dollar = eme::core::Price::parse("1");
    test.expect(whole_dollar.has_value() && whole_dollar->raw() == 10'000,
                "whole-dollar price is scaled exactly");

    test.expect(!eme::core::Price::parse("1.0001").has_value(),
                "price above the event-contract payout range is rejected");
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
}

void test_snapshot_and_deltas(TestContext& test) {
    eme::book::OrderBook book;
    const std::vector bids{level(6'200, 1'000), level(6'100, 500)};
    const std::vector asks{level(6'400, 800), level(6'500, 900)};

    test.expect(book.apply_snapshot(9, 100, bids, asks) ==
                    eme::book::BookUpdateResult::applied,
                "valid snapshot is applied");
    test.expect(book.state() == eme::book::BookState::valid,
                "snapshot makes the book valid");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 6'200,
                "best bid is the highest bid");
    test.expect(book.best_ask().has_value() && book.best_ask()->raw() == 6'400,
                "best ask is the lowest ask");

    test.expect(book.apply_delta(9, 101, eme::book::Side::bid, price(6'300), 200) ==
                    eme::book::BookUpdateResult::applied,
                "next sequenced delta is applied");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 6'300,
                "delta can establish a new best bid");

    test.expect(book.apply_delta(9, 102, eme::book::Side::bid, price(6'300), -200) ==
                    eme::book::BookUpdateResult::applied,
                "delta can remove an entire level");
    test.expect(book.level_count(eme::book::Side::bid) == 2U,
                "zero-quantity level is erased");

    test.expect(book.apply_delta(9, 104, eme::book::Side::ask, price(6'400), -100) ==
                    eme::book::BookUpdateResult::sequence_gap,
                "sequence gap is detected");
    test.expect(book.state() == eme::book::BookState::stale,
                "sequence gap invalidates the book");
    test.expect(!book.best_bid().has_value(),
                "stale book does not expose an actionable best bid");
    test.expect(book.apply_delta(9, 105, eme::book::Side::ask, price(6'400), -100) ==
                    eme::book::BookUpdateResult::requires_snapshot,
                "stale book rejects further deltas");

    test.expect(book.apply_snapshot(10, 1, bids, asks) ==
                    eme::book::BookUpdateResult::applied,
                "fresh snapshot recovers the book on a new stream");
    test.expect(book.apply_delta(11, 2, eme::book::Side::ask, price(6'400), -100) ==
                    eme::book::BookUpdateResult::stream_mismatch,
                "stream mismatch is detected");
    test.expect(book.state() == eme::book::BookState::stale,
                "stream mismatch invalidates the book");
}

void test_invalid_levels(TestContext& test) {
    eme::book::OrderBook book;
    const std::vector duplicate_bids{level(5'000, 100), level(5'000, 200)};

    test.expect(book.apply_snapshot(1, 1, duplicate_bids, {}) ==
                    eme::book::BookUpdateResult::invalid_level,
                "duplicate snapshot price is rejected");
    test.expect(book.state() == eme::book::BookState::stale,
                "invalid snapshot leaves the book stale");

    test.expect(book.apply_snapshot(2, 1, {level(5'000, 100)}, {}) ==
                    eme::book::BookUpdateResult::applied,
                "valid snapshot recovers after invalid input");
    test.expect(book.apply_delta(2, 2, eme::book::Side::bid, price(5'000), -101) ==
                    eme::book::BookUpdateResult::invalid_level,
                "delta cannot make a level negative");
    test.expect(book.state() == eme::book::BookState::stale,
                "negative resulting quantity fails closed");
}

void test_normalized_event_model(TestContext& test) {
    const eme::market::BookDelta delta{
        7U,
        3U,
        11U,
        eme::market::ReceiveTime{},
        eme::book::Side::bid,
        price(4'200),
        eme::core::QuantityDelta::from_raw(100),
    };
    const eme::market::NormalizedMarketEvent event{delta};
    test.expect(std::holds_alternative<eme::market::BookDelta>(event),
                "normalized event variant carries a book delta");
}

void test_kalshi_legacy_snapshot_fixture(TestContext& test) {
    namespace kalshi = eme::gateway::kalshi;

    const kalshi::WireOrderBookSnapshot wire{
        7U,
        2U,
        2U,
        eme::market::ReceiveTime{},
        kalshi::BookPriceConvention::legacy_separate_scales,
        {{"0.0800", "300.00"}, {"0.2200", "333.00"}},
        {{"0.5400", "20.00"}, {"0.5600", "146.00"}},
    };

    const auto result = kalshi::normalize_orderbook_snapshot(wire);
    test.expect(std::holds_alternative<eme::market::BookSnapshot>(result),
                "official legacy Kalshi snapshot fixture normalizes");
    if (!std::holds_alternative<eme::market::BookSnapshot>(result)) {
        return;
    }

    const auto& normalized = std::get<eme::market::BookSnapshot>(result);
    eme::book::OrderBook book;
    test.expect(book.apply_snapshot(
                    normalized.stream_id,
                    normalized.sequence,
                    normalized.bids,
                    normalized.asks) == eme::book::BookUpdateResult::applied,
                "normalized Kalshi snapshot applies to venue-neutral book");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 2'200,
                "YES bid remains a bid on the YES scale");
    test.expect(book.best_ask().has_value() && book.best_ask()->raw() == 4'400,
                "legacy NO bid is complemented into a YES ask");
    test.expect(book.quantity_at(eme::book::Side::ask, price(4'400)).raw() == 14'600,
                "NO-side quantity is preserved during normalization");

    auto zero_quantity_wire = wire;
    zero_quantity_wire.yes_bids = {{"0.5000", "0.00"}};
    const auto zero_quantity_result = kalshi::normalize_orderbook_snapshot(zero_quantity_wire);
    test.expect(std::holds_alternative<kalshi::NormalizationError>(zero_quantity_result) &&
                    std::get<kalshi::NormalizationError>(zero_quantity_result) ==
                        kalshi::NormalizationError::zero_quantity,
                "zero-quantity snapshot level is rejected");
}

void test_kalshi_unified_price_normalization(TestContext& test) {
    namespace kalshi = eme::gateway::kalshi;

    const kalshi::WireOrderBookSnapshot wire{
        7U,
        2U,
        2U,
        eme::market::ReceiveTime{},
        kalshi::BookPriceConvention::unified_yes_scale,
        {{"0.0800", "300.00"}, {"0.2200", "333.00"}},
        {{"0.4600", "20.00"}, {"0.4400", "146.00"}},
    };

    const auto result = kalshi::normalize_orderbook_snapshot(wire);
    test.expect(std::holds_alternative<eme::market::BookSnapshot>(result),
                "unified YES-scale Kalshi snapshot normalizes");
    if (!std::holds_alternative<eme::market::BookSnapshot>(result)) {
        return;
    }

    const auto& normalized = std::get<eme::market::BookSnapshot>(result);
    eme::book::OrderBook book;
    test.expect(book.apply_snapshot(
                    normalized.stream_id,
                    normalized.sequence,
                    normalized.bids,
                    normalized.asks) == eme::book::BookUpdateResult::applied,
                "unified snapshot applies to venue-neutral book");
    test.expect(book.best_ask().has_value() && book.best_ask()->raw() == 4'400,
                "unified NO level is already expressed as a YES ask");
}

void test_kalshi_delta_normalization(TestContext& test) {
    namespace kalshi = eme::gateway::kalshi;

    const kalshi::WireOrderBookDelta official_yes_delta{
        7U,
        2U,
        3U,
        eme::market::ReceiveTime{},
        kalshi::BookPriceConvention::legacy_separate_scales,
        kalshi::OutcomeSide::yes,
        "0.960",
        "-54.00",
    };
    const auto yes_result = kalshi::normalize_orderbook_delta(official_yes_delta);
    test.expect(std::holds_alternative<eme::market::BookDelta>(yes_result),
                "official Kalshi delta fixture normalizes");
    if (std::holds_alternative<eme::market::BookDelta>(yes_result)) {
        const auto& delta = std::get<eme::market::BookDelta>(yes_result);
        test.expect(delta.side == eme::book::Side::bid && delta.price.raw() == 9'600,
                    "YES delta maps to a normalized bid");
        test.expect(delta.quantity_delta.raw() == -5'400,
                    "signed fixed-point delta is preserved");
    }

    auto legacy_no_delta = official_yes_delta;
    legacy_no_delta.outcome_side = kalshi::OutcomeSide::no;
    legacy_no_delta.price_dollars = "0.3000";
    legacy_no_delta.quantity_delta_fp = "10.00";
    const auto legacy_result = kalshi::normalize_orderbook_delta(legacy_no_delta);
    test.expect(std::holds_alternative<eme::market::BookDelta>(legacy_result) &&
                    std::get<eme::market::BookDelta>(legacy_result).side == eme::book::Side::ask &&
                    std::get<eme::market::BookDelta>(legacy_result).price.raw() == 7'000,
                "legacy NO delta is complemented into a YES ask");

    auto unified_no_delta = legacy_no_delta;
    unified_no_delta.price_convention = kalshi::BookPriceConvention::unified_yes_scale;
    unified_no_delta.price_dollars = "0.7000";
    const auto unified_result = kalshi::normalize_orderbook_delta(unified_no_delta);
    test.expect(std::holds_alternative<eme::market::BookDelta>(unified_result) &&
                    std::get<eme::market::BookDelta>(unified_result).price.raw() == 7'000,
                "unified NO delta retains its YES-scale ask price");

    auto invalid_delta = official_yes_delta;
    invalid_delta.price_dollars = "1.0001";
    const auto invalid_price_result = kalshi::normalize_orderbook_delta(invalid_delta);
    test.expect(std::holds_alternative<kalshi::NormalizationError>(invalid_price_result) &&
                    std::get<kalshi::NormalizationError>(invalid_price_result) ==
                        kalshi::NormalizationError::invalid_price,
                "out-of-range wire price is rejected");
    invalid_delta.price_dollars = "0.5000";
    invalid_delta.quantity_delta_fp = "1.001";
    const auto invalid_quantity_result = kalshi::normalize_orderbook_delta(invalid_delta);
    test.expect(std::holds_alternative<kalshi::NormalizationError>(invalid_quantity_result) &&
                    std::get<kalshi::NormalizationError>(invalid_quantity_result) ==
                        kalshi::NormalizationError::invalid_quantity,
                "wire delta with excess precision is rejected");
    invalid_delta.quantity_delta_fp = "0.00";
    const auto zero_delta_result = kalshi::normalize_orderbook_delta(invalid_delta);
    test.expect(std::holds_alternative<kalshi::NormalizationError>(zero_delta_result) &&
                    std::get<kalshi::NormalizationError>(zero_delta_result) ==
                        kalshi::NormalizationError::zero_delta,
                "zero wire delta is rejected");
}

}  // namespace

int main() {
    TestContext test;
    test_fixed_point(test);
    test_snapshot_and_deltas(test);
    test_invalid_levels(test);
    test_normalized_event_model(test);
    test_kalshi_legacy_snapshot_fixture(test);
    test_kalshi_unified_price_normalization(test);
    test_kalshi_delta_normalization(test);
    return test.result();
}
