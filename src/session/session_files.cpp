#include "session_files.hpp"

#include <nlohmann/json.hpp>
#include <picosha2.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace eme::session::detail {
namespace {

using Json = nlohmann::json;

[[nodiscard]] SessionError error(const SessionErrorCode code,
                               const std::filesystem::path& path) {
    return {code, path.filename().string(), 0U};
}

[[nodiscard]] std::optional<SessionError> require_regular(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        return error(SessionErrorCode::invalid_artifact, path);
    }
    return std::nullopt;
}

[[noreturn]] void reject(const std::string_view field,
                        const SessionErrorCode code = SessionErrorCode::invalid_manifest) {
    throw SessionError{code, std::string{field}, 0U};
}

void shape(const Json& object, const std::initializer_list<std::string_view> fields) {
    if (!object.is_object() || object.size() != fields.size()) { reject("shape"); }
    for (const auto field : fields) {
        if (!object.contains(field)) { reject(field); }
    }
}

[[nodiscard]] std::uint64_t integer(const Json& object, const std::string_view key) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) { reject(key); }
    return value.get<std::uint64_t>();
}

[[nodiscard]] ArtifactFingerprint artifact(const Json& value) {
    shape(value, {"bytes", "sha256"});
    if (!value["sha256"].is_string()) { reject("sha256"); }
    auto hash = value["sha256"].get<std::string>();
    if (hash.size() != 64U || !std::all_of(hash.begin(), hash.end(), [](const char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) { reject("sha256"); }
    return {integer(value, "bytes"), std::move(hash)};
}

}  // namespace

std::variant<std::string, SessionError> read_bounded(
    const std::filesystem::path& path, const std::size_t maximum) {
    if (const auto failure = require_regular(path)) { return *failure; }
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { return error(SessionErrorCode::io_error, path); }
    if (size > maximum) { return error(SessionErrorCode::input_too_large, path); }
    std::ifstream input{path, std::ios::binary};
    if (!input) { return error(SessionErrorCode::io_error, path); }
    // Allocate for the artifact, not the 4 MiB limit. The extra byte detects growth.
    std::string bytes(static_cast<std::size_t>(size) + 1U, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (input.bad() || (input.fail() && !input.eof())) {
        return error(SessionErrorCode::io_error, path);
    }
    if (count > maximum) { return error(SessionErrorCode::input_too_large, path); }
    if (count != size) { return error(SessionErrorCode::invalid_artifact, path); }
    bytes.resize(count);
    return bytes;
}

ArtifactFingerprint fingerprint_bytes(const std::string_view bytes) {
    return {static_cast<std::uint64_t>(bytes.size()),
            picosha2::hash256_hex_string(bytes.begin(), bytes.end())};
}

std::variant<ArtifactFingerprint, SessionError> fingerprint_file(const std::filesystem::path& path) {
    if (const auto failure = require_regular(path)) { return *failure; }
    std::ifstream input{path, std::ios::binary};
    if (!input) { return error(SessionErrorCode::io_error, path); }
    picosha2::hash256_one_by_one hash;
    std::array<char, 64U * 1024U> buffer{};
    std::uint64_t total = 0U;
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (input.bad() || (input.fail() && !input.eof())) {
            return error(SessionErrorCode::io_error, path);
        }
        const auto size = static_cast<std::uint64_t>(count);
        if (size > std::numeric_limits<std::uint64_t>::max() - total) {
            return error(SessionErrorCode::input_too_large, path);
        }
        total += size;
        hash.process(buffer.data(), buffer.data() + count);
    }
    hash.finish();
    return ArtifactFingerprint{total, picosha2::get_hash_hex_string(hash)};
}

std::optional<SessionError> write_new_file(
    const std::filesystem::path& path, const std::string_view bytes) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (status.type() != std::filesystem::file_type::not_found ||
        (ec && ec != std::errc::no_such_file_or_directory)) {
        return error(SessionErrorCode::invalid_artifact, path);
    }
    // The session directory has a single owner; concurrent modification is unsupported.
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output) { return error(SessionErrorCode::io_error, path); }
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    output.close();
    if (!output) { return error(SessionErrorCode::io_error, path); }
    return std::nullopt;
}

std::variant<SessionManifest, SessionError> parse_manifest(const std::string_view bytes) {
    if (bytes.size() > maximum_manifest_bytes) {
        return SessionError{SessionErrorCode::input_too_large, "manifest.json", 0U};
    }
    try {
        std::vector<std::unordered_set<std::string>> keys;
        const auto root = Json::parse(bytes, [&keys](
            const int depth, const Json::parse_event_t event, Json& value) {
            if (depth > 3) { reject("depth"); }
            if (event == Json::parse_event_t::object_start) { keys.emplace_back(); }
            else if (event == Json::parse_event_t::object_end) { keys.pop_back(); }
            else if (event == Json::parse_event_t::key &&
                     !keys.back().insert(value.get<std::string>()).second) {
                reject("duplicate_field");
            }
            return true;
        });
        shape(root, {"schema_version", "state", "venue", "journal_schema_version",
                     "metadata_version", "metadata", "journal", "records"});
        if (integer(root, "schema_version") != 1U ||
            integer(root, "journal_schema_version") != journal::current_schema_version) {
            reject("schema_version", SessionErrorCode::unsupported_version);
        }
        if (root["state"] != "complete" || root["venue"] != "kalshi") { reject("state/venue"); }
        SessionManifest manifest{integer(root, "metadata_version"), artifact(root["metadata"]),
                                 artifact(root["journal"]), integer(root, "records")};
        // Journal v2 has a 12-byte file header and at least 53+8 bytes per frame.
        if (manifest.metadata_version == 0U || manifest.metadata.bytes == 0U ||
            manifest.metadata.bytes > gateway::kalshi::maximum_metadata_bytes ||
            manifest.journal.bytes < 12U ||
            manifest.records > (manifest.journal.bytes - 12U) / 61U) {
            reject("bounds");
        }
        return manifest;
    } catch (const SessionError& failure) { return failure; }
    catch (const Json::exception&) {
        return SessionError{SessionErrorCode::invalid_manifest, "json", 0U};
    }
}

std::string serialize_manifest(const SessionManifest& manifest) {
    const auto encoded = [](const ArtifactFingerprint& value) {
        return Json{{"bytes", value.bytes}, {"sha256", value.sha256}};
    };
    return Json{{"schema_version", 1U}, {"state", "complete"}, {"venue", "kalshi"},
                {"journal_schema_version", journal::current_schema_version},
                {"metadata_version", manifest.metadata_version},
                {"metadata", encoded(manifest.metadata)}, {"journal", encoded(manifest.journal)},
                {"records", manifest.records}}.dump() + '\n';
}

std::optional<SessionError> validate_journal(
    const std::filesystem::path& path, const market::MetadataVersion version,
    const std::uint64_t expected_records) {
    if (const auto failure = require_regular(path)) { return failure; }
    auto opened = journal::open_raw_journal_reader(path);
    if (const auto* failure = std::get_if<journal::JournalError>(&opened)) {
        return SessionError{SessionErrorCode::invalid_journal,
                            std::string{journal::to_string(failure->code)}, failure->record_index};
    }
    auto& reader = *std::get<std::unique_ptr<journal::RawJournalReader>>(opened);
    while (true) {
        auto next = reader.read_next();
        if (std::holds_alternative<journal::EndOfJournal>(next)) { break; }
        if (const auto* failure = std::get_if<journal::JournalError>(&next)) {
            return SessionError{SessionErrorCode::invalid_journal,
                                std::string{journal::to_string(failure->code)}, failure->record_index};
        }
        if (std::get<journal::RawMarketRecord>(next).metadata_version != version) {
            return SessionError{SessionErrorCode::metadata_version_mismatch,
                                "record.metadata_version", reader.records_read() - 1U};
        }
        if (reader.records_read() > expected_records) {
            return SessionError{SessionErrorCode::record_count_mismatch, "records", expected_records};
        }
    }
    if (reader.records_read() != expected_records) {
        return SessionError{SessionErrorCode::record_count_mismatch, "records", reader.records_read()};
    }
    return std::nullopt;
}

}  // namespace eme::session::detail
