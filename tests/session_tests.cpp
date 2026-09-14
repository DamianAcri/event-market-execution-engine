#include "eme/session/capture_session.hpp"
#include "session/session_files.hpp"
#include "test_support.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

namespace {
namespace fs = std::filesystem;
namespace session = eme::session;
namespace kalshi = eme::gateway::kalshi;
using Json = nlohmann::json;

constexpr auto metadata_text = R"({"schema_version":1,"metadata_version":11,"venue":"kalshi",
"markets":[{"id":42,"ticker":"TEST-MARKET"}],"constraints":[]})";

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("eme-session-" + std::to_string(now));
        if (!fs::create_directory(path)) { throw std::runtime_error{"temporary directory collision"}; }
    }
    ~TemporaryDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    fs::path path;
};

kalshi::MetadataSnapshot metadata() {
    return std::move(std::get<kalshi::MetadataSnapshot>(kalshi::parse_metadata_snapshot(metadata_text)));
}

eme::journal::RawMarketRecord record(const std::uint64_t sequence = 10U) {
    return {eme::journal::current_schema_version, 11U, 1U, {}, {}, sequence, std::nullopt,
            "orderbook_snapshot", "{\"type\":\"orderbook_snapshot\",\"sid\":2,\"seq\":" +
            std::to_string(sequence) + ",\"msg\":{\"market_ticker\":\"TEST-MARKET\","
            "\"yes_dollars_fp\":[[\"0.4000\",\"1.00\"]],\"no_dollars_fp\":[]}}"};
}

std::unique_ptr<session::SessionWriter> writer(const fs::path& directory) {
    return std::move(std::get<std::unique_ptr<session::SessionWriter>>(
        session::create_session(directory, metadata())));
}

session::SessionManifest complete(const fs::path& directory) {
    auto output = writer(directory);
    if (output->append(record(10U)) || output->append(record(11U))) {
        throw std::runtime_error{"fixture append failed"};
    }
    return std::get<session::SessionManifest>(output->finalize());
}

std::string read(const fs::path& file) {
    std::ifstream input{file, std::ios::binary};
    if (!input) { throw std::runtime_error{"fixture read failed"}; }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write(const fs::path& file, const std::string& bytes) {
    std::ofstream output{file, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) { throw std::runtime_error{"fixture write failed"}; }
}

void expect_error(eme::test::Context& test, const session::VerifySessionResult& result,
                  const session::SessionErrorCode expected, const std::string_view message) {
    const auto* failure = std::get_if<session::SessionError>(&result);
    test.expect(failure != nullptr && failure->code == expected, message);
}

void lifecycle(eme::test::Context& test) {
    TemporaryDirectory temp;
    const auto directory = temp.path / "capture";
    auto output = writer(directory);
    expect_error(test, session::verify_session(directory), session::SessionErrorCode::incomplete_session,
                 "an open writer has no completed session");
    auto wrong = record(); wrong.metadata_version = 12U;
    const auto mismatch = output->append(wrong);
    test.expect(mismatch && mismatch->code == session::SessionErrorCode::metadata_version_mismatch,
                "wrong metadata is rejected before journal mutation");
    auto invalid = record(); invalid.schema_version = 1U;
    test.expect(output->append(invalid).has_value(), "unsupported record rejected");
    test.expect(!output->append(record(10U)) && !output->append(record(11U)), "valid records appended");
    const auto sealed = output->finalize();
    const auto* manifest = std::get_if<session::SessionManifest>(&sealed);
    test.expect(manifest && manifest->records == 2U, "only accepted records counted");
    const auto verified = session::verify_session(directory);
    const auto* loaded = std::get_if<session::VerifiedSession>(&verified);
    test.expect(loaded && manifest && loaded->manifest == *manifest &&
                loaded->metadata.markets().find("TEST-MARKET") == 42U,
                "verified session owns the exact reviewed registry");
    test.expect(output->append(record()).has_value() &&
                std::holds_alternative<session::SessionError>(output->finalize()),
                "a finalized writer cannot append or finalize again");
    const auto before = read(directory / session::manifest_filename);
    test.expect(std::holds_alternative<session::SessionError>(session::create_session(directory, metadata())),
                "existing session cannot be overwritten");
    test.expect(read(directory / session::manifest_filename) == before, "existing manifest unchanged");
    const auto other = complete(temp.path / "same-data");
    test.expect(manifest && other == *manifest && read(temp.path / "same-data" / session::manifest_filename) == before,
                "same inputs produce identical manifest bytes regardless of path");
    test.expect(!fs::exists(directory / session::detail::pending_manifest_filename),
                "successful publication consumes the pending manifest");

    { auto abandoned = writer(temp.path / "abandoned"); (void)abandoned->append(record()); }
    expect_error(test, session::verify_session(temp.path / "abandoned"),
                 session::SessionErrorCode::incomplete_session, "destruction does not finalize");
    auto empty = writer(temp.path / "empty");
    const auto empty_final = empty->finalize();
    test.expect(std::holds_alternative<session::SessionManifest>(empty_final) &&
                std::get<session::SessionManifest>(empty_final).records == 0U &&
                std::holds_alternative<session::VerifiedSession>(session::verify_session(temp.path / "empty")),
                "explicitly finalized empty journal is valid");
}

void artifact_integrity(eme::test::Context& test) {
    TemporaryDirectory temp;
    const auto dir = temp.path / "capture";
    const auto manifest = complete(dir);
    const auto original = read(dir / session::journal_filename);
    const auto meta = read(dir / session::metadata_filename);
    // Whole-frame loss passes the raw journal reader, but not the finalized session.
    auto only_one = record(10U);
    const auto first_path = temp.path / "first.journal";
    {
        auto opened = eme::journal::open_raw_journal_writer(first_path);
        auto& out = *std::get<std::unique_ptr<eme::journal::RawJournalWriter>>(opened);
        (void)out.append(only_one); (void)out.flush();
    }
    write(dir / session::journal_filename, read(first_path));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::fingerprint_mismatch,
                 "whole final record removal detected");
    write(dir / session::journal_filename, original + original.substr(12U));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::fingerprint_mismatch,
                 "appended valid records detected");
    auto altered = original; altered[altered.size() / 2U] ^= 1;
    write(dir / session::journal_filename, altered);
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::fingerprint_mismatch,
                 "same-size byte alteration detected");
    auto forged_crc = manifest;
    forged_crc.journal = session::detail::fingerprint_bytes(altered);
    write(dir / session::manifest_filename, session::detail::serialize_manifest(forged_crc));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::invalid_journal,
                 "record CRC is still verified when an outer fingerprint matches");
    write(dir / session::manifest_filename, session::detail::serialize_manifest(manifest));
    write(dir / session::journal_filename, original);
    auto changed_meta = meta;
    changed_meta.replace(changed_meta.find("TEST-MARKET"), 11U, "OTHER-MARKT");
    write(dir / session::metadata_filename, changed_meta);
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::fingerprint_mismatch,
                 "different metadata with the same version is rejected");
    write(dir / session::metadata_filename, meta);
    write(dir / session::metadata_filename, "{}");
    auto forged_metadata = manifest;
    forged_metadata.metadata = session::detail::fingerprint_bytes("{}");
    write(dir / session::manifest_filename, session::detail::serialize_manifest(forged_metadata));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::invalid_metadata,
                 "metadata schema still checked when its fingerprint matches");
    write(dir / session::metadata_filename, meta);
    auto forged = manifest; forged.records = 1U;
    write(dir / session::manifest_filename, session::detail::serialize_manifest(forged));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::record_count_mismatch,
                 "count is independently checked against decoded records");
    forged = manifest; forged.metadata_version = 12U;
    write(dir / session::manifest_filename, session::detail::serialize_manifest(forged));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::metadata_version_mismatch,
                 "manifest and metadata version must agree");
    const auto wrong_journal = temp.path / "wrong-version.journal";
    {
        auto opened = eme::journal::open_raw_journal_writer(wrong_journal);
        auto& out = *std::get<std::unique_ptr<eme::journal::RawJournalWriter>>(opened);
        auto wrong = record(); wrong.metadata_version = 12U;
        (void)out.append(wrong); (void)out.flush();
    }
    write(dir / session::journal_filename, read(wrong_journal));
    forged = manifest; forged.records = 1U;
    forged.journal = session::detail::fingerprint_bytes(read(wrong_journal));
    write(dir / session::manifest_filename, session::detail::serialize_manifest(forged));
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::metadata_version_mismatch,
                 "each record must use the session metadata version");
    write(dir / session::journal_filename, original);
    write(dir / session::manifest_filename, session::detail::serialize_manifest(manifest));
    fs::remove(dir / session::metadata_filename);
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::invalid_artifact,
                 "missing metadata rejected");
    fs::create_directory(dir / session::metadata_filename);
    expect_error(test, session::verify_session(dir), session::SessionErrorCode::invalid_artifact,
                 "directories cannot stand in for regular artifacts");
}

void manifest_validation(eme::test::Context& test) {
    TemporaryDirectory temp;
    const auto dir = temp.path / "capture";
    (void)complete(dir);
    const auto original = read(dir / session::manifest_filename);
    auto check = [&](const std::string& bytes) {
        write(dir / session::manifest_filename, bytes);
        test.expect(std::holds_alternative<session::SessionError>(session::verify_session(dir)),
                    "invalid manifest rejected before use");
    };
    check("{"); check(original + "false"); check("[]");
    check("{\"schema_version\":1," + original.substr(1U));
    check("{\"nested\":[[[[[[[[]]]]]]]]}");
    check(std::string(session::detail::maximum_manifest_bytes + 1U, ' '));
    const auto root = Json::parse(original);
    for (const auto key : {"schema_version", "journal_schema_version", "metadata_version", "records"}) {
        for (const auto& value : {Json(-1), Json(1.5), Json("1"), Json(true), Json(nullptr)}) {
            auto changed = root; changed[key] = value; check(changed.dump());
        }
    }
    for (const auto field : {"schema_version", "metadata_version", "journal", "metadata", "records"}) {
        auto changed = root; changed.erase(field); check(changed.dump());
    }
    { auto changed = root; changed["path"] = "../escape"; check(changed.dump()); }
    { auto changed = root; changed["schema_version"] = 2U; check(changed.dump()); }
    { auto changed = root; changed["journal_schema_version"] = 1U; check(changed.dump()); }
    { auto changed = root; changed["metadata_version"] = 0U; check(changed.dump()); }
    { auto changed = root; changed["state"] = "open"; check(changed.dump()); }
    { auto changed = root; changed["venue"] = "other"; check(changed.dump()); }
    for (const auto hash : {"", "ABC", "../metadata.json"}) {
        auto changed = root; changed["metadata"]["sha256"] = hash; check(changed.dump());
    }
    { auto changed = root; changed["metadata"]["sha256"] = std::string(64U, 'A'); check(changed.dump()); }
    { auto changed = root; changed["journal"]["bytes"] = 11U; check(changed.dump()); }
    { auto changed = root; changed["records"] = UINT64_MAX; check(changed.dump()); }
    { auto changed = root; changed["metadata"]["bytes"] = kalshi::maximum_metadata_bytes + 1U; check(changed.dump()); }
    for (std::size_t end = 0U; end < original.size() - 1U; ++end) { check(original.substr(0U, end)); }
}

void finalization_failures(eme::test::Context& test) {
    TemporaryDirectory temp;
    auto output = writer(temp.path / "changed");
    (void)output->append(record());
    write(temp.path / "changed" / session::metadata_filename, "{}");
    const auto failed = output->finalize();
    const auto* error = std::get_if<session::SessionError>(&failed);
    test.expect(error && error->code == session::SessionErrorCode::fingerprint_mismatch,
                "finalization checks the metadata originally captured");
    expect_error(test, session::verify_session(temp.path / "changed"),
                 session::SessionErrorCode::incomplete_session, "failed finalization is not complete");
    test.expect(std::holds_alternative<session::SessionError>(output->finalize()),
                "failed finalization cannot be retried implicitly");
    auto blocked = writer(temp.path / "blocked");
    fs::create_directory(temp.path / "blocked" / session::detail::pending_manifest_filename);
    test.expect(std::holds_alternative<session::SessionError>(blocked->finalize()),
                "pending manifest collision refuses publication");
    expect_error(test, session::verify_session(temp.path / "blocked"),
                 session::SessionErrorCode::incomplete_session, "pending data never substitutes for manifest");
    auto existing = writer(temp.path / "existing-manifest");
    write(temp.path / "existing-manifest" / session::manifest_filename, "do not overwrite");
    test.expect(std::holds_alternative<session::SessionError>(existing->finalize()) &&
                read(temp.path / "existing-manifest" / session::manifest_filename) == "do not overwrite",
                "publication cannot overwrite an existing manifest");
    const auto large_dir = temp.path / "oversized";
    (void)complete(large_dir);
    write(large_dir / session::metadata_filename, std::string(kalshi::maximum_metadata_bytes + 1U, ' '));
    expect_error(test, session::verify_session(large_dir), session::SessionErrorCode::input_too_large,
                 "metadata file reads obey the byte bound");
    std::error_code ec;
    fs::create_directory_symlink(large_dir, temp.path / "linked-directory", ec);
    if (!ec) {
        expect_error(test, session::verify_session(temp.path / "linked-directory"),
                     session::SessionErrorCode::invalid_artifact, "session root cannot be a symlink");
    }
    const auto linked_dir = temp.path / "linked-file";
    (void)complete(linked_dir);
    fs::remove(linked_dir / session::metadata_filename);
    ec.clear();
    fs::create_symlink(large_dir / session::metadata_filename, linked_dir / session::metadata_filename, ec);
    if (!ec) {
        expect_error(test, session::verify_session(linked_dir), session::SessionErrorCode::invalid_artifact,
                     "artifact cannot be a symlink");
    }
}

void hashes(eme::test::Context& test) {
    test.expect(session::detail::fingerprint_bytes("").sha256 ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "SHA256 empty known vector");
    test.expect(session::detail::fingerprint_bytes("abc").sha256 ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "SHA256 abc known vector");
    TemporaryDirectory temp;
    const std::string million(1'000'000U, 'a');
    write(temp.path / "hash", million);
    const auto fingerprint = session::detail::fingerprint_file(temp.path / "hash");
    const auto* value = std::get_if<session::ArtifactFingerprint>(&fingerprint);
    test.expect(value && value->bytes == 1'000'000U && value->sha256 ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "streaming SHA256 crosses buffer boundaries with a known vector");
}

int fixture(const fs::path& directory) {
    fs::create_directories(directory);
    write(directory / "source.json", metadata_text);
    auto opened = eme::journal::open_raw_journal_writer(directory / "source.journal");
    auto& out = *std::get<std::unique_ptr<eme::journal::RawJournalWriter>>(opened);
    return out.append(record()) || out.append(record(11U)) || out.flush() ? 1 : 0;
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    try {
        if (argc == 3 && std::string_view{argv[1]} == "--fixture") { return fixture(argv[2]); }
        eme::test::Context test;
        lifecycle(test); artifact_integrity(test); manifest_validation(test);
        finalization_failures(test); hashes(test);
        return test.result();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
