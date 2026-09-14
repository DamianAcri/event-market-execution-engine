#include "eme/session/capture_session.hpp"

#include "session_files.hpp"

#include <system_error>
#include <utility>

namespace eme::session {
namespace {

[[nodiscard]] std::optional<SessionError> require_directory(const std::filesystem::path& path) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec || !std::filesystem::is_directory(status)) {
        return SessionError{SessionErrorCode::invalid_artifact, "directory", 0U};
    }
    return std::nullopt;
}

}  // namespace

SessionWriter::SessionWriter(std::filesystem::path directory, SessionManifest manifest,
                             std::unique_ptr<journal::RawJournalWriter> writer)
    : directory_{std::move(directory)}, manifest_{std::move(manifest)}, writer_{std::move(writer)} {}

CreateSessionResult create_session(
    const std::filesystem::path& directory,
    const gateway::kalshi::MetadataSnapshot& metadata) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(directory, ec);
    if (ec) { return SessionError{SessionErrorCode::io_error, "directory", 0U}; }
    if (!std::filesystem::create_directory(absolute, ec)) {
        return SessionError{(!ec || ec == std::errc::file_exists)
            ? SessionErrorCode::directory_exists : SessionErrorCode::io_error, "directory", 0U};
    }
    // Persist precisely the canonical bytes fingerprinted here (no added LF).
    const auto& bytes = metadata.canonical_json();
    if (bytes.size() > gateway::kalshi::maximum_metadata_bytes) {
        return SessionError{SessionErrorCode::input_too_large, "metadata.json", 0U};
    }
    if (const auto failure = detail::write_new_file(absolute / metadata_filename, bytes)) {
        return *failure;
    }
    auto opened = journal::open_raw_journal_writer(absolute / journal_filename);
    if (const auto* failure = std::get_if<journal::JournalError>(&opened)) {
        return SessionError{SessionErrorCode::invalid_journal,
                            std::string{journal::to_string(failure->code)}, failure->record_index};
    }
    SessionManifest manifest{metadata.markets().metadata_version(),
                             detail::fingerprint_bytes(bytes), {}, 0U};
    return std::unique_ptr<SessionWriter>{new SessionWriter{std::move(absolute),
        std::move(manifest), std::move(std::get<std::unique_ptr<journal::RawJournalWriter>>(opened))}};
}

std::optional<SessionError> SessionWriter::append(const journal::RawMarketRecord& record) {
    if (closed_) { return SessionError{SessionErrorCode::writer_closed, "append", manifest_.records}; }
    if (record.metadata_version != manifest_.metadata_version) {
        return SessionError{SessionErrorCode::metadata_version_mismatch,
                            "record.metadata_version", manifest_.records};
    }
    if (const auto failure = writer_->append(record)) {
        if (failure->code == journal::JournalErrorCode::io_error) { closed_ = true; }
        return SessionError{SessionErrorCode::invalid_journal,
                            std::string{journal::to_string(failure->code)}, failure->record_index};
    }
    manifest_.records = writer_->records_written();
    return std::nullopt;
}

FinalizeSessionResult SessionWriter::finalize() {
    if (closed_) { return SessionError{SessionErrorCode::writer_closed, "finalize", manifest_.records}; }
    closed_ = true;
    if (const auto failure = writer_->flush()) {
        return SessionError{SessionErrorCode::io_error, "market.journal", failure->record_index};
    }
    writer_.reset();
    if (const auto failure = require_directory(directory_)) { return *failure; }
    auto metadata_bytes = detail::read_bounded(
        directory_ / metadata_filename, gateway::kalshi::maximum_metadata_bytes);
    if (const auto* failure = std::get_if<SessionError>(&metadata_bytes)) { return *failure; }
    if (detail::fingerprint_bytes(std::get<std::string>(metadata_bytes)) != manifest_.metadata) {
        return SessionError{SessionErrorCode::fingerprint_mismatch, "metadata.json", 0U};
    }
    if (const auto failure = detail::validate_journal(
            directory_ / journal_filename, manifest_.metadata_version, manifest_.records)) {
        return *failure;
    }
    auto journal_hash = detail::fingerprint_file(directory_ / journal_filename);
    if (const auto* failure = std::get_if<SessionError>(&journal_hash)) { return *failure; }
    manifest_.journal = std::move(std::get<ArtifactFingerprint>(journal_hash));
    const auto pending = directory_ / detail::pending_manifest_filename;
    if (const auto failure = detail::write_new_file(pending, detail::serialize_manifest(manifest_))) {
        return *failure;
    }
    const auto final = directory_ / manifest_filename;
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(final, ec);
    if (status.type() != std::filesystem::file_type::not_found ||
        (ec && ec != std::errc::no_such_file_or_directory)) {
        return SessionError{SessionErrorCode::invalid_artifact, "manifest.json", 0U};
    }
    ec.clear();
    std::filesystem::rename(pending, final, ec);
    if (ec) { return SessionError{SessionErrorCode::io_error, "manifest.json", 0U}; }
    return manifest_;
}

VerifySessionResult verify_session(const std::filesystem::path& directory) {
    if (const auto failure = require_directory(directory)) { return *failure; }
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(directory / manifest_filename, ec);
    if (status.type() == std::filesystem::file_type::not_found &&
        (!ec || ec == std::errc::no_such_file_or_directory)) {
        return SessionError{SessionErrorCode::incomplete_session, "manifest.json", 0U};
    }
    auto manifest_bytes = detail::read_bounded(directory / manifest_filename, detail::maximum_manifest_bytes);
    if (const auto* failure = std::get_if<SessionError>(&manifest_bytes)) { return *failure; }
    auto parsed = detail::parse_manifest(std::get<std::string>(manifest_bytes));
    if (const auto* failure = std::get_if<SessionError>(&parsed)) { return *failure; }
    auto manifest = std::move(std::get<SessionManifest>(parsed));
    auto metadata_bytes = detail::read_bounded(
        directory / metadata_filename, gateway::kalshi::maximum_metadata_bytes);
    if (const auto* failure = std::get_if<SessionError>(&metadata_bytes)) { return *failure; }
    const auto& bytes = std::get<std::string>(metadata_bytes);
    if (detail::fingerprint_bytes(bytes) != manifest.metadata) {
        return SessionError{SessionErrorCode::fingerprint_mismatch, "metadata.json", 0U};
    }
    auto metadata = gateway::kalshi::parse_metadata_snapshot(bytes);
    if (const auto* failure = std::get_if<gateway::kalshi::MetadataError>(&metadata)) {
        return SessionError{SessionErrorCode::invalid_metadata,
                            std::string{gateway::kalshi::to_string(failure->code)}, 0U};
    }
    auto snapshot = std::move(std::get<gateway::kalshi::MetadataSnapshot>(metadata));
    if (snapshot.markets().metadata_version() != manifest.metadata_version) {
        return SessionError{SessionErrorCode::metadata_version_mismatch, "metadata.json", 0U};
    }
    auto journal_hash = detail::fingerprint_file(directory / journal_filename);
    if (const auto* failure = std::get_if<SessionError>(&journal_hash)) { return *failure; }
    if (std::get<ArtifactFingerprint>(journal_hash) != manifest.journal) {
        return SessionError{SessionErrorCode::fingerprint_mismatch, "market.journal", 0U};
    }
    if (const auto failure = detail::validate_journal(
            directory / journal_filename, manifest.metadata_version, manifest.records)) {
        return *failure;
    }
    return VerifiedSession{std::move(manifest), std::move(snapshot)};
}

std::string_view to_string(const SessionErrorCode code) noexcept {
    switch (code) {
    case SessionErrorCode::directory_exists: return "directory_exists";
    case SessionErrorCode::incomplete_session: return "incomplete_session";
    case SessionErrorCode::invalid_artifact: return "invalid_artifact";
    case SessionErrorCode::io_error: return "io_error";
    case SessionErrorCode::input_too_large: return "input_too_large";
    case SessionErrorCode::invalid_manifest: return "invalid_manifest";
    case SessionErrorCode::unsupported_version: return "unsupported_version";
    case SessionErrorCode::fingerprint_mismatch: return "fingerprint_mismatch";
    case SessionErrorCode::invalid_metadata: return "invalid_metadata";
    case SessionErrorCode::metadata_version_mismatch: return "metadata_version_mismatch";
    case SessionErrorCode::invalid_journal: return "invalid_journal";
    case SessionErrorCode::record_count_mismatch: return "record_count_mismatch";
    case SessionErrorCode::writer_closed: return "writer_closed";
    }
    return "unknown_session_error";
}

}  // namespace eme::session
