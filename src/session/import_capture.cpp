#include "eme/session/replay.hpp"
#include "study_json.hpp"

namespace eme::session {

std::optional<ReplayError> import_capture(const std::filesystem::path& metadata_path,
    const std::filesystem::path& capture_path, const std::filesystem::path& new_directory) {
    try {
        auto metadata = gateway::kalshi::parse_metadata_snapshot(detail::read_text(metadata_path));
        if (!std::holds_alternative<gateway::kalshi::MetadataSnapshot>(metadata)) { detail::invalid("capture metadata"); }
        const auto root = detail::parse_strict(detail::read_text(capture_path, 64U * 1024U * 1024U));
        detail::shape(root, {"schema_version", "source_kind", "provenance", "use_yes_price", "controls", "records"});
        if (detail::integer(root, "schema_version") != 1U || root["use_yes_price"] != true ||
            !root["records"].is_array() || root["records"].size() > 100'000U) { detail::invalid("capture schema/records"); }
        const auto& snapshot = std::get<gateway::kalshi::MetadataSnapshot>(metadata);
        auto created = create_session(new_directory, snapshot);
        if (!std::holds_alternative<std::unique_ptr<SessionWriter>>(created)) { detail::invalid("create capture session"); }
        auto& writer = *std::get<std::unique_ptr<SessionWriter>>(created);
        for (const auto& item : root["records"]) {
            detail::shape(item, {"generation", "time_ns", "observed_at_ns", "sequence", "payload"});
            const auto channel = detail::string(item["payload"], "type");
            const journal::RawMarketRecord record{journal::current_schema_version, snapshot.markets().metadata_version(),
                detail::integer(item, "generation", std::numeric_limits<std::uint64_t>::max()),
                market::ReceiveTime{std::chrono::nanoseconds{detail::integer(item, "time_ns")}},
                journal::WallTime{std::chrono::nanoseconds{detail::integer(item, "observed_at_ns")}},
                detail::integer(item, "sequence", std::numeric_limits<std::uint64_t>::max()), std::nullopt, channel, item["payload"].dump()};
            if (writer.append(record)) { detail::invalid("capture record"); }
        }
        if (!std::holds_alternative<SessionManifest>(writer.finalize())) { detail::invalid("capture finalize"); }
        const auto manifest = detail::fingerprint_file(new_directory / manifest_filename);
        if (!std::holds_alternative<ArtifactFingerprint>(manifest)) { detail::invalid("capture manifest"); }
        const detail::Json plan{{"schema_version", 1U}, {"source_kind", root["source_kind"]}, {"provenance", root["provenance"]},
            {"use_yes_price", true}, {"manifest_sha256", std::get<ArtifactFingerprint>(manifest).sha256}, {"controls", root["controls"]}};
        if (detail::write_new_file(new_directory / "replay.json", plan.dump() + '\n')) { detail::invalid("capture plan write"); }
        auto loaded = load_replay(new_directory, new_directory / "replay.json");
        if (const auto* failure = std::get_if<ReplayError>(&loaded)) { return *failure; }
        ReplayObserver observer;
        const auto checked = replay(std::get<ReplayInput>(loaded), observer);
        if (const auto* failure = std::get_if<ReplayError>(&checked)) { return *failure; }
        return std::nullopt;
    } catch (const ReplayError& failure) { return failure; }
    catch (const detail::Json::exception&) { return ReplayError{"invalid capture JSON", 0U}; }
}
}  // namespace eme::session
