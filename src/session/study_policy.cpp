#include "study_policy.hpp"
#include <algorithm>
#include <tuple>

namespace eme::session::detail {
Policy load_policy(const std::filesystem::path& path, const ReplayInput& input) {
    const auto bytes = detail::read_text(path);
    const auto root = detail::parse_strict(bytes);
    const auto version = detail::integer(root, "schema_version");
    const bool residual_exit = version == 4U && root.value("strategy", "") == "residual_exit_v4";
    const bool lifecycle = residual_exit || (version == 3U && root.value("strategy", "") == "execution_lifecycle_v3");
    const bool optimal = lifecycle || (version == 2U && root.value("strategy", "") == "one_attempt_net_profit_v2");
    if (!optimal && (version != 1U || root.value("strategy", "") != "one_attempt_per_constraint_v1")) {
        detail::invalid("policy schema/strategy");
    }
    auto shape = root;
    if (residual_exit) { shape.erase("residual_exit"); }
    if (lifecycle) { shape.erase("lifecycle"); }
    if (optimal) { shape.erase("max_sizing_evaluations"); }
    detail::shape(shape, {"schema_version", "strategy", "fee_provenance", "capital_micro_usd", "operating_cost_micro_usd",
        "quantity_cap_centicontracts", "quantity_step_centicontracts", "min_margin_micro_usd", "max_book_age_ns",
        "leg_latency_ns", "reject_legs", "available_liquidity_bps", "fees"});
    (void)detail::string(root, "fee_provenance");
    Policy policy;
    policy.optimal_sizing = optimal;
    if (optimal) {
        policy.max_sizing_evaluations = detail::integer(root, "max_sizing_evaluations", 1'000'000U);
        if (policy.max_sizing_evaluations == 0U) { detail::invalid("sizing evaluation budget"); }
    }
    policy.capital = static_cast<std::int64_t>(detail::integer(root, "capital_micro_usd", cash_limit));
    policy.operating_cost = static_cast<std::int64_t>(detail::integer(root, "operating_cost_micro_usd", cash_limit));
    policy.quantity_cap = static_cast<std::int64_t>(detail::integer(root, "quantity_cap_centicontracts", quantity_limit));
    policy.quantity_step = static_cast<std::int64_t>(detail::integer(root, "quantity_step_centicontracts", 100U));
    policy.min_margin = static_cast<std::int64_t>(detail::integer(root, "min_margin_micro_usd", cash_limit));
    policy.max_age = static_cast<std::int64_t>(detail::integer(root, "max_book_age_ns", cash_limit));
    policy.fill_bps = static_cast<std::int64_t>(detail::integer(root, "available_liquidity_bps", 10'000U));
    if (policy.capital == 0 || policy.quantity_cap == 0 ||
        (policy.quantity_step != 1 && policy.quantity_step != 100) || policy.quantity_cap < policy.quantity_step) {
        detail::invalid("policy bounds/grid");
    }
    if (!root["leg_latency_ns"].is_array() || root["leg_latency_ns"].size() != 2U ||
        !root["reject_legs"].is_array() || root["reject_legs"].size() != 2U) { detail::invalid("leg policy"); }
    for (std::size_t leg = 0U; leg < 2U; ++leg) {
        policy.latency[leg] = static_cast<std::int64_t>(detail::integer(Json{{"latency", root["leg_latency_ns"][leg]}}, "latency", cash_limit));
        if (!root["reject_legs"][leg].is_boolean()) { detail::invalid("reject_legs"); }
        policy.reject[leg] = root["reject_legs"][leg].get<bool>();
    }
    if (!root["fees"].is_array() || root["fees"].size() > input.session.metadata.markets().size()) { detail::invalid("fees"); }
    for (const auto& item : root["fees"]) {
        detail::shape(item, {"market_id", "coefficient_ppm", "balance_quantum_micro"});
        const auto id = static_cast<market::MarketId>(detail::integer(item, "market_id", std::numeric_limits<market::MarketId>::max()));
        const core::FeePolicy fee{static_cast<std::uint32_t>(detail::integer(item, "coefficient_ppm", 1'000'000U)),
            static_cast<std::uint32_t>(detail::integer(item, "balance_quantum_micro", 10'000U))};
        if (!input.session.metadata.markets().find(id) ||
            (fee.balance_quantum_micro != 100U && fee.balance_quantum_micro != 10'000U) ||
            !policy.fees.emplace(id, fee).second) { detail::invalid("fee market/quantum"); }
    }
    for (const auto id : input.session.metadata.constraints().sorted_ids()) {
        for (const auto market : input.session.metadata.constraints().find(id)->dependent_markets) {
            if (!policy.fees.contains(market)) { detail::invalid("missing market fee policy"); }
        }
    }
    if (lifecycle) {
        const auto& config = root.at("lifecycle");
        detail::shape(config, {"execution_policy", "first_leg", "response_latency_ns", "completion_timeout_ns",
            "completion_loss_limit_micro_usd", "maximum_completion_orders", "settlements"});
        LifecyclePolicy life;
        const auto name = detail::string(config, "execution_policy");
        if (name != "parallel_hold" && name != "sequential_complete") { detail::invalid("execution_policy"); }
        life.sequential = name == "sequential_complete";
        life.reverse_legs = detail::integer(config, "first_leg", 1U) == 1U;
        if (!config["response_latency_ns"].is_array() || config["response_latency_ns"].size() != 2U) { detail::invalid("response latency"); }
        for (std::size_t leg = 0; leg < 2U; ++leg) {
            life.response_latency[leg] = static_cast<std::int64_t>(detail::integer(Json{{"latency", config["response_latency_ns"][leg]}}, "latency", cash_limit));
        }
        life.completion_timeout = static_cast<std::int64_t>(detail::integer(config, "completion_timeout_ns", cash_limit));
        life.completion_loss_limit = static_cast<std::int64_t>(detail::integer(config, "completion_loss_limit_micro_usd", cash_limit));
        life.maximum_completion_orders = detail::integer(config, "maximum_completion_orders", 4U);
        if (life.maximum_completion_orders == 0U) { detail::invalid("completion order budget"); }
        if (!config["settlements"].is_array() || config["settlements"].size() > input.session.metadata.markets().size()) { detail::invalid("settlements"); }
        std::map<market::MarketId, bool> outcomes;
        for (const auto& value : config["settlements"]) {
            detail::shape(value, {"market_id", "yes_wins", "time_ns", "provenance"});
            const auto id = static_cast<market::MarketId>(detail::integer(value, "market_id", std::numeric_limits<market::MarketId>::max()));
            if (!input.session.metadata.markets().find(id) || !value["yes_wins"].is_boolean()) { detail::invalid("settlement market/outcome"); }
            const auto yes = value["yes_wins"].get<bool>();
            if (!outcomes.emplace(id, yes).second) { detail::invalid("duplicate settlement"); }
            (void)detail::string(value, "provenance");
            life.settlements.push_back({id, yes, static_cast<std::int64_t>(detail::integer(value, "time_ns"))});
        }
        // Labels may be incomplete, but supplied outcomes cannot contradict the
        // reviewed relationship that underwrites the portfolio's payout floor.
        for (const auto id : input.session.metadata.constraints().sorted_ids()) {
            const auto& definition = *input.session.metadata.constraints().find(id);
            const auto possible = std::any_of(definition.valid_worlds.begin(), definition.valid_worlds.end(), [&](const auto& world) {
                return std::all_of(world.assignments.begin(), world.assignments.end(), [&](const auto& assignment) {
                    const auto found = outcomes.find(assignment.market_id);
                    return found == outcomes.end() || found->second == assignment.settles_yes;
                });
            });
            if (!possible) { detail::invalid("settlement contradicts reviewed relationship"); }
        }
        std::sort(life.settlements.begin(), life.settlements.end(), [](const auto& a, const auto& b) {
            return std::tie(a.time, a.market_id) < std::tie(b.time, b.market_id);
        });
        policy.lifecycle = std::move(life);
    }
    if (residual_exit) {
        const auto& config = root.at("residual_exit");
        detail::shape(config, {"mode", "arrival_latency_ns", "response_latency_ns", "timeout_ns",
            "minimum_price_1e4", "reject", "available_liquidity_bps"});
        const auto mode = detail::string(config, "mode");
        if (mode != "hold" && mode != "reduce_once") { detail::invalid("residual exit mode"); }
        if (!config["reject"].is_boolean()) { detail::invalid("residual exit rejection"); }
        ResidualExitPolicy exit;
        exit.reduce = mode == "reduce_once"; exit.reject = config["reject"].get<bool>();
        exit.latency = static_cast<std::int64_t>(detail::integer(config, "arrival_latency_ns", cash_limit));
        exit.response_latency = static_cast<std::int64_t>(detail::integer(config, "response_latency_ns", cash_limit));
        exit.timeout = static_cast<std::int64_t>(detail::integer(config, "timeout_ns", cash_limit));
        exit.minimum_price = static_cast<std::int64_t>(detail::integer(config, "minimum_price_1e4", 10'000U));
        exit.fill_bps = static_cast<std::int64_t>(detail::integer(config, "available_liquidity_bps", 10'000U));
        policy.residual_exit = exit;
    }
    policy.json = root;
    policy.hash = detail::fingerprint_bytes(bytes).sha256;
    return policy;
}

} // namespace eme::session::detail
