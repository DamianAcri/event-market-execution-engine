#pragma once

#include "eme/gateway/kalshi/metadata_snapshot.hpp"
#include "eme/journal/raw_journal.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace eme::session {

inline constexpr std::string_view manifest_filename = "manifest.json";
inline constexpr std::string_view metadata_filename = "metadata.json";
inline constexpr std::string_view journal_filename = "market.journal";

struct ArtifactFingerprint final {
    std::uint64_t bytes{};
    std::string sha256;
    friend bool operator==(const ArtifactFingerprint&, const ArtifactFingerprint&) = default;
};

struct SessionManifest final {
    market::MetadataVersion metadata_version{};
    ArtifactFingerprint metadata;
    ArtifactFingerprint journal;
    std::uint64_t records{};
    friend bool operator==(const SessionManifest&, const SessionManifest&) = default;
};

enum class SessionErrorCode : std::uint8_t {
    directory_exists,
    incomplete_session,
    invalid_artifact,
    io_error,
    input_too_large,
    invalid_manifest,
    unsupported_version,
    fingerprint_mismatch,
    invalid_metadata,
    metadata_version_mismatch,
    invalid_journal,
    record_count_mismatch,
    writer_closed,
};

struct SessionError final {
    SessionErrorCode code{};
    std::string field;
    std::uint64_t record_index{};
};

[[nodiscard]] std::string_view to_string(SessionErrorCode code) noexcept;

struct VerifiedSession final {
    SessionManifest manifest;
    gateway::kalshi::MetadataSnapshot metadata;
};

using VerifySessionResult = std::variant<VerifiedSession, SessionError>;
using FinalizeSessionResult = std::variant<SessionManifest, SessionError>;

// Offline integrity verification, not validation of market payload semantics.
// The caller must keep finalized files unchanged throughout verification/use.
[[nodiscard]] VerifySessionResult verify_session(const std::filesystem::path& directory);

class SessionWriter;
using CreateSessionResult = std::variant<std::unique_ptr<SessionWriter>, SessionError>;

// Exclusively creates a NEW directory. Failures leave an incomplete directory
// for inspection; destruction never publishes a completion manifest.
[[nodiscard]] CreateSessionResult create_session(
    const std::filesystem::path& directory,
    const gateway::kalshi::MetadataSnapshot& metadata);

class SessionWriter final {
public:
    SessionWriter(const SessionWriter&) = delete;
    SessionWriter& operator=(const SessionWriter&) = delete;

    [[nodiscard]] std::optional<SessionError> append(const journal::RawMarketRecord& record);
    // One attempt only: flush/close, verify artifacts, then publish the manifest.
    // Stream flush + rename is not a power-loss durability guarantee (no fsync).
    [[nodiscard]] FinalizeSessionResult finalize();

private:
    friend CreateSessionResult create_session(
        const std::filesystem::path&, const gateway::kalshi::MetadataSnapshot&);
    SessionWriter(std::filesystem::path directory, SessionManifest manifest,
                  std::unique_ptr<journal::RawJournalWriter> writer);

    std::filesystem::path directory_;
    SessionManifest manifest_;
    std::unique_ptr<journal::RawJournalWriter> writer_;
    bool closed_{};
};

}  // namespace eme::session
