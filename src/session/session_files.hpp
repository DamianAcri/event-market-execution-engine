#pragma once

#include "eme/session/capture_session.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace eme::session::detail {

inline constexpr std::size_t maximum_manifest_bytes = 4096U;
inline constexpr std::string_view pending_manifest_filename = "manifest.pending";

[[nodiscard]] std::variant<std::string, SessionError> read_bounded(
    const std::filesystem::path& path, std::size_t maximum);
[[nodiscard]] std::variant<ArtifactFingerprint, SessionError> fingerprint_file(
    const std::filesystem::path& path);
[[nodiscard]] ArtifactFingerprint fingerprint_bytes(std::string_view bytes);
[[nodiscard]] std::optional<SessionError> write_new_file(
    const std::filesystem::path& path, std::string_view bytes);
[[nodiscard]] std::variant<SessionManifest, SessionError> parse_manifest(std::string_view bytes);
[[nodiscard]] std::string serialize_manifest(const SessionManifest& manifest);
[[nodiscard]] std::optional<SessionError> validate_journal(
    const std::filesystem::path& path, market::MetadataVersion version,
    std::uint64_t expected_records);

}  // namespace eme::session::detail
