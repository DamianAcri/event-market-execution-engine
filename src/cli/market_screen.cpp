#include "cli/market_screen.hpp"

#include "eme/core/net_sizing.hpp"
#include "session/study_json.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <unordered_map>

namespace eme::cli {
namespace {
namespace detail = session::detail;
using Json = detail::Json;
using MarketId = market::MarketId;
constexpr std::size_t maximum_screen_bytes = 32U * 1024U * 1024U;
constexpr std::uint64_t maximum_solver_evaluations = 10'000'000U;

struct SnapshotBook final {
    std::uint64_t request_time_ms{};
    std::uint64_t received_time_ms{};
    std::array<std::vector<core::BuyLevel>, 2U> buys;
};

MarketId market_id(const Json& value, const gateway::kalshi::MetadataSnapshot& metadata) {
    const auto id = static_cast<MarketId>(detail::integer(value, "market_id", std::numeric_limits<MarketId>::max()));
    if (!metadata.markets().find(id)) { detail::invalid("unknown market_id"); }
    return id;
}

std::vector<core::BuyLevel> parse_bids(const Json& levels) {
    if (!levels.is_array() || levels.size() > 10'001U) { detail::invalid("bid levels"); }
    std::vector<core::BuyLevel> buys;
    buys.reserve(levels.size());
    std::int64_t previous = -1;
    for (const auto& level : levels) {
        if (!level.is_array() || level.size() != 2U || !level[0].is_number_unsigned() ||
            !level[1].is_number_unsigned() || level[0].get<std::uint64_t>() > 10'000U ||
            level[1].get<std::uint64_t>() == 0U ||
            level[1].get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            detail::invalid("bid price/quantity");
        }
        const auto price = level[0].get<std::int64_t>();
        if (price <= previous) { detail::invalid("bids must be ascending and unique"); }
        previous = price;
        buys.push_back({*core::Price::from_raw(10'000 - price), *core::Quantity::from_raw(level[1].get<std::int64_t>())});
    }
    std::reverse(buys.begin(), buys.end());
    return buys;
}

std::string_view status_name(const core::SizingStatus status) noexcept {
    switch (status) {
    case core::SizingStatus::optimal: return "optimal";
    case core::SizingStatus::no_positive_margin: return "no_positive_margin";
    case core::SizingStatus::no_depth: return "no_depth";
    case core::SizingStatus::insufficient_cash: return "insufficient_cash";
    case core::SizingStatus::search_budget_exceeded: return "search_budget_exceeded";
    case core::SizingStatus::invalid_input: return "invalid_input";
    case core::SizingStatus::arithmetic_error: return "arithmetic_error";
    }
    return "invalid_input";
}

// A fixed one-contract diagnostic retains negative values for research ranking.
// Unlike the optimizer this is not a best-size quote. Depth and funding are still
// priced by the production fee/reservation primitives, with no fill inference.
Json one_contract_diagnostic(const std::array<core::BuyDepth, 2U>& depth, const core::SizingLimits& limits) {
    if (limits.cap.raw() < 100) { return nullptr; }
    std::int64_t notional = 0;
    std::int64_t debit = 0;
    std::int64_t reservation = 0;
    for (const auto& leg : depth) {
        std::int64_t remaining = 100;
        core::FeeAccumulator accumulator;
        auto last = *core::Price::from_raw(0);
        for (const auto& level : leg.levels) {
            const auto amount = std::min(remaining, level.quantity.raw() / limits.step.raw() * limits.step.raw());
            if (amount == 0) { continue; }
            const auto charge = core::charge_buy_fill(*core::Quantity::from_raw(amount), level.price, leg.fees, accumulator);
            if (!charge) { return nullptr; }
            notional += charge->notional.raw();
            debit += charge->debit.raw();
            last = level.price;
            remaining -= amount;
            if (remaining == 0) { break; }
        }
        if (remaining != 0) { return nullptr; }
        const auto reserve = core::buy_reservation(*core::Quantity::from_raw(100), limits.step, last, leg.fees);
        if (!reserve) { return nullptr; }
        reservation += reserve->raw();
    }
    return {{"quantity_centicontracts", 100}, {"gross_margin_micro", 1'000'000 - notional},
        {"net_margin_micro", 1'000'000 - debit}, {"debit_micro", debit},
        {"reservation_micro", reservation}, {"funded", reservation <= limits.available.raw()}};
}

Json screen(const gateway::kalshi::MetadataSnapshot& metadata, const Json& input) {
    detail::shape(input, {"schema_version", "as_of_ms", "max_age_ms", "max_skew_ms", "max_total_evaluations", "sizing", "fees", "books"});
    if (detail::integer(input, "schema_version") != 1U) { detail::invalid("schema_version"); }
    const auto as_of = detail::integer(input, "as_of_ms");
    const auto max_age = detail::integer(input, "max_age_ms");
    const auto max_skew = detail::integer(input, "max_skew_ms");
    const auto total_budget = detail::integer(input, "max_total_evaluations", maximum_solver_evaluations);
    const auto& sizing = input.at("sizing");
    detail::shape(sizing, {"cap_centicontracts", "step_centicontracts", "available_cash_micro", "minimum_margin_micro", "max_evaluations"});
    core::SizingLimits limits{
        *core::Quantity::from_raw(static_cast<std::int64_t>(detail::integer(sizing, "cap_centicontracts", 100'000'000U))),
        *core::Quantity::from_raw(static_cast<std::int64_t>(detail::integer(sizing, "step_centicontracts", 100U))),
        *core::Cash::from_raw(static_cast<std::int64_t>(detail::integer(sizing, "available_cash_micro", 1'000'000'000'000'000U))),
        *core::Cash::from_raw(static_cast<std::int64_t>(detail::integer(sizing, "minimum_margin_micro", 1'000'000'000'000'000U))),
        detail::integer(sizing, "max_evaluations", maximum_solver_evaluations)};
    if ((limits.step.raw() != 1 && limits.step.raw() != 100) || limits.cap.raw() < limits.step.raw() ||
        limits.max_evaluations == 0U || total_budget == 0U) { detail::invalid("sizing limits"); }

    const auto& fee_rows = input.at("fees");
    const auto& book_rows = input.at("books");
    if (!fee_rows.is_array() || !book_rows.is_array() || fee_rows.size() > metadata.markets().size() ||
        book_rows.size() > metadata.markets().size()) { detail::invalid("fees/books arrays"); }
    std::unordered_map<MarketId, core::FeePolicy> fees;
    fees.reserve(fee_rows.size());
    for (const auto& row : fee_rows) {
        detail::shape(row, {"market_id", "coefficient_ppm", "balance_quantum_micro"});
        const auto id = market_id(row, metadata);
        const core::FeePolicy fee{static_cast<std::uint32_t>(detail::integer(row, "coefficient_ppm", 1'000'000U)),
            static_cast<std::uint32_t>(detail::integer(row, "balance_quantum_micro", 10'000U))};
        if ((fee.balance_quantum_micro != 100U && fee.balance_quantum_micro != 10'000U) || !fees.emplace(id, fee).second) {
            detail::invalid("fee policy");
        }
    }
    std::unordered_map<MarketId, SnapshotBook> books;
    books.reserve(book_rows.size());
    for (const auto& row : book_rows) {
        detail::shape(row, {"market_id", "request_time_ms", "received_time_ms", "yes_bids", "no_bids"});
        const auto id = market_id(row, metadata);
        SnapshotBook book{detail::integer(row, "request_time_ms"), detail::integer(row, "received_time_ms"), {}};
        if (book.request_time_ms > book.received_time_ms || book.received_time_ms > as_of) { detail::invalid("book time bounds"); }
        // Buying an outcome matches the opposite outcome's bids.
        book.buys[0U] = parse_bids(row.at("no_bids"));
        book.buys[1U] = parse_bids(row.at("yes_bids"));
        if (!books.emplace(id, std::move(book)).second) { detail::invalid("duplicate book"); }
    }

    Json output{{"schema_version", 1}, {"kind", "indicative_rest_screen"}, {"as_of_ms", as_of},
        {"synchronous_books", false}, {"simulated_fills", false}, {"joint_capital_allocation", false},
        {"time_basis", "client_request_response_intervals"}, {"exchange_timestamp_verified", false},
        {"metadata_version", metadata.markets().metadata_version()}, {"market_count", metadata.markets().size()},
        {"book_count", books.size()}, {"constraints", Json::array()}};
    std::uint64_t evaluated = 0U;
    std::uint64_t positive = 0U;
    bool complete = true;
    Json counts = Json::object();
    for (const auto id : metadata.constraints().sorted_ids()) {
        const auto& compiled = *metadata.constraints().find(id);
        if (compiled.guaranteed_leg_templates.size() != 2U) { detail::invalid("unsupported constraint legs"); }
        std::array<constraint::PayoffLegTemplate, 2U> legs{compiled.guaranteed_leg_templates[0U], compiled.guaranteed_leg_templates[1U]};
        std::sort(legs.begin(), legs.end(), [](const auto& left, const auto& right) { return left.market_id < right.market_id; });
        Json row{{"constraint_id", id}, {"status", "missing_book"}, {"evaluated_quantities", 0U},
            {"market_ids", {legs[0U].market_id, legs[1U].market_id}}, {"quote", nullptr},
            {"one_contract_diagnostic", nullptr}};
        const auto left = books.find(legs[0U].market_id);
        const auto right = books.find(legs[1U].market_id);
        if (left != books.end() && right != books.end()) {
            const auto oldest = std::min(left->second.request_time_ms, right->second.request_time_ms);
            const auto newest = std::max(left->second.received_time_ms, right->second.received_time_ms);
            row["maximum_age_ms"] = as_of - oldest;
            row["capture_span_ms"] = newest - oldest;
            if (as_of - oldest > max_age) { row["status"] = "stale_book"; }
            else if (newest - oldest > max_skew) { row["status"] = "nonsynchronous_books"; }
            else if (!fees.contains(legs[0U].market_id) || !fees.contains(legs[1U].market_id)) { row["status"] = "missing_fee"; }
            else if (evaluated == total_budget) { row["status"] = "screening_budget_exceeded"; complete = false; }
            else {
                std::array<core::BuyDepth, 2U> depth;
                for (std::size_t index = 0U; index < 2U; ++index) {
                    const auto& leg = legs[index];
                    const auto outcome = leg.outcome == constraint::ContractOutcome::yes ? 0U : 1U;
                    depth[index] = {books.at(leg.market_id).buys[outcome], fees.at(leg.market_id)};
                }
                row["one_contract_diagnostic"] = one_contract_diagnostic(depth, limits);
                auto bounded = limits;
                bounded.max_evaluations = std::min(limits.max_evaluations, total_budget - evaluated);
                const auto result = core::size_buy_pair(depth, bounded);
                evaluated += result.evaluated_quantities;
                row["status"] = status_name(result.status);
                row["evaluated_quantities"] = result.evaluated_quantities;
                row["intervals_pruned"] = result.intervals_pruned;
                if (result.status == core::SizingStatus::search_budget_exceeded || result.status == core::SizingStatus::invalid_input ||
                    result.status == core::SizingStatus::arithmetic_error) { complete = false; }
                if (result.quote) {
                    ++positive;
                    const auto& quote = *result.quote;
                    Json quoted_legs = Json::array();
                    std::int64_t debit = 0;
                    std::int64_t reserve = 0;
                    for (std::size_t index = 0U; index < 2U; ++index) {
                        const auto& part = quote.legs[index];
                        debit += part.debit.raw();
                        reserve += part.reservation.raw();
                        quoted_legs.push_back({{"market_id", legs[index].market_id},
                            {"outcome", legs[index].outcome == constraint::ContractOutcome::yes ? "yes" : "no"},
                            {"limit_price_1e4", part.limit.raw()}, {"notional_micro", part.notional.raw()},
                            {"debit_micro", part.debit.raw()}, {"fees_and_rounding_micro", part.debit.raw() - part.notional.raw()},
                            {"reservation_micro", part.reservation.raw()}});
                    }
                    row["quote"] = {{"quantity_centicontracts", quote.quantity.raw()}, {"net_margin_micro", quote.net_margin.raw()},
                        {"payout_floor_micro", quote.payout_floor.raw()}, {"debit_micro", debit}, {"reservation_micro", reserve},
                        {"legs", std::move(quoted_legs)}};
                }
            }
        }
        const auto status = row.at("status").get<std::string>();
        counts[status] = counts.value(status, std::uint64_t{0U}) + 1U;
        output["constraints"].push_back(std::move(row));
    }
    output["solver_complete"] = complete;
    output["positive_quotes"] = positive;
    output["evaluated_quantities"] = evaluated;
    output["status_counts"] = std::move(counts);
    return output;
}
}  // namespace

MarketScreenResult screen_markets(const gateway::kalshi::MetadataSnapshot& metadata, const std::string_view input) {
    try {
        if (input.size() > maximum_screen_bytes) { return MarketScreenError{"input too large"}; }
        return screen(metadata, detail::parse_strict(input)).dump();
    } catch (const session::ReplayError& error) { return MarketScreenError{error.reason}; }
    catch (const Json::exception&) { return MarketScreenError{"invalid JSON"}; }
}

int run_market_screen_command(const int argc, const char* const argv[]) {
    if (argc != 5 || std::string_view{argv[2]} != "screen") {
        std::cerr << "Usage: event-engine market screen <metadata.json> <screen.json>\n";
        return 2;
    }
    try {
        const auto parsed = gateway::kalshi::parse_metadata_snapshot(detail::read_text(argv[3]));
        const auto* metadata = std::get_if<gateway::kalshi::MetadataSnapshot>(&parsed);
        if (metadata == nullptr) { std::cerr << "Market screen metadata invalid\n"; return 1; }
        const auto result = screen_markets(*metadata, detail::read_text(argv[4], maximum_screen_bytes));
        if (const auto* error = std::get_if<MarketScreenError>(&result)) {
            std::cerr << "Market screen input invalid: " << error->field << '\n';
            return 1;
        }
        std::cout << std::get<std::string>(result) << '\n';
        return std::cout ? 0 : 1;
    } catch (const session::ReplayError& error) {
        std::cerr << "Market screen read failed: " << error.reason << '\n';
        return 1;
    }
}
}  // namespace eme::cli
