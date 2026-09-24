#pragma once

#include "eme/session/replay.hpp"
#include "eme/core/basket_sizing.hpp"
#include <functional>

namespace eme::session {

struct BasketEpisode final {
    std::uint32_t basket_id{};
    std::uint64_t episode{};
    std::int64_t time_ns{};
    core::SizedBasket quote;
    bool left_censored{};
};
using BasketEpisodeHandler = std::function<void(const BasketEpisode&)>;

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
// Typed onset notification shares the continuous control's decision kernel.
// Called only on positive transitions, never by reading a future episode trace.
[[nodiscard]] std::unique_ptr<BasketObservation> make_basket_observation(
    const gateway::kalshi::MetadataSnapshot&, const std::filesystem::path& policy_path,
    std::ostream&, BasketEpisodeHandler);
[[nodiscard]] std::optional<ReplayError> run_basket_observation(
    const ReplayInput&, const std::filesystem::path& policy_path, std::ostream&);

}  // namespace eme::session
