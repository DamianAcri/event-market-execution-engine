#include "cli/basket_screen.hpp"

#include "eme/core/net_sizing.hpp"
#include "session/study_json.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace eme::cli {
namespace {
namespace detail = session::detail;
using Json = detail::Json;
using MarketId = std::uint32_t;
constexpr std::size_t maximum_screen_bytes = 32U * 1024U * 1024U;
constexpr std::uint64_t maximum_evaluations = 10'000'000U;
constexpr std::int64_t maximum_quantity = 10'000;
constexpr std::int64_t quantity_step = 100;

struct SnapshotBook final {
    std::uint64_t requested{}, received{};
    std::array<std::vector<core::BuyLevel>, 2U> buys;
    std::array<std::int64_t, 2U> available{};
};
struct Basket final {
    std::uint32_t id{};
    std::string key;
    std::array<MarketId, 3U> markets{};
};
struct Evaluation final {
    std::int64_t quantity{}, notional{}, debit{}, reserve{}, margin{};
    std::array<core::SizedLeg, 3U> legs;
};

// Completed price levels are charged once. The last (possibly fractional)
// level is recomputed from that prefix for each whole-contract candidate, so
// expanding q does not incorrectly split one assumed fill into several fills.
struct CostCursor final {
    core::BuyDepth depth;
    std::size_t index{};
    std::int64_t prefix_quantity{}, prefix_notional{}, prefix_debit{};
    core::FeeAccumulator accumulator;

    std::optional<core::SizedLeg> at(const std::int64_t quantity) {
        while (index < depth.levels.size() &&
               depth.levels[index].quantity.raw() < quantity - prefix_quantity) {
            const auto& level = depth.levels[index];
            const auto charge = core::charge_buy_fill(level.quantity, level.price, depth.fees, accumulator);
            if (!charge) { return std::nullopt; }
            prefix_quantity += level.quantity.raw();
            prefix_notional += charge->notional.raw();
            prefix_debit += charge->debit.raw();
            ++index;
        }
        if (index == depth.levels.size()) { return std::nullopt; }
        const auto& level = depth.levels[index];
        auto partial_accumulator = accumulator;
        const auto charge = core::charge_buy_fill(*core::Quantity::from_raw(quantity - prefix_quantity),
            level.price, depth.fees, partial_accumulator);
        // Order quantities are whole contracts; possible executions may be
        // centicontract fragments. Reserve conservatively for that finer grid.
        const auto reserve = core::buy_reservation(*core::Quantity::from_raw(quantity),
            *core::Quantity::from_raw(1), level.price, depth.fees);
        if (!charge || !reserve) { return std::nullopt; }
        return core::SizedLeg{level.price, *core::Cash::from_raw(prefix_notional + charge->notional.raw()),
            *core::Cash::from_raw(prefix_debit + charge->debit.raw()), *reserve};
    }
};

std::uint32_t positive_id(const Json& row, const std::string_view field) {
    const auto id = detail::integer(row, field, std::numeric_limits<std::uint32_t>::max());
    if (id == 0U) { detail::invalid(std::string{field}); }
    return static_cast<std::uint32_t>(id);
}

MarketId known_id(const Json& row, const std::string_view field,
                  const std::unordered_set<MarketId>& markets) {
    const auto id = positive_id(row, field);
    if (!markets.contains(id)) { detail::invalid("unknown market_id"); }
    return id;
}

std::vector<core::BuyLevel> parse_bids(const Json& rows, std::size_t& total_levels, std::int64_t& available) {
    if (!rows.is_array() || rows.size() > 10'001U || total_levels + rows.size() > 1'000'000U) {
        detail::invalid("bid levels");
    }
    total_levels += rows.size();
    std::vector<core::BuyLevel> result;
    result.reserve(rows.size());
    std::int64_t previous = -1;
    for (const auto& level : rows) {
        if (!level.is_array() || level.size() != 2U || !level[0].is_number_unsigned() ||
            !level[1].is_number_unsigned() || level[0].get<std::uint64_t>() > 10'000U ||
            level[1].get<std::uint64_t>() == 0U ||
            level[1].get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            detail::invalid("bid price/quantity");
        }
        const auto price = level[0].get<std::int64_t>();
        if (price <= previous) { detail::invalid("bids must be ascending and unique"); }
        previous = price;
        // Larger visible sizes cannot affect this bounded search. Saturating
        // the quantity also bounds all cursor arithmetic independently of JSON.
        const auto quantity = std::min(level[1].get<std::int64_t>(), maximum_quantity);
        available = std::min(maximum_quantity, available + quantity);
        result.push_back({*core::Price::from_raw(10'000 - price), *core::Quantity::from_raw(quantity)});
    }
    std::reverse(result.begin(), result.end());
    return result;
}

Json quote_json(const Evaluation& value, const Basket& basket) {
    Json legs = Json::array();
    for (std::size_t index = 0U; index < 3U; ++index) {
        const auto& leg = value.legs[index];
        legs.push_back({{"market_id", basket.markets[index]}, {"outcome", index == 0U ? "yes" : "no"},
            {"limit_price_1e4", leg.limit.raw()}, {"notional_micro", leg.notional.raw()},
            {"debit_micro", leg.debit.raw()}, {"fees_and_rounding_micro", leg.debit.raw() - leg.notional.raw()},
            {"reservation_micro", leg.reservation.raw()}});
    }
    return {{"quantity_centicontracts", value.quantity}, {"net_margin_micro", value.margin},
        {"payout_floor_micro", value.quantity * 20'000}, {"notional_micro", value.notional},
        {"debit_micro", value.debit}, {"reservation_micro", value.reserve}, {"legs", std::move(legs)}};
}

Json screen(const Json& input) {
    detail::shape(input, {"schema_version", "markets", "baskets", "as_of_ms", "max_age_ms", "max_skew_ms",
        "max_total_evaluations", "sizing", "fees", "books"});
    if (detail::integer(input, "schema_version") != 1U) { detail::invalid("schema_version"); }
    const auto as_of = detail::integer(input, "as_of_ms");
    const auto max_age = detail::integer(input, "max_age_ms");
    const auto max_skew = detail::integer(input, "max_skew_ms");
    const auto total_budget = detail::integer(input, "max_total_evaluations", maximum_evaluations);
    const auto& sizing = input.at("sizing");
    detail::shape(sizing, {"cap_centicontracts", "step_centicontracts", "available_cash_micro", "minimum_margin_micro", "max_evaluations"});
    const auto cap = static_cast<std::int64_t>(detail::integer(sizing, "cap_centicontracts", maximum_quantity));
    const auto step = detail::integer(sizing, "step_centicontracts");
    const auto cash = static_cast<std::int64_t>(detail::integer(sizing, "available_cash_micro", 1'000'000'000'000'000U));
    const auto margin_threshold = static_cast<std::int64_t>(detail::integer(sizing, "minimum_margin_micro", 1'000'000'000'000'000U));
    const auto row_budget = detail::integer(sizing, "max_evaluations", maximum_evaluations);
    if (cap < quantity_step || cap % quantity_step != 0 || step != quantity_step || row_budget == 0U || total_budget == 0U) {
        detail::invalid("sizing limits");
    }
    const auto& market_rows = input.at("markets");
    const auto& basket_rows = input.at("baskets");
    if (!market_rows.is_array() || market_rows.size() > 4096U || !basket_rows.is_array() || basket_rows.size() > 16'384U) {
        detail::invalid("markets/baskets arrays");
    }
    std::unordered_set<MarketId> markets;
    std::unordered_set<std::string> tickers;
    markets.reserve(market_rows.size());
    tickers.reserve(market_rows.size());
    for (const auto& row : market_rows) {
        detail::shape(row, {"id", "ticker"});
        if (!markets.insert(positive_id(row, "id")).second || !tickers.insert(detail::string(row, "ticker")).second) {
            detail::invalid("duplicate market");
        }
    }
    std::vector<Basket> baskets;
    baskets.reserve(basket_rows.size());
    std::unordered_set<std::uint32_t> basket_ids;
    std::unordered_set<std::string> basket_keys, triples;
    basket_ids.reserve(basket_rows.size());
    basket_keys.reserve(basket_rows.size());
    triples.reserve(basket_rows.size());
    // A threshold may appear as the upper threshold in one basket and the
    // lower threshold in another, but its supplied economic meaning must agree.
    std::unordered_map<MarketId, std::array<std::uint64_t, 3U>> semantics;
    semantics.reserve(markets.size());
    for (const auto& row : basket_rows) {
        detail::shape(row, {"id", "key", "lower_market_id", "upper_market_id", "range_market_id",
            "lower_threshold_cents", "upper_threshold_cents", "interval_lower_cents", "interval_upper_cents"});
        Basket basket{positive_id(row, "id"), detail::string(row, "key"),
            {known_id(row, "lower_market_id", markets), known_id(row, "upper_market_id", markets), known_id(row, "range_market_id", markets)}};
        const auto a = detail::integer(row, "lower_threshold_cents");
        const auto b = detail::integer(row, "upper_threshold_cents");
        const auto lower = detail::integer(row, "interval_lower_cents");
        const auto upper = detail::integer(row, "interval_upper_cents");
        if (!(a < lower && lower <= upper && upper <= b) || basket.markets[0] == basket.markets[1] ||
            basket.markets[0] == basket.markets[2] || basket.markets[1] == basket.markets[2]) {
            detail::invalid("basket interval/markets");
        }
        const auto triple = std::to_string(basket.markets[0]) + ":" + std::to_string(basket.markets[1]) + ":" + std::to_string(basket.markets[2]);
        if (!basket_ids.insert(basket.id).second || !basket_keys.insert(basket.key).second || !triples.insert(triple).second) {
            detail::invalid("duplicate basket");
        }
        const std::array<std::array<std::uint64_t, 3U>, 3U> meanings{{{0U, a, a}, {0U, b, b}, {1U, lower, upper}}};
        for (std::size_t index = 0; index < 3U; ++index) {
            const auto [existing, inserted] = semantics.emplace(basket.markets[index], meanings[index]);
            if (!inserted && existing->second != meanings[index]) { detail::invalid("inconsistent market semantics"); }
        }
        baskets.push_back(std::move(basket));
    }
    std::sort(baskets.begin(), baskets.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
    const auto& fee_rows = input.at("fees");
    const auto& book_rows = input.at("books");
    if (!fee_rows.is_array() || !book_rows.is_array() || fee_rows.size() > markets.size() || book_rows.size() > markets.size()) {
        detail::invalid("fees/books arrays");
    }
    std::unordered_map<MarketId, core::FeePolicy> fees;
    fees.reserve(fee_rows.size());
    for (const auto& row : fee_rows) {
        detail::shape(row, {"market_id", "coefficient_ppm", "balance_quantum_micro"});
        const auto id = known_id(row, "market_id", markets);
        const core::FeePolicy fee{static_cast<std::uint32_t>(detail::integer(row, "coefficient_ppm", 1'000'000U)),
            static_cast<std::uint32_t>(detail::integer(row, "balance_quantum_micro", 10'000U))};
        if ((fee.balance_quantum_micro != 100U && fee.balance_quantum_micro != 10'000U) || !fees.emplace(id, fee).second) {
            detail::invalid("fee policy");
        }
    }
    std::unordered_map<MarketId, SnapshotBook> books;
    books.reserve(book_rows.size());
    std::size_t total_levels = 0U;
    for (const auto& row : book_rows) {
        detail::shape(row, {"market_id", "request_time_ms", "received_time_ms", "yes_bids", "no_bids"});
        const auto id = known_id(row, "market_id", markets);
        SnapshotBook book{detail::integer(row, "request_time_ms"), detail::integer(row, "received_time_ms"), {}, {}};
        if (book.requested > book.received || book.received > as_of) { detail::invalid("book time bounds"); }
        book.buys[0] = parse_bids(row.at("no_bids"), total_levels, book.available[0]);
        book.buys[1] = parse_bids(row.at("yes_bids"), total_levels, book.available[1]);
        if (!book.buys[0].empty() && !book.buys[1].empty() && book.buys[0][0].price.raw() + book.buys[1][0].price.raw() < 10'000) {
            detail::invalid("crossed book");
        }
        if (!books.emplace(id, std::move(book)).second) { detail::invalid("duplicate book"); }
    }
    Json output{{"schema_version", 1}, {"kind", "conditional_payoff_screen"}, {"as_of_ms", as_of},
        {"synchronous_books", false}, {"simulated_fills", false}, {"joint_capital_allocation", false},
        {"production_certificate", false}, {"settlement_rules_verified_by_native_screen", false},
        {"payoff_model", "yes_lower_threshold_no_upper_threshold_no_inclusive_range"},
        {"conditional_payout_floor_micro_per_contract", 2'000'000},
        {"settlement_hypotheses", {"common_observation_strict_thresholds_inclusive_interval", "all_three_markets_settle_no"}},
        {"fill_model", "one_assumed_fill_per_consumed_price_level"}, {"funding_fill_step_centicontracts", 1},
        {"time_basis", "client_request_response_intervals"}, {"exchange_timestamp_verified", false},
        {"market_count", markets.size()}, {"book_count", books.size()}, {"baskets", Json::array()}};
    std::uint64_t evaluated = 0U, positive = 0U;
    bool complete = true;
    Json counts = Json::object();
    for (const auto& basket : baskets) {
        Json row{{"basket_id", basket.id}, {"key", basket.key}, {"market_ids", basket.markets},
            {"status", "missing_book"}, {"evaluated_quantities", 0U}, {"quote", nullptr}, {"one_contract_diagnostic", nullptr}};
        const bool present = std::all_of(basket.markets.begin(), basket.markets.end(), [&books](const auto id) { return books.contains(id); });
        if (present) {
            auto oldest = as_of;
            std::uint64_t newest = 0U;
            auto available = cap;
            bool all_fees = true;
            for (std::size_t index = 0U; index < 3U; ++index) {
                const auto id = basket.markets[index];
                const auto& book = books.at(id);
                oldest = std::min(oldest, book.requested);
                newest = std::max(newest, book.received);
                available = std::min(available, book.available[index == 0U ? 0U : 1U]);
                all_fees = all_fees && fees.contains(id);
            }
            row["maximum_age_ms"] = as_of - oldest;
            row["capture_span_ms"] = newest - oldest;
            if (as_of - oldest > max_age) { row["status"] = "stale_book"; }
            else if (newest - oldest > max_skew) { row["status"] = "nonsynchronous_books"; }
            else if (!all_fees) { row["status"] = "missing_fee"; }
            else if (available < quantity_step) { row["status"] = "no_depth"; }
            else if (evaluated == total_budget) { row["status"] = "screening_budget_exceeded"; complete = false; }
            else {
                std::array<CostCursor, 3U> cursors;
                for (std::size_t index = 0U; index < 3U; ++index) {
                    const auto id = basket.markets[index];
                    cursors[index].depth = {books.at(id).buys[index == 0U ? 0U : 1U], fees.at(id)};
                }
                std::optional<Evaluation> best;
                std::uint64_t row_evaluated = 0U;
                bool funded = false, failed = false;
                row["status"] = "no_positive_margin";
                for (std::int64_t q = quantity_step; q <= available; q += quantity_step) {
                    if (row_evaluated == row_budget || evaluated == total_budget) {
                        row["status"] = "search_budget_exceeded";
                        complete = false; failed = true; break;
                    }
                    ++row_evaluated;
                    ++evaluated;
                    Evaluation value;
                    value.quantity = q;
                    for (std::size_t index = 0U; index < 3U; ++index) {
                        const auto leg = cursors[index].at(q);
                        if (!leg) { failed = true; break; }
                        value.legs[index] = *leg;
                        value.notional += leg->notional.raw();
                        value.debit += leg->debit.raw();
                        value.reserve += leg->reservation.raw();
                    }
                    if (failed) { row["status"] = "arithmetic_error"; complete = false; break; }
                    value.margin = q * 20'000 - value.debit;
                    if (q == quantity_step) {
                        auto diagnostic = quote_json(value, basket);
                        diagnostic["gross_margin_micro"] = q * 20'000 - value.notional;
                        diagnostic["funded"] = value.reserve <= cash;
                        row["one_contract_diagnostic"] = std::move(diagnostic);
                    }
                    // Reserve is monotone in q and worst consumed price. An
                    // unfunded candidate proves all following sizes unfunded.
                    if (value.reserve > cash) { break; }
                    funded = true;
                    if (value.margin > margin_threshold && (!best || value.margin > best->margin ||
                        (value.margin == best->margin && (value.reserve < best->reserve ||
                            (value.reserve == best->reserve && q < best->quantity))))) {
                        best = value;
                    }
                }
                row["evaluated_quantities"] = row_evaluated;
                if (!failed && best) { row["status"] = "optimal"; row["quote"] = quote_json(*best, basket); ++positive; }
                else if (!failed && !funded) { row["status"] = "insufficient_cash"; }
            }
        }
        const auto status = row.at("status").get<std::string>();
        counts[status] = counts.value(status, std::uint64_t{0U}) + 1U;
        output["baskets"].push_back(std::move(row));
    }
    output["solver_complete"] = complete;
    output["positive_quotes"] = positive;
    output["evaluated_quantities"] = evaluated;
    output["status_counts"] = std::move(counts);
    return output;
}
}  // namespace

BasketScreenResult screen_baskets(const std::string_view input) {
    try {
        if (input.size() > maximum_screen_bytes) { return BasketScreenError{"input too large"}; }
        return screen(detail::parse_strict(input)).dump();
    } catch (const session::ReplayError& error) { return BasketScreenError{error.reason}; }
    catch (const Json::exception&) { return BasketScreenError{"invalid JSON"}; }
}

int run_basket_screen_command(const int argc, const char* const argv[]) {
    if (argc != 4 || std::string_view{argv[2]} != "screen") {
        std::cerr << "Usage: event-engine basket screen <screen.json>\n";
        return 2;
    }
    try {
        const auto result = screen_baskets(detail::read_text(argv[3], maximum_screen_bytes));
        if (const auto* error = std::get_if<BasketScreenError>(&result)) {
            std::cerr << "Basket screen input invalid: " << error->field << '\n'; return 1;
        }
        std::cout << std::get<std::string>(result) << '\n';
        return std::cout ? 0 : 1;
    } catch (const session::ReplayError& error) {
        std::cerr << "Basket screen read failed: " << error.reason << '\n'; return 1;
    }
}
}  // namespace eme::cli
