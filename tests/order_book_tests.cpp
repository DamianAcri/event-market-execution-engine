#include "eme/book/order_book.hpp"
#include "eme/market/normalized_event.hpp"
#include "test_support.hpp"

#include <limits>
#include <variant>
#include <vector>

namespace {

void test_snapshot_and_deltas(eme::test::Context& test) {
    eme::book::OrderBook book;
    const std::vector bids{eme::test::level(6'200, 1'000), eme::test::level(6'100, 500)};
    const std::vector asks{eme::test::level(6'400, 800), eme::test::level(6'500, 900)};

    test.expect(book.apply_snapshot(9, 100, bids, asks) ==
                    eme::book::BookUpdateResult::applied,
                "valid snapshot is applied");
    test.expect(book.state() == eme::book::BookState::valid,
                "snapshot makes the book valid");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 6'200,
                "best bid is the highest bid");
    test.expect(book.best_ask().has_value() && book.best_ask()->raw() == 6'400,
                "best ask is the lowest ask");

    test.expect(book.apply_delta(
                    9, 101, eme::book::Side::bid, eme::test::price(6'300),
                    eme::test::delta(200)) == eme::book::BookUpdateResult::applied,
                "next sequenced delta is applied");
    test.expect(book.best_bid().has_value() && book.best_bid()->raw() == 6'300,
                "delta can establish a new best bid");

    test.expect(book.apply_delta(
                    9, 102, eme::book::Side::bid, eme::test::price(6'300),
                    eme::test::delta(-200)) == eme::book::BookUpdateResult::applied,
                "delta can remove an entire level");
    test.expect(book.level_count(eme::book::Side::bid) == 2U,
                "zero-quantity level is erased");

    test.expect(book.apply_delta(
                    9, 104, eme::book::Side::ask, eme::test::price(6'400),
                    eme::test::delta(-100)) == eme::book::BookUpdateResult::sequence_gap,
                "sequence gap is detected");
    test.expect(book.state() == eme::book::BookState::stale,
                "sequence gap invalidates the book");
    test.expect(!book.best_bid().has_value(),
                "stale book does not expose an actionable best bid");
    test.expect(book.apply_delta(
                    9, 105, eme::book::Side::ask, eme::test::price(6'400),
                    eme::test::delta(-100)) == eme::book::BookUpdateResult::requires_snapshot,
                "stale book rejects further deltas");

    test.expect(book.apply_snapshot(10, 1, bids, asks) ==
                    eme::book::BookUpdateResult::applied,
                "fresh snapshot recovers the book on a new stream");
    test.expect(book.apply_delta(
                    11, 2, eme::book::Side::ask, eme::test::price(6'400),
                    eme::test::delta(-100)) == eme::book::BookUpdateResult::stream_mismatch,
                "stream mismatch is detected");
    test.expect(book.state() == eme::book::BookState::stale,
                "stream mismatch invalidates the book");
}

void test_invalid_levels(eme::test::Context& test) {
    eme::book::OrderBook book;
    const std::vector duplicate_bids{
        eme::test::level(5'000, 100), eme::test::level(5'000, 200)};

    test.expect(book.apply_snapshot(1, 1, duplicate_bids, {}) ==
                    eme::book::BookUpdateResult::invalid_level,
                "duplicate snapshot price is rejected");
    test.expect(book.state() == eme::book::BookState::stale,
                "invalid snapshot leaves the book stale");
    test.expect(book.apply_snapshot(2, 1, {eme::test::level(5'000, 100)}, {}) ==
                    eme::book::BookUpdateResult::applied,
                "valid snapshot recovers after invalid input");
    test.expect(book.apply_delta(
                    2, 2, eme::book::Side::bid, eme::test::price(5'000),
                    eme::test::delta(-101)) == eme::book::BookUpdateResult::invalid_level,
                "delta cannot make a level negative");
    test.expect(book.state() == eme::book::BookState::stale,
                "negative resulting quantity fails closed");

    test.expect(book.apply_snapshot(
                    3, 1, {eme::test::level(
                              5'000, std::numeric_limits<std::int64_t>::max())}, {}) ==
                    eme::book::BookUpdateResult::applied,
                "maximum representable quantity can be snapshotted");
    test.expect(book.apply_delta(
                    3, 2, eme::book::Side::bid, eme::test::price(5'000),
                    eme::test::delta(1)) == eme::book::BookUpdateResult::invalid_level,
                "positive quantity overflow fails closed");

    test.expect(book.apply_snapshot(4, 1, {eme::test::level(5'000, 100)}, {}) ==
                    eme::book::BookUpdateResult::applied,
                "fresh snapshot recovers after overflow");
    test.expect(book.apply_delta(
                    4, 2, eme::book::Side::bid, eme::test::price(5'000),
                    eme::test::delta(std::numeric_limits<std::int64_t>::min())) ==
                    eme::book::BookUpdateResult::invalid_level,
                "minimum signed delta is rejected without arithmetic overflow");
}

void test_normalized_event_model(eme::test::Context& test) {
    const eme::market::BookDelta delta{
        7U,
        1U,
        3U,
        11U,
        eme::market::ReceiveTime{},
        eme::book::Side::bid,
        eme::test::price(4'200),
        eme::test::delta(100),
    };
    const eme::market::NormalizedMarketEvent event{delta};
    test.expect(std::holds_alternative<eme::market::BookDelta>(event),
                "normalized event variant carries a book delta");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_snapshot_and_deltas(test);
    test_invalid_levels(test);
    test_normalized_event_model(test);
    return test.result();
}
