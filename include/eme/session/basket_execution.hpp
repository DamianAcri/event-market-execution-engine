#pragma once
#include "eme/session/replay.hpp"
namespace eme::session {
// Chronological offline counterfactuals; no network/order transport interface.
class BasketExecutionStudy : public ReplayObserver {
  public:
    virtual void report(const ReplayPlan &) = 0;
};
[[nodiscard]] std::unique_ptr<BasketExecutionStudy>
make_basket_execution_study(const gateway::kalshi::MetadataSnapshot &,
                            const std::filesystem::path &basket_policy,
                            const std::filesystem::path &study_policy, std::ostream &);
[[nodiscard]] std::optional<ReplayError>
run_basket_execution_study(const ReplayInput &, const std::filesystem::path &basket_policy,
                           const std::filesystem::path &study_policy, std::ostream &);
} // namespace eme::session
