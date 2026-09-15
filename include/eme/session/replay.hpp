#pragma once

#include "eme/gateway/kalshi/orderbook_processor.hpp"
#include "eme/opportunity/candidate_tracker.hpp"
#include "eme/session/capture_session.hpp"

#include <iosfwd>
#include <span>
#include <vector>

namespace eme::session {

enum class ControlAction : std::uint8_t { open, close, recover };
struct ReplayControl final {
    std::uint64_t before_record{};
    std::int64_t time_ns{};
    ControlAction action{};
    std::uint64_t target{}; // generation for open/close, market ID for recovery
};
struct ReplayPlan final {
    std::string source_kind;
    std::string provenance;
    std::string manifest_sha256;
    std::string plan_sha256;
    std::vector<ReplayControl> controls;
    bool ws_controller{};
    std::vector<market::MarketId> markets;
};
struct ReplayInput final {
    std::filesystem::path directory;
    VerifiedSession session;
    ReplayPlan plan;
};
struct ReplayError final { std::string reason; std::uint64_t record_index{}; };

[[nodiscard]] std::variant<ReplayInput, ReplayError> load_replay(
    const std::filesystem::path& directory, const std::filesystem::path& plan_path);

// Explicit offline adapter envelope -> session v1 + manifest-bound replay.json.
// The source label/provenance is an assertion, not proof of authenticity.
[[nodiscard]] std::optional<ReplayError> import_capture(
    const std::filesystem::path& metadata_path, const std::filesystem::path& capture_path,
    const std::filesystem::path& new_directory);

struct ReplayFrame final {
    std::uint64_t record_index{}; // raw-record ordinal, controls occur before it
    std::int64_t time_ns{};
    std::optional<market::MarketId> market_id;
    bool applied{}; // true only for successfully applied market data
};
class ReplayObserver {
public:
    virtual ~ReplayObserver() = default;
    // Called before each input, with the state from earlier inputs only.
    virtual void before(std::int64_t, const market::MarketState&) {}
    virtual void after(const ReplayFrame&, std::span<const opportunity::CandidateEvent>,
                       const market::MarketState&) {}
    // End of observation: orders beyond this time must remain unfilled.
    virtual void finish(std::int64_t, const market::MarketState&) {}
};
struct ReplaySummary final {
    std::uint64_t records{};
    std::uint64_t controls{};
    std::uint64_t candidate_events{};
    std::uint64_t rejected_updates{};
    std::int64_t last_time_ns{};
};

// Streaming replay. Input files must remain unchanged after load_replay.
// JSONL is provisional until replay_complete; an error never emits completion.
[[nodiscard]] std::variant<ReplaySummary, ReplayError> replay(
    const ReplayInput& input, ReplayObserver& observer, std::ostream* jsonl = nullptr);

}  // namespace eme::session
