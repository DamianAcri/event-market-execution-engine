#pragma once

#include "eme/session/replay.hpp"

namespace eme::session {

// Conditional quotes only: no orders, simulated fills, positions, or transport.
// The same observer consumes live controller callbacks and finalized replay.
class BasketObservation : public ReplayObserver {
public:
    virtual void start(bool live = false) = 0;
    virtual void checkpoint(std::int64_t time_ns) = 0;
    virtual void report(const ReplayPlan&) = 0;
    [[nodiscard]] virtual std::optional<std::int64_t> next_event_time() const = 0;
};

[[nodiscard]] std::unique_ptr<BasketObservation> make_basket_observation(
    const gateway::kalshi::MetadataSnapshot&, const std::filesystem::path& policy_path,
    std::ostream&);
[[nodiscard]] std::optional<ReplayError> run_basket_observation(
    const ReplayInput&, const std::filesystem::path& policy_path, std::ostream&);

}  // namespace eme::session
