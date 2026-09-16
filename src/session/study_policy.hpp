#pragma once
#include "eme/core/execution_cost.hpp"
#include "study_json.hpp"
#include <array>
#include <map>

namespace eme::session::detail {
inline constexpr std::int64_t cash_limit = 1'000'000'000'000'000LL;
inline constexpr std::int64_t quantity_limit = 100'000'000LL;

struct Settlement final {
    market::MarketId market_id{};
    bool yes_wins{};
    std::int64_t time{};
};
struct LifecyclePolicy final {
    bool sequential{};
    bool reverse_legs{};
    std::array<std::int64_t, 2U> response_latency{};
    std::int64_t completion_timeout{};
    std::int64_t completion_loss_limit{};
    std::uint64_t maximum_completion_orders{};
    std::vector<Settlement> settlements;
};
struct ResidualExitPolicy final {
    bool reduce{}, reject{};
    std::int64_t latency{}, response_latency{}, timeout{}, minimum_price{}, fill_bps{};
};
struct Policy final {
    std::optional<LifecyclePolicy> lifecycle;
    std::optional<ResidualExitPolicy> residual_exit;
    bool optimal_sizing{};
    std::uint64_t max_sizing_evaluations{};
    std::int64_t capital{};
    std::int64_t operating_cost{};
    std::int64_t quantity_cap{};
    std::int64_t quantity_step{};
    std::int64_t min_margin{};
    std::int64_t max_age{};
    std::array<std::int64_t, 2U> latency{};
    std::array<bool, 2U> reject{};
    std::int64_t fill_bps{};
    std::map<market::MarketId, core::FeePolicy> fees;
    Json json;
    std::string hash;
};

[[nodiscard]] Policy load_policy(const std::filesystem::path&, const ReplayInput&);
} // namespace eme::session::detail
