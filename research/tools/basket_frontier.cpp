// Offline diagnostic, not an execution policy. Reuses production replay,
// ledger, funding and sizing; reports point observations, never inferred fills.
#include "eme/core/basket_sizing.hpp"
#include "eme/session/basket_observation.hpp"
#include "session/study_json.hpp"
#include <array>
#include <iostream>
#include <sstream>
#include <unordered_map>

using namespace eme;
using Json = nlohmann::json;
using I = std::int64_t;
constexpr I cap = 10000;
struct Cost { I notional{}, debit{}, reserve{}; bool present{}; };
struct Cache {
    core::FeePolicy fee{};
    std::array<std::vector<core::BuyLevel>, 2> depth;
    std::array<std::array<Cost, 101>, 2> costs{};
    std::array<I, 2> passive_price{-1, -1}, queue{};
    I updated{};
    bool valid{};
};

Cost cost(const std::vector<core::BuyLevel>& depth, const core::FeePolicy fee, const I quantity) {
    core::FeeAccumulator accumulator;
    Cost out;
    I left = quantity, limit = 0;
    for (const auto& level : depth) {
        const auto take = std::min(left, level.quantity.raw());
        if (!take) { break; }
        const auto charge = core::charge_buy_fill(*core::Quantity::from_raw(take), level.price, fee, accumulator);
        if (!charge) { throw std::runtime_error("ledger failure"); }
        out.notional += charge->notional.raw(); out.debit += charge->debit.raw();
        left -= take; limit = level.price.raw();
    }
    if (left) { return out; }
    const auto reserve = core::buy_reservation(*core::Quantity::from_raw(quantity),
        *core::Quantity::from_raw(1), *core::Price::from_raw(limit), fee);
    if (!reserve) { throw std::runtime_error("reserve failure"); }
    out.reserve = reserve->raw(); out.present = true;
    return out;
}

Cost passive_cost(const I price, const core::FeePolicy fee, const I quantity) {
    core::FeeAccumulator accumulator;
    const auto q = *core::Quantity::from_raw(quantity);
    const auto p = *core::Price::from_raw(price);
    const auto charge = core::charge_buy_fill(q, p, fee, accumulator);
    const auto reserve = core::buy_reservation(q, *core::Quantity::from_raw(1), p, fee);
    if (!charge || !reserve) { throw std::runtime_error("passive diagnostic arithmetic failure"); }
    return {charge->notional.raw(), charge->debit.raw(), reserve->raw(), true};
}

struct Point { I q{}, net{}, gross{}, fees{}, reserve{}, time{}, generation{}; std::uint64_t record{}; int passive{-1}; };
struct Metric {
    std::uint64_t states{}, quantities{}, gross_positive{}, net_positive{};
    std::optional<Point> best_total, best_unit;
    Json witness;
};
struct Basket { unsigned id{}; std::array<unsigned, 3> ids{}; std::array<Metric, 3> metrics; };

Json point_json(const Point& p) {
    return {{"quantity_centicontracts", p.q}, {"net_margin_micro", p.net},
        {"gross_margin_micro", p.gross}, {"fees_and_rounding_micro", p.fees},
        {"reservation_micro", p.reserve}, {"time_ns", p.time}, {"record_index", p.record},
        {"generation", p.generation}, {"passive_leg_index", p.passive}};
}
bool unit_better(const Point& p, const Point& other) {
    return p.net * other.q > other.net * p.q;
}

class Frontier final : public session::ReplayObserver {
public:
    explicit Frontier(const Json& policy) {
        from_ = policy.at("valid_from_unix_ms").get<I>() * 1000000;
        until_ = policy.at("valid_until_unix_ms").get<I>() * 1000000;
        const auto& screen = policy.at("screen");
        cash_ = screen.at("sizing").at("available_cash_micro").get<I>();
        if (screen.at("sizing").at("cap_centicontracts") != cap ||
            screen.at("sizing").at("step_centicontracts") != 100 ||
            screen.at("sizing").at("minimum_margin_micro") != 0) {
            throw std::runtime_error("diagnostic requires frozen whole sizes 1..100 and zero admission margin");
        }
        for (const auto& row : screen.at("fees")) {
            auto& m = markets_[row.at("market_id").get<unsigned>()];
            m.fee = {row.at("coefficient_ppm").get<std::uint32_t>(), row.at("balance_quantum_micro").get<std::uint32_t>()};
        }
        for (const auto& row : screen.at("baskets")) {
            Basket b; b.id = row.at("id").get<unsigned>();
            b.ids = {row.at("lower_market_id").get<unsigned>(), row.at("upper_market_id").get<unsigned>(), row.at("range_market_id").get<unsigned>()};
            for (const auto id : b.ids) { dependencies_[id].push_back(baskets_.size()); }
            baskets_.push_back(std::move(b));
        }
    }
    void after(const session::ReplayFrame& frame, std::span<const opportunity::CandidateEvent>, const market::MarketState& state) override {
        ++records_;
        if (frame.trade) { ++trades_; }
        if (!frame.observed_wall_ns || *frame.observed_wall_ns < from_ || *frame.observed_wall_ns >= until_) { return; }
        if (!state.connected() || state.connection_generation() != generation_) {
            generation_ = state.connection_generation().value_or(0);
            for (auto& [id, m] : markets_) { (void)id; m.valid = false; }
        }
        if (!state.connected() || !frame.applied || !frame.market_id) { return; }
        ++updates_;
        auto& m = markets_.at(*frame.market_id);
        m.updated = frame.time_ns;
        const auto* book = state.find_book(*frame.market_id);
        m.valid = book && book->state() == book::BookState::valid &&
            !(book->best_bid() && book->best_ask() && book->best_bid()->raw() > book->best_ask()->raw());
        if (m.valid) {
            for (std::size_t side = 0; side < 2; ++side) {
                auto& depth = m.depth[side]; depth.clear(); I available = 0;
                book->visit_levels(side == 0 ? book::Side::ask : book::Side::bid, [&](const book::Level level) {
                    const auto take = std::min(level.quantity.raw(), cap - available);
                    if (take) { depth.push_back({*core::Price::from_raw(side == 0 ? level.price.raw() : 10000 - level.price.raw()), *core::Quantity::from_raw(take)}); available += take; }
                    return available < cap;
                });
                for (I q = 1; q <= 100; ++q) { m.costs[side][static_cast<std::size_t>(q)] = cost(depth, m.fee, q * 100); }
                const auto bid = side == 0 ? book->best_bid() : book->best_ask();
                m.passive_price[side] = bid ? (side == 0 ? bid->raw() : 10000 - bid->raw()) : -1;
                m.queue[side] = bid ? book->quantity_at(side == 0 ? book::Side::bid : book::Side::ask, *bid).raw() : 0;
                // Joining must rest rather than cross the opposite side.
                if (!depth.empty() && m.passive_price[side] >= depth.front().price.raw()) { m.passive_price[side] = -1; }
            }
        }
        for (const auto index : dependencies_.at(*frame.market_id)) { evaluate(baskets_[index], frame); }
    }
    Json result() const {
        Json rows = Json::array();
        constexpr std::array<const char*, 3> names{"all_taker", "one_passive_zero_fee_hypothesis", "one_passive_taker_fee_stress"};
        for (const auto& b : baskets_) {
            for (std::size_t mode = 0; mode < 3; ++mode) {
                const auto& m = b.metrics[mode];
                rows.push_back({{"basket_id", b.id}, {"mode", names[mode]}, {"evaluated_states", m.states},
                    {"funded_quantities", m.quantities}, {"gross_positive_quantities", m.gross_positive},
                    {"net_positive_quantities", m.net_positive},
                    {"best_total_net", m.best_total ? point_json(*m.best_total) : Json(nullptr)},
                    {"best_net_per_contract", m.best_unit ? point_json(*m.best_unit) : Json(nullptr)},
                    {"best_per_contract_witness", m.witness}});
            }
        }
        Json sizes = Json::array();
        for (std::size_t q = 1; q <= 100; ++q) {
            const auto& m = sizes_[q];
            sizes.push_back({{"contracts", q}, {"observations", m.quantities}, {"gross_positive", m.gross_positive},
                {"net_positive", m.net_positive}, {"best_net", m.best_total ? point_json(*m.best_total) : Json(nullptr)}});
        }
        return {{"schema_version", 1}, {"records", records_}, {"book_updates", updates_}, {"public_trades", trades_},
            {"basket_evaluations", evaluations_}, {"native_sizing_parity_checks", parity_},
            {"native_evaluated_quantities", native_quantities_}, {"rows", rows}, {"all_taker_by_size", sizes},
            {"orders_sent", 0}, {"simulated_fills", false}, {"realized_pnl_micro", nullptr},
            {"coverage_duration_estimated", false}, {"counts_are_independent_trials", false},
            {"passive_fee_verified", false}, {"passive_comparators_are_executable_upper_bounds", false}};
    }
private:
    Json witness(const Basket& b, const Point& p) const {
        Json legs = Json::array();
        for (std::size_t leg = 0; leg < 3; ++leg) {
            const auto& m = markets_.at(b.ids[leg]); const auto side = leg == 0 ? 0U : 1U;
            Json levels = Json::array();
            for (const auto& level : m.depth[side]) { levels.push_back({level.price.raw(), level.quantity.raw()}); }
            legs.push_back({{"market_id", b.ids[leg]}, {"buy_depth", levels}, {"fee_coefficient_ppm", m.fee.coefficient_ppm},
                {"balance_quantum_micro", m.fee.balance_quantum_micro}, {"last_book_update_ns", m.updated},
                {"join_price_1e4", m.passive_price[side]}, {"visible_queue_centicontracts", m.queue[side]}});
        }
        return {{"point", point_json(p)}, {"legs", legs}};
    }
    static void count(Metric& m, const Point& p) {
        ++m.quantities;
        if (p.gross > 0) { ++m.gross_positive; }
        if (p.net > 0) { ++m.net_positive; }
        if (!m.best_total || p.net > m.best_total->net) { m.best_total = p; }
    }
    void evaluate(Basket& b, const session::ReplayFrame& frame) {
        ++evaluations_;
        for (const auto id : b.ids) { if (!markets_.at(id).valid) { return; } }
        std::array<core::BuyDepth, 3> native_depth;
        for (std::size_t i = 0; i < 3; ++i) { const auto& m = markets_.at(b.ids[i]); native_depth[i] = {m.depth[i == 0 ? 0 : 1], m.fee}; }
        const auto native = core::size_buy_basket(native_depth, {*core::Quantity::from_raw(cap), *core::Quantity::from_raw(100), *core::Cash::from_raw(cash_), *core::Cash::from_raw(0), 100});
        native_quantities_ += native.evaluated_quantities;
        if (native.status != core::SizingStatus::optimal && native.status != core::SizingStatus::no_positive_margin &&
            native.status != core::SizingStatus::no_depth && native.status != core::SizingStatus::insufficient_cash) { throw std::runtime_error("native sizing failure"); }
        std::optional<Point> positive;
        for (std::size_t mode = 0; mode < 3; ++mode) {
            auto& metric = b.metrics[mode]; ++metric.states;
            for (int passive = mode == 0 ? -1 : 0; passive < (mode == 0 ? 0 : 3); ++passive) {
                for (I q = 1; q <= 100; ++q) {
                    I notional = 0, debit = 0, reserve = 0; bool available = true;
                    for (int leg = 0; leg < 3; ++leg) {
                        const auto& m = markets_.at(b.ids[static_cast<std::size_t>(leg)]); const auto side = leg == 0 ? 0U : 1U;
                        auto charged = m.costs[side][static_cast<std::size_t>(q)];
                        if (passive == leg) {
                            if (m.passive_price[side] < 0) { available = false; break; }
                            const core::FeePolicy fee{mode == 1 ? 0U : m.fee.coefficient_ppm, m.fee.balance_quantum_micro};
                            charged = passive_cost(m.passive_price[side], fee, q * 100);
                        }
                        if (!charged.present) { available = false; break; }
                        notional += charged.notional; debit += charged.debit; reserve += charged.reserve;
                    }
                    if (!available || reserve > cash_) { continue; }
                    const Point point{q * 100, q * 2000000 - debit, q * 2000000 - notional, debit - notional,
                        reserve, frame.time_ns, static_cast<I>(generation_), frame.record_index, passive};
                    count(metric, point);
                    if (!metric.best_unit || unit_better(point, *metric.best_unit)) {
                        metric.best_unit = point; metric.witness = witness(b, point);
                    }
                    if (mode == 0) {
                        count(sizes_[static_cast<std::size_t>(q)], point);
                        if (point.net > 0 && (!positive || point.net > positive->net ||
                            (point.net == positive->net && point.reserve < positive->reserve))) { positive = point; }
                        if (q == 1 && (!native.one_contract_diagnostic || native.one_contract_diagnostic->net_margin_micro != point.net)) { throw std::runtime_error("one-contract parity mismatch"); }
                    }
                }
            }
        }
        if (positive.has_value() != native.quote.has_value() || (positive &&
            (positive->q != native.quote->quantity.raw() || positive->net != native.quote->net_margin_micro))) { throw std::runtime_error("positive sizing parity mismatch"); }
        ++parity_;
    }
    std::unordered_map<unsigned, Cache> markets_;
    std::unordered_map<unsigned, std::vector<std::size_t>> dependencies_;
    std::vector<Basket> baskets_;
    std::array<Metric, 101> sizes_;
    I from_{}, until_{}, cash_{};
    std::uint64_t generation_{}, records_{}, updates_{}, trades_{}, evaluations_{}, native_quantities_{}, parity_{};
};

int main(int argc, char** argv) {
    try {
        if (argc != 2) { throw std::runtime_error("usage: basket-frontier CAPTURE_DIRECTORY"); }
        const std::filesystem::path root{argv[1]};
        const auto policy_path = root / "session/basket-policy.json";
        const auto policy = session::detail::parse_strict(session::detail::read_text(policy_path));
        auto loaded = session::load_replay(root / "session", root / "session/replay.json");
        if (const auto* failure = std::get_if<session::ReplayError>(&loaded)) { throw std::runtime_error(failure->reason); }
        const auto& input = std::get<session::ReplayInput>(loaded);
        std::ostringstream reference_output;
        // Validate every policy and contract shape through the production observer.
        const auto reference = session::make_basket_observation(input.session.metadata, policy_path, reference_output);
        Frontier observer{policy};
        const auto replayed = session::replay(input, observer);
        if (const auto* failure = std::get_if<session::ReplayError>(&replayed)) { throw std::runtime_error(failure->reason); }
        auto result = observer.result();
        result["manifest_sha256"] = input.plan.manifest_sha256;
        result["policy_sha256"] = session::detail::fingerprint_bytes(session::detail::read_text(policy_path)).sha256;
        std::cout << result.dump(2) << '\n';
    } catch (const session::ReplayError& error) { std::cerr << error.reason << '\n'; return 1; }
      catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
