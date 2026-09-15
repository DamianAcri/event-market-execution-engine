#pragma once

#include "eme/session/replay.hpp"

namespace eme::session {

// Offline only: versioned, fixed-policy aggressive IOC simulation. Emits
// auditable JSONL decisions, fills, cancellations and a settlement lower bound.
[[nodiscard]] std::optional<ReplayError> run_execution_study(
    const ReplayInput& input, const std::filesystem::path& policy_path, std::ostream& output);

}  // namespace eme::session
