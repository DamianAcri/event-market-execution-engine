#include "eme/market/market_state.hpp"
#include "test_support.hpp"

#include <variant>
#include <vector>

namespace {

[[nodiscard]] bool book_result(
    const eme::market::MarketApplyResult& result,
    const eme::book::BookUpdateResult expected) {
    const auto* update = std::get_if<eme::book::BookUpdateResult>(&result);
    return update != nullptr && *update == expected;
}

[[nodiscard]] bool state_error(
    const eme::market::MarketApplyResult& result,
    const eme::market::MarketStateError expected) {
    const auto* error = std::get_if<eme::market::MarketStateError>(&result);
    return error != nullptr && *error == expected;
}

[[nodiscard]] eme::market::BookSnapshot snapshot(
    const eme::market::MarketId market_id,
    const eme::market::ConnectionGeneration generation,
    const eme::book::StreamId stream_id,
    const eme::book::SequenceNumber sequence) {
    return {
        market_id,
        generation,
        stream_id,
        sequence,
        eme::market::ReceiveTime{},
        {eme::test::level(4'000, 100)},
        {eme::test::level(6'000, 100)},
    };
}

[[nodiscard]] eme::market::BookDelta delta(
    const eme::market::MarketId market_id,
    const eme::market::ConnectionGeneration generation,
    const eme::book::StreamId stream_id,
    const eme::book::SequenceNumber sequence,
    const std::int64_t quantity_delta) {
    return {
        market_id,
        generation,
        stream_id,
        sequence,
        eme::market::ReceiveTime{},
        eme::book::Side::bid,
        eme::test::price(4'500),
        eme::test::delta(quantity_delta),
    };
}

void test_connection_lifecycle(eme::test::Context& test) {
    eme::market::MarketState state;
    test.expect(!state.connected() && !state.connection_generation().has_value(),
                "market state starts disconnected");
    test.expect(!state.open_connection(0U), "zero connection generation is invalid");
    test.expect(state.open_connection(1U), "first connection generation opens");
    test.expect(state.connected() && state.connection_generation() == 1U,
                "active generation is observable");
    test.expect(!state.open_connection(1U), "same generation cannot be reopened");
    test.expect(!state.open_connection(0U), "older generation cannot replace current state");

    test.expect(book_result(state.apply(snapshot(7U, 1U, 2U, 10U)),
                            eme::book::BookUpdateResult::applied),
                "snapshot creates a live market book");
    test.expect(state.book_count() == 1U && state.valid_book_count() == 1U,
                "book counters reflect live state");
    test.expect(book_result(state.apply(delta(7U, 1U, 2U, 11U, 50)),
                            eme::book::BookUpdateResult::applied),
                "current-generation delta is applied");

    test.expect(!state.close_connection(2U),
                "wrong generation cannot close the current connection");
    test.expect(state.close_connection(1U), "current connection closes");
    const auto* stale_book = state.find_book(7U);
    test.expect(stale_book != nullptr && stale_book->state() == eme::book::BookState::stale,
                "disconnect invalidates every existing book");
    test.expect(state.valid_book_count() == 0U &&
                    !stale_book->best_bid().has_value(),
                "stale book exposes no actionable quote");
    test.expect(state_error(state.apply(delta(7U, 1U, 2U, 12U, 1)),
                            eme::market::MarketStateError::connection_mismatch),
                "updates cannot apply while disconnected");
}

void test_generation_recovery(eme::test::Context& test) {
    eme::market::MarketState state;
    test.expect(state.open_connection(4U), "initial generation opens");
    test.expect(book_result(state.apply(snapshot(7U, 4U, 10U, 20U)),
                            eme::book::BookUpdateResult::applied),
                "initial snapshot applies");
    test.expect(book_result(state.apply(snapshot(8U, 4U, 11U, 30U)),
                            eme::book::BookUpdateResult::applied),
                "second market snapshot applies");

    test.expect(state.open_connection(5U), "newer connection generation opens");
    test.expect(state.valid_book_count() == 0U,
                "generation change invalidates all books atomically");
    test.expect(book_result(state.apply(snapshot(7U, 5U, 12U, 1U)),
                            eme::book::BookUpdateResult::recovery_not_started),
                "stale book rejects snapshot before recovery is requested");
    test.expect(state.begin_recovery(7U), "recovery transition is explicit");
    const auto* recovering = state.find_book(7U);
    test.expect(recovering != nullptr &&
                    recovering->state() == eme::book::BookState::recovering,
                "book enters recovering state");
    test.expect(book_result(state.apply(delta(7U, 5U, 12U, 2U, 10)),
                            eme::book::BookUpdateResult::requires_snapshot),
                "recovering book rejects deltas");
    test.expect(book_result(state.apply(snapshot(7U, 5U, 12U, 1U)),
                            eme::book::BookUpdateResult::applied),
                "validated current-generation snapshot completes recovery");
    test.expect(state.valid_book_count() == 1U,
                "only recovered books return to valid state");

    test.expect(state_error(state.apply(delta(7U, 4U, 10U, 21U, 10)),
                            eme::market::MarketStateError::connection_mismatch),
                "late update from prior generation is rejected");
    const auto* live_book = state.find_book(7U);
    test.expect(live_book != nullptr && live_book->state() == eme::book::BookState::valid &&
                    live_book->last_sequence() == 1U,
                "late old-generation update cannot corrupt recovered state");
    test.expect(!state.begin_recovery(99U), "unknown market cannot enter recovery");
}

void test_fail_closed_faults(eme::test::Context& test) {
    eme::market::MarketState state;
    test.expect(state.open_connection(9U), "connection opens for fault test");
    test.expect(book_result(state.apply(delta(77U, 9U, 1U, 1U, 10)),
                            eme::book::BookUpdateResult::requires_snapshot),
                "delta cannot create an unknown book");
    test.expect(state.book_count() == 0U,
                "rejected unknown delta does not allocate market state");
    test.expect(book_result(state.apply(snapshot(77U, 9U, 1U, 100U)),
                            eme::book::BookUpdateResult::applied),
                "fault-test snapshot applies");
    test.expect(book_result(state.apply(delta(77U, 9U, 1U, 102U, 10)),
                            eme::book::BookUpdateResult::sequence_gap),
                "dropped delta is detected as a gap");
    const auto* stale = state.find_book(77U);
    test.expect(stale != nullptr && stale->state() == eme::book::BookState::stale,
                "gap makes the affected book stale");
    test.expect(state.begin_recovery(77U), "gap recovery can be requested");
    test.expect(book_result(state.apply(snapshot(77U, 9U, 1U, 200U)),
                            eme::book::BookUpdateResult::applied),
                "fresh snapshot restores state after a dropped delta");

    const eme::market::NormalizedMarketEvent event{delta(77U, 9U, 1U, 201U, 5)};
    test.expect(book_result(state.apply(event), eme::book::BookUpdateResult::applied),
                "variant event follows the same deterministic apply path");
    test.expect(book_result(state.apply(event), eme::book::BookUpdateResult::sequence_gap),
                "duplicated delta is rejected rather than applied twice");
    test.expect(state.begin_recovery(77U),
                "book can recover after duplicated input invalidates continuity");
    test.expect(book_result(state.apply(snapshot(77U, 9U, 1U, 300U)),
                            eme::book::BookUpdateResult::applied),
                "snapshot restores state after duplicate detection");
    test.expect(book_result(state.apply(delta(77U, 9U, 1U, 299U, 5)),
                            eme::book::BookUpdateResult::sequence_gap),
                "reordered older delta is rejected");
    test.expect(state.begin_recovery(77U) &&
                    book_result(state.apply(snapshot(77U, 9U, 2U, 400U)),
                                eme::book::BookUpdateResult::applied),
                "new stream snapshot recovers after reordered input");
    test.expect(book_result(state.apply(snapshot(77U, 9U, 2U, 350U)),
                            eme::book::BookUpdateResult::stale_snapshot),
                "late snapshot cannot roll a valid market backward");
    const auto* preserved = state.find_book(77U);
    test.expect(preserved != nullptr && preserved->state() == eme::book::BookState::valid &&
                    preserved->last_sequence() == 400U,
                "rejected snapshot preserves the newer actionable state");
    test.expect(book_result(state.apply(snapshot(77U, 9U, 2U, 500U)),
                            eme::book::BookUpdateResult::applied),
                "validated snapshot can refresh an already live subscription");
    const auto* refreshed = state.find_book(77U);
    test.expect(refreshed != nullptr && refreshed->last_sequence() == 500U,
                "live snapshot establishes its new sequence base");
}

}  // namespace

int main() {
    eme::test::Context test;
    test_connection_lifecycle(test);
    test_generation_recovery(test);
    test_fail_closed_faults(test);

    {
        eme::market::MarketState shared{eme::market::SequenceScope::shared_stream};
        test.expect(shared.open_connection(1U), "shared stream connection");
        test.expect(book_result(shared.apply(snapshot(7U, 1U, 2U, 10U)), eme::book::BookUpdateResult::applied), "shared first book");
        test.expect(book_result(shared.apply(snapshot(8U, 1U, 2U, 11U)), eme::book::BookUpdateResult::applied), "shared second book");
        std::int64_t expected_a = 0, expected_b = 0;
        for (std::uint64_t sequence = 12U; sequence < 212U; ++sequence) {
            const auto id = sequence % 3U == 0U ? 7U : 8U;
            (id == 7U ? expected_a : expected_b) += 5;
            test.expect(book_result(shared.apply(delta(id, 1U, 2U, sequence, 5)), eme::book::BookUpdateResult::applied), "generated interleaved sequence");
        }
        test.expect(shared.find_book(7U)->quantity_at(eme::book::Side::bid, eme::test::price(4500)).raw() == expected_a &&
                    shared.find_book(8U)->quantity_at(eme::book::Side::bid, eme::test::price(4500)).raw() == expected_b,
                    "independent per-market quantity oracle");
        test.expect(book_result(shared.apply(snapshot(9U, 1U, 2U, 213U)), eme::book::BookUpdateResult::sequence_gap), "snapshot cannot hide missing shared message");
        test.expect(!shared.connected() && shared.valid_book_count() == 0U, "shared gap closes and invalidates all books");
        test.expect(shared.open_connection(2U) && shared.begin_recovery(7U), "shared stream recovery");
        test.expect(book_result(shared.apply(snapshot(7U, 2U, 2U, 1U)), eme::book::BookUpdateResult::applied), "reused stream sequence resets in new generation");
        test.expect(book_result(shared.apply(delta(8U, 2U, 2U, 2U, 5)), eme::book::BookUpdateResult::requires_snapshot), "delta cannot revive a stale book");
        test.expect(shared.valid_book_count() == 0U, "failed shared recovery invalidates earlier recovered book");
    }
    return test.result();
}
