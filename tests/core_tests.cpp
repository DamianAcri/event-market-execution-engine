#include "eme/book/order_book.hpp"
#include "eme/core/fixed_point.hpp"
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
        100,
    };
    const eme::market::NormalizedMarketEvent event{delta};
    test.expect(std::holds_alternative<eme::market::BookDelta>(event),
                "normalized event variant carries a book delta");
}

}  // namespace

int main() {
    TestContext test;
    test_fixed_point(test);
    test_snapshot_and_deltas(test);
    test_invalid_levels(test);
    test_normalized_event_model(test);
    return test.result();
}
