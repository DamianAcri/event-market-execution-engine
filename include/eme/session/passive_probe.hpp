#pragma once

#include "eme/session/replay.hpp"

namespace eme::session {
// Offline only. Has no order transport or credentials interface.
class PassiveProbe : public ReplayObserver {
public:
    virtual void report(const ReplayPlan&) = 0;
};
[[nodiscard]] std::unique_ptr<PassiveProbe> make_passive_probe(
    const gateway::kalshi::MetadataSnapshot&, const std::filesystem::path& basket_policy,
    const std::filesystem::path& probe_policy, std::ostream&);
[[nodiscard]] std::optional<ReplayError> run_passive_probe(
    const ReplayInput&, const std::filesystem::path& basket_policy,
    const std::filesystem::path& probe_policy, std::ostream&);
}  // namespace eme::session
