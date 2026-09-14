#include "eme/opportunity/candidate_tracker.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace op = eme::opportunity;
namespace c = eme::constraint;
namespace m = eme::market;
using eme::test::level;

void snapshot(m::MarketState& state, const m::MarketId id, const std::uint64_t seq,
              const std::int64_t bid, const std::int64_t ask, const std::int64_t quantity) {
    const auto result = state.apply(m::BookSnapshot{id, *state.connection_generation(), 1U, seq, {},
        {level(bid, quantity)}, {level(ask, quantity)}});
    if (result != m::MarketApplyResult{eme::book::BookUpdateResult::applied}) {
        throw std::runtime_error{"fixture snapshot rejected"};
    }
}

c::ConstraintDefinition complement(const std::uint32_t id, const m::MarketId left,
                                   const m::MarketId right) {
    return {id, 1U, "pair-" + std::to_string(id), "synthetic reviewed rules", c::Complement{left, right}};
}

void add(c::ConstraintRegistry& registry, c::ConstraintDefinition definition) {
    if (registry.add(std::move(definition)) != c::ConstraintRegistrationResult::registered) {
        throw std::runtime_error{"fixture definition rejected"};
    }
}

void lifecycle(eme::test::Context& test) {
    c::ConstraintRegistry registry;
    add(registry, {7U, 3U, "a-implies-b", "synthetic", c::Implication{2U, 1U}});
    op::CandidateTracker tracker{11U, registry};
    m::MarketState state;
    test.expect(tracker.refresh_all(state).empty(), "disconnected state cannot open a candidate");
    (void)state.open_connection(1U);
    snapshot(state, 1U, 1U, 5500, 6000, 400);
    test.expect(tracker.refresh(1U, state).empty(), "missing dependent book cannot open a candidate");
    snapshot(state, 2U, 1U, 7000, 7500, 250);
    auto events = tracker.refresh(2U, state);
    test.expect(events.size() == 1U && tracker.active_count() == 1U, "candidate opens once both books exist");
    if (events.empty()) { return; }
    const auto first = events.front();
    test.expect(first.kind == op::CandidateEventKind::opened && first.quote &&
        first.quote->quantity.raw() == 250 && first.quote->acquisition_cost.raw() == 2'250'000 &&
        first.quote->minimum_payout.raw() == 2'500'000 && first.quote->gross_margin.raw() == 250'000,
        "BUY YES(B) at .60 plus NO(A) at .30, 2.50 contracts: cost 2.25, floor 2.50");
    test.expect(first.id.metadata_version == 11U && first.id.semantic_version == 3U &&
        first.id.legs[0U] == c::PayoffLegTemplate{1U, c::ContractOutcome::yes} &&
        first.id.legs[1U] == c::PayoffLegTemplate{2U, c::ContractOutcome::no}, "identity includes canonical directed legs");
    snapshot(state, 2U, 2U, 7000, 7600, 250);
    test.expect(tracker.refresh(2U, state).empty(), "irrelevant ask change does not change BUY NO quote");
    snapshot(state, 1U, 2U, 5000, 5500, 200);
    events = tracker.refresh(1U, state);
    test.expect(events.size() == 1U && events[0U].kind == op::CandidateEventKind::updated &&
        events[0U].id == first.id && events[0U].quote->gross_margin.raw() == 300'000,
        "changed quote updates the same identity, with new bottleneck quantity");
    snapshot(state, 1U, 3U, 6500, 7000, 200);
    events = tracker.refresh(1U, state);
    test.expect(events.size() == 1U && events[0U].kind == op::CandidateEventKind::invalidated &&
        events[0U].reason == op::InvalidationReason::non_positive_margin && !events[0U].quote,
        "zero gross margin invalidates, without retaining a usable quote");
    test.expect(tracker.refresh(1U, state).empty() && tracker.active_count() == 0U,
        "unchanged invalid candidates do not repeatedly emit invalidations");
    snapshot(state, 1U, 4U, 5000, 5500, 200);
    events = tracker.refresh(1U, state);
    test.expect(events.size() == 1U && events[0U].id == first.id &&
        events[0U].kind == op::CandidateEventKind::opened, "reappearance retains identity");
    (void)state.apply(m::BookDelta{1U, 1U, 1U, 99U, {}, eme::book::Side::ask,
        eme::test::price(5500), eme::test::delta(1)});
    events = tracker.refresh(1U, state);
    test.expect(events.size() == 1U && events[0U].reason == op::InvalidationReason::book_unavailable,
        "a rejected sequence gap still invalidates dependent candidates");
    (void)state.begin_recovery(1U);
    test.expect(tracker.refresh(1U, state).empty(), "recovery alone cannot reopen");
    snapshot(state, 1U, 100U, 5000, 5500, 200);
    test.expect(tracker.refresh(1U, state).size() == 1U, "recovery snapshot can reopen");
    (void)state.apply(m::BookSnapshot{1U, 1U, 1U, 101U, {}, {level(5000, 200)}, {}});
    events = tracker.refresh(1U, state);
    test.expect(events.size() == 1U && events[0U].reason == op::InvalidationReason::missing_quote,
        "empty required side invalidates an otherwise valid book");
}

void identities_and_global_changes(eme::test::Context& test) {
    c::ConstraintRegistry forward, reverse;
    add(forward, complement(9U, 1U, 2U)); add(forward, complement(3U, 3U, 4U));
    add(reverse, complement(3U, 4U, 3U)); add(reverse, complement(9U, 2U, 1U));
    op::CandidateTracker a{1U, forward}, b{1U, reverse};
    m::MarketState state;
    (void)state.open_connection(1U);
    for (m::MarketId id = 1U; id <= 4U; ++id) { snapshot(state, id, 1U, 3000, 4000, 125); }
    auto left = a.refresh_all(state); auto right = b.refresh_all(state);
    test.expect(std::equal(left.begin(), left.end(), right.begin(), right.end()) &&
        left.size() == 2U && left[0U].id.constraint_id == 3U && left[1U].id.constraint_id == 9U,
        "registration order and symmetric complement leg order do not change ordered events");
    const auto identity = left[0U].id;
    op::CandidateTracker other_version{2U, forward};
    test.expect(other_version.refresh_all(state)[0U].id != identity, "metadata version changes identity");
    c::ConstraintRegistry semantic;
    auto changed = complement(3U, 3U, 4U); changed.semantic_version = 2U; add(semantic, changed);
    op::CandidateTracker revised{1U, semantic};
    test.expect(revised.refresh_all(state)[0U].id != identity, "semantic version changes identity");
    bool rejected = false;
    try { op::CandidateTracker invalid{0U, forward}; } catch (const std::invalid_argument&) { rejected = true; }
    test.expect(rejected, "zero metadata version rejected at construction");
    // The tracker owns its projection; later registry registration does not leak in.
    add(forward, complement(20U, 1U, 4U));
    test.expect(a.definition_count() == 2U, "registry mutation cannot silently alter an existing tracker");
    (void)state.close_connection(1U);
    left = a.refresh(1U, state);
    test.expect(left.size() == 2U && a.active_count() == 0U &&
        left[0U].reason == op::InvalidationReason::disconnected &&
        left[1U].reason == op::InvalidationReason::disconnected,
        "connection change invalidates even candidates unrelated to the refresh market");
    (void)state.open_connection(2U);
    test.expect(a.refresh(1U, state).empty(), "new generation cannot use stale prior books");
    for (m::MarketId id = 1U; id <= 4U; ++id) {
        (void)state.begin_recovery(id); snapshot(state, id, 1U, 3000, 4000, 125);
    }
    (void)a.refresh_all(state);
    (void)state.open_connection(3U); // No preceding close is required to invalidate.
    test.expect(a.refresh(99U, state).size() == 2U && a.active_count() == 0U,
        "generation change triggers a global invalidation, including an unrelated refresh key");
}

void arithmetic_oracle(eme::test::Context& test) {
    c::ConstraintRegistry registry; add(registry, complement(1U, 1U, 2U));
    for (const std::int64_t left : {0, 1, 4999, 5000, 9999, 10000}) {
        for (const std::int64_t right : {0, 1, 4999, 5000, 9999, 10000}) {
            for (const std::int64_t units : {1, 99, 100, 101, 125}) {
                m::MarketState state; (void)state.open_connection(1U);
                snapshot(state, 1U, 1U, 0, left, units);
                snapshot(state, 2U, 1U, 0, right, units + 10);
                op::CandidateTracker tracker{1U, registry};
                const auto events = tracker.refresh_all(state);
                // Independent cash accounting per leg and exhaustive binary settlement.
                const auto cost = left * units + right * units;
                auto floor = std::numeric_limits<std::int64_t>::max();
                for (const bool settles_left : {false, true}) {
                    const auto left_cash = settles_left ? units * 10000 : 0;
                    const auto right_cash = !settles_left ? units * 10000 : 0;
                    floor = std::min(floor, left_cash + right_cash);
                }
                test.expect(events.size() == (floor > cost ? 1U : 0U), "strictly positive gross margin required");
                if (!events.empty()) {
                    const auto& quote = *events[0U].quote;
                    test.expect(quote.acquisition_cost.raw() == cost && quote.minimum_payout.raw() == floor &&
                        quote.gross_margin.raw() == floor - cost && quote.quantity.raw() == units,
                        "fractional quantity, endpoint prices and one-microdollar results match cash oracle");
                }
            }
        }
    }
    m::MarketState state; (void)state.open_connection(1U);
    snapshot(state, 1U, 1U, 0, 4000, 100); snapshot(state, 2U, 1U, 0, 5000, 100);
    op::CandidateTracker tracker{1U, registry}; (void)tracker.refresh_all(state);
    snapshot(state, 1U, 2U, 0, 4000, std::numeric_limits<std::int64_t>::max());
    snapshot(state, 2U, 2U, 0, 5000, std::numeric_limits<std::int64_t>::max());
    const auto events = tracker.refresh_all(state);
    test.expect(events.size() == 1U && events[0U].reason == op::InvalidationReason::arithmetic_overflow,
        "unrepresentable payout invalidates rather than wrapping or clipping sizing silently");
}

void differential(eme::test::Context& test) {
    c::ConstraintRegistry registry;
    for (std::uint32_t id = 1U; id <= 24U; ++id) {
        const auto left = id % 8U + 1U;
        const auto right = (id + 3U) % 8U + 1U;
        auto definition = complement(id, left, right);
        if (id % 2U == 0U) { definition.relationship = c::Implication{left, right}; }
        add(registry, definition);
    }
    op::CandidateTracker incremental{1U, registry}, complete{1U, registry};
    m::MarketState state; (void)state.open_connection(1U);
    std::uint32_t random = 0x41C64E6DU;
    const auto next = [&random] { random = random * 1664525U + 1013904223U; return random; };
    const auto compare = [&](const m::MarketId changed) {
        const auto actual = incremental.refresh(changed, state);
        const auto expected = complete.refresh_all(state);
        test.expect(std::equal(actual.begin(), actual.end(), expected.begin(), expected.end()) &&
            incremental.active_count() == complete.active_count(), "incremental events match ordered full scan");
    };
    for (std::uint64_t step = 1U; step <= 2000U; ++step) {
        const auto id = next() % 8U + 1U;
        if (step % 100U == 0U) {
            const auto generation = *state.connection_generation();
            (void)state.close_connection(generation); compare(id);
            (void)state.open_connection(generation + 1U); compare(id);
        }
        const auto* book = state.find_book(id);
        if (book && book->state() == eme::book::BookState::stale) {
            (void)state.begin_recovery(id); compare(id);
        }
        const auto bid = static_cast<std::int64_t>(next() % 8001U);
        const auto ask = bid + static_cast<std::int64_t>(next() % static_cast<std::uint32_t>(10001 - bid));
        snapshot(state, id, step + 1U, bid, ask, static_cast<std::int64_t>(next() % 1000U + 1U));
        compare(id);
        if (step % 37U == 0U) {
            (void)state.apply(m::BookDelta{id, *state.connection_generation(), 1U, step + 9U, {},
                eme::book::Side::ask, eme::test::price(ask), eme::test::delta(1)});
            compare(id);
        }
    }
}
}  // namespace

int main() {
    eme::test::Context test;
    lifecycle(test); identities_and_global_changes(test); arithmetic_oracle(test); differential(test);
    return test.result();
}
