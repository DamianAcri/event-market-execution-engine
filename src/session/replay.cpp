#include "eme/session/replay.hpp"
#include "study_json.hpp"

#include <algorithm>
#include <ostream>

namespace eme::session {
namespace {
using detail::Json;
std::string_view status(const book::BookUpdateResult value) {
    switch (value) {
        case book::BookUpdateResult::applied: return "applied";
        case book::BookUpdateResult::requires_snapshot: return "requires_snapshot";
        case book::BookUpdateResult::stream_mismatch: return "stream_mismatch";
        case book::BookUpdateResult::sequence_gap: return "sequence_gap";
        case book::BookUpdateResult::invalid_level: return "invalid_level";
        case book::BookUpdateResult::recovery_not_started: return "recovery_not_started";
        case book::BookUpdateResult::stale_snapshot: return "stale_snapshot";
    }
    return "unknown";
}
std::string_view action_name(const ControlAction action) {
    switch (action) {
        case ControlAction::open: return "open";
        case ControlAction::close: return "close";
        case ControlAction::recover: return "recover";
    }
    return "unknown";
}
Json candidate_json(const opportunity::CandidateEvent& event) {
    using namespace opportunity;
    const auto& id = event.id;
    Json legs = Json::array();
    for (const auto& leg : id.legs) {
        legs.push_back({{"market_id", leg.market_id},
                        {"outcome", leg.outcome == constraint::ContractOutcome::yes ? "yes" : "no"}});
    }
    Json result{{"metadata_version", id.metadata_version}, {"constraint_id", id.constraint_id},
        {"semantic_version", id.semantic_version}, {"direction", "buy_guaranteed_legs"}, {"legs", legs},
        {"kind", event.kind == CandidateEventKind::opened ? "opened" :
            event.kind == CandidateEventKind::updated ? "updated" : "invalidated"},
        {"reason_code", static_cast<unsigned>(event.reason)}};
    if (event.quote) {
        const auto& quote = *event.quote;
        result["quote"] = {{"quantity_centicontracts", quote.quantity.raw()},
            {"prices_1e4", {quote.acquisition_prices[0U].raw(), quote.acquisition_prices[1U].raw()}},
            {"cost_micro_usd", quote.acquisition_cost.raw()}, {"payout_floor_micro_usd", quote.minimum_payout.raw()},
            {"gross_margin_micro_usd", quote.gross_margin.raw()}};
    }
    return result;
}
}  // namespace

std::variant<ReplayInput, ReplayError> load_replay(
    const std::filesystem::path& directory, const std::filesystem::path& plan_path) {
    try {
        auto verified = verify_session(directory);
        if (const auto* failure = std::get_if<SessionError>(&verified)) {
            return ReplayError{std::string{to_string(failure->code)} + ": " + failure->field, failure->record_index};
        }
        auto session = std::get<VerifiedSession>(std::move(verified));
        const auto bytes = detail::read_text(plan_path);
        const auto root = detail::parse_strict(bytes);
        detail::shape(root, {"schema_version", "source_kind", "provenance", "manifest_sha256", "use_yes_price", "controls"});
        if (detail::integer(root, "schema_version") != 1U || root["use_yes_price"] != true) { detail::invalid("replay schema/price convention"); }
        ReplayPlan plan{detail::string(root, "source_kind"), detail::string(root, "provenance"),
                        detail::string(root, "manifest_sha256"), detail::fingerprint_bytes(bytes).sha256, {}};
        if (plan.source_kind != "synthetic" && plan.source_kind != "observed_ws" &&
            plan.source_kind != "observed_rest_samples") { detail::invalid("source_kind"); }
        const auto fingerprint = detail::fingerprint_file(directory / manifest_filename);
        const auto* bound = std::get_if<ArtifactFingerprint>(&fingerprint);
        if (!bound || bound->sha256 != plan.manifest_sha256) { detail::invalid("manifest binding"); }
        const auto& controls = root["controls"];
        if (!controls.is_array() || controls.empty() || controls.size() > 100'000U) { detail::invalid("controls"); }
        for (const auto& item : controls) {
            detail::shape(item, {"before_record", "time_ns", "action", "target"});
            const auto name = detail::string(item, "action");
            if (name != "open" && name != "close" && name != "recover") { detail::invalid("action"); }
            ReplayControl control{detail::integer(item, "before_record", session.manifest.records),
                static_cast<std::int64_t>(detail::integer(item, "time_ns")),
                name == "open" ? ControlAction::open : name == "close" ? ControlAction::close : ControlAction::recover,
                detail::integer(item, "target", std::numeric_limits<std::uint64_t>::max())};
            if (control.target == 0U || (control.action == ControlAction::recover &&
                (control.target > std::numeric_limits<market::MarketId>::max() ||
                 !session.metadata.markets().find(static_cast<market::MarketId>(control.target))))) { detail::invalid("target"); }
            if (!plan.controls.empty() && (control.before_record < plan.controls.back().before_record ||
                control.time_ns < plan.controls.back().time_ns)) { detail::invalid("control order"); }
            plan.controls.push_back(control);
        }
        if (plan.controls.front().action != ControlAction::open || plan.controls.front().before_record != 0U) {
            detail::invalid("initial open required");
        }
        return ReplayInput{directory, std::move(session), std::move(plan)};
    } catch (const ReplayError& failure) { return failure; }
    catch (const Json::exception&) { return ReplayError{"invalid replay JSON", 0U}; }
}

std::variant<ReplaySummary, ReplayError> replay(const ReplayInput& input,
    ReplayObserver& observer, std::ostream* output) {
    gateway::kalshi::OrderBookProcessor processor{input.session.metadata.markets()};
    opportunity::CandidateTracker tracker{input.session.manifest.metadata_version, input.session.metadata.constraints()};
    auto opened = journal::open_raw_journal_reader(input.directory / journal_filename);
    if (!std::holds_alternative<std::unique_ptr<journal::RawJournalReader>>(opened)) { return ReplayError{"journal open", 0U}; }
    auto& reader = *std::get<std::unique_ptr<journal::RawJournalReader>>(opened);
    ReplaySummary summary;
    std::size_t control_index = 0U;
    const auto emit = [output](const Json& value) { if (output) { *output << value.dump() << '\n'; } };
    emit({{"type", "replay_start"}, {"schema_version", 1U}, {"manifest_sha256", input.plan.manifest_sha256},
        {"plan_sha256", input.plan.plan_sha256}, {"source_kind", input.plan.source_kind}, {"provenance", input.plan.provenance}});
    const auto after = [&](const ReplayFrame& frame, const std::span<const opportunity::CandidateEvent> events, Json line) {
        line["record_index"] = frame.record_index;
        line["time_ns"] = frame.time_ns;
        line["candidates"] = Json::array();
        for (const auto& event : events) { line["candidates"].push_back(candidate_json(event)); }
        summary.candidate_events += events.size();
        emit(line);
        observer.after(frame, events, processor.state());
    };
    while (true) {
        while (control_index < input.plan.controls.size() &&
               input.plan.controls[control_index].before_record == summary.records) {
            const auto& control = input.plan.controls[control_index++];
            if (control.time_ns < summary.last_time_ns) { return ReplayError{"control clock regression", summary.records}; }
            observer.before(control.time_ns, processor.state());
            summary.last_time_ns = control.time_ns;
            bool accepted = false;
            switch (control.action) {
                case ControlAction::open: accepted = processor.open_connection(control.target); break;
                case ControlAction::close: accepted = processor.close_connection(control.target); break;
                case ControlAction::recover: accepted = processor.begin_recovery(static_cast<market::MarketId>(control.target)); break;
            }
            if (!accepted) { return ReplayError{"rejected control: " + std::string{action_name(control.action)}, summary.records}; }
            ++summary.controls;
            after({summary.records, control.time_ns, std::nullopt, false}, tracker.refresh_all(processor.state()),
                  {{"type", "control"}, {"action", action_name(control.action)}, {"target", control.target}});
        }
        const auto next = reader.read_next();
        if (std::holds_alternative<journal::EndOfJournal>(next)) { break; }
        const auto* record = std::get_if<journal::RawMarketRecord>(&next);
        if (!record) { return ReplayError{"invalid journal record", summary.records}; }
        if (summary.records >= input.session.manifest.records) { return ReplayError{"record count changed", summary.records}; }
        const auto time = record->received_at.time_since_epoch().count();
        if (time < summary.last_time_ns) { return ReplayError{"receive clock regression", summary.records}; }
        observer.before(time, processor.state());
        summary.last_time_ns = time;
        const auto processed = processor.process(*record);
        const auto* result = std::get_if<book::BookUpdateResult>(&processed);
        const auto market_id = processor.last_market_id();
        // A malformed record cannot silently leave an old tradable book alive.
        if (!result || !market_id) { return ReplayError{"payload/metadata/connection rejected", summary.records}; }
        const bool applied = *result == book::BookUpdateResult::applied;
        if (!applied) { ++summary.rejected_updates; }
        after({summary.records, time, market_id, applied}, tracker.refresh(*market_id, processor.state()),
            {{"type", "market"}, {"market_id", *market_id}, {"generation", record->connection_generation},
             {"sequence", record->sequence}, {"status", status(*result)}});
        ++summary.records;
    }
    if (summary.records != input.session.manifest.records || control_index != input.plan.controls.size()) {
        return ReplayError{"incomplete replay", summary.records};
    }
    observer.finish(summary.last_time_ns, processor.state());
    emit({{"type", "replay_complete"}, {"records", summary.records}, {"controls", summary.controls},
          {"candidate_events", summary.candidate_events}, {"rejected_updates", summary.rejected_updates},
          {"last_time_ns", summary.last_time_ns}});
    if (output && !*output) { return ReplayError{"output write failed", summary.records}; }
    return summary;
}
}  // namespace eme::session
