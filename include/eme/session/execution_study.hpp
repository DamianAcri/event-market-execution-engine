#pragma once

#include "eme/session/replay.hpp"

namespace eme::session {

// One economic implementation for offline replay and live, local simulation.
// No transport, credentials or order-submission capability is available here.
// Metadata and output must outlive the observer. Caller owns event ordering.
class ExecutionSimulation : public ReplayObserver {
public:
    virtual void start(bool live = false) = 0;
    virtual void report(const ReplayPlan&) = 0;
    virtual void checkpoint(std::int64_t time_ns) = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> next_event_time() const = 0;
};
[[nodiscard]] std::unique_ptr<ExecutionSimulation> make_execution_simulation(
    const gateway::kalshi::MetadataSnapshot&, const std::filesystem::path& policy_path,
    std::ostream&, bool live = false);

// Offline only: versioned, fixed-policy aggressive IOC simulation. Emits
// auditable JSONL decisions, fills, cancellations and a settlement lower bound.
[[nodiscard]] std::optional<ReplayError> run_execution_study(
    const ReplayInput& input, const std::filesystem::path& policy_path, std::ostream& output);

}  // namespace eme::session
