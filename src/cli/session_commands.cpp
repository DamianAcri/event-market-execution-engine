#include "session_commands.hpp"

#include "eme/session/capture_session.hpp"
#include "eme/session/execution_study.hpp"
#include "session/session_files.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace eme::cli {
namespace {

int failed(const session::SessionError& error) {
    std::cerr << "Session verification failed: " << session::to_string(error.code)
              << " at " << error.field << " record " << error.record_index << '\n';
    return 1;
}

int summary(const session::SessionManifest& manifest) {
    std::cout << "SESSION         VERIFIED\n"
              << "METADATA        " << manifest.metadata_version << '\n'
              << "RECORDS         " << manifest.records << '\n'
              << "JOURNAL_BYTES   " << manifest.journal.bytes << '\n'
              << "METADATA_SHA256 " << manifest.metadata.sha256 << '\n'
              << "JOURNAL_SHA256  " << manifest.journal.sha256 << '\n';
    return std::cout ? 0 : 1;
}

int pack(const std::string_view metadata_path, const std::string_view journal_path,
         const std::string_view directory) {
    namespace kalshi = gateway::kalshi;
    auto bytes = session::detail::read_bounded(metadata_path, kalshi::maximum_metadata_bytes);
    if (const auto* error = std::get_if<session::SessionError>(&bytes)) { return failed(*error); }
    auto metadata = kalshi::parse_metadata_snapshot(std::get<std::string>(bytes));
    if (const auto* error = std::get_if<kalshi::MetadataError>(&metadata)) {
        std::cerr << "Metadata verification failed: " << kalshi::to_string(error->code)
                  << " at " << error->field << '\n';
        return 1;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(journal_path, ec)) || ec) {
        return failed({session::SessionErrorCode::invalid_artifact, "source journal", 0U});
    }
    auto opened = journal::open_raw_journal_reader(journal_path);
    if (const auto* error = std::get_if<journal::JournalError>(&opened)) {
        return failed({session::SessionErrorCode::invalid_journal,
                       std::string{journal::to_string(error->code)}, error->record_index});
    }
    auto created = session::create_session(directory, std::get<kalshi::MetadataSnapshot>(metadata));
    if (const auto* error = std::get_if<session::SessionError>(&created)) { return failed(*error); }
    auto& reader = *std::get<std::unique_ptr<journal::RawJournalReader>>(opened);
    auto& writer = *std::get<std::unique_ptr<session::SessionWriter>>(created);
    while (true) {
        auto next = reader.read_next();
        if (std::holds_alternative<journal::EndOfJournal>(next)) { break; }
        if (const auto* error = std::get_if<journal::JournalError>(&next)) {
            return failed({session::SessionErrorCode::invalid_journal,
                           std::string{journal::to_string(error->code)}, error->record_index});
        }
        if (const auto error = writer.append(std::get<journal::RawMarketRecord>(next))) {
            return failed(*error);
        }
    }
    const auto finalized = writer.finalize();
    if (const auto* error = std::get_if<session::SessionError>(&finalized)) { return failed(*error); }
    return summary(std::get<session::SessionManifest>(finalized));
}

}  // namespace

int run_session_command(const int argc, const char* const argv[]) {
    const auto replay_failed = [](const session::ReplayError& error) {
        std::cerr << "Replay/study failed: " << error.reason << " at record " << error.record_index << '\n';
        return 1;
    };
    if (argc == 6 && std::string_view{argv[2]} == "import") {
        if (const auto failure = session::import_capture(argv[3], argv[4], argv[5])) { return replay_failed(*failure); }
        std::cout << "CAPTURE         IMPORTED AND REPLAY VERIFIED\n";
        return std::cout ? 0 : 1;
    }
    if ((argc == 5 && std::string_view{argv[2]} == "replay") ||
        (argc == 6 && std::string_view{argv[2]} == "study")) {
        auto loaded = session::load_replay(argv[3], argv[4]);
        if (const auto* failure = std::get_if<session::ReplayError>(&loaded)) { return replay_failed(*failure); }
        const auto& input = std::get<session::ReplayInput>(loaded);
        if (argc == 6) {
            if (const auto failure = session::run_execution_study(input, argv[5], std::cout)) { return replay_failed(*failure); }
        } else {
            session::ReplayObserver observer;
            const auto result = session::replay(input, observer, &std::cout);
            if (const auto* failure = std::get_if<session::ReplayError>(&result)) { return replay_failed(*failure); }
        }
        return 0;
    }
    if (argc == 6 && std::string_view{argv[2]} == "pack") {
        return pack(argv[3], argv[4], argv[5]);
    }
    if (argc == 4 && std::string_view{argv[2]} == "verify") {
        const auto verified = session::verify_session(argv[3]);
        if (const auto* error = std::get_if<session::SessionError>(&verified)) { return failed(*error); }
        return summary(std::get<session::VerifiedSession>(verified).manifest);
    }
    std::cerr << "Usage: event-engine session pack <metadata> <journal> <new-directory>\n"
              << "       event-engine session verify <directory>\n";
    std::cerr << "       event-engine session import <metadata> <capture.json> <new-directory>\n"
              << "       event-engine session replay <directory> <replay.json>\n"
              << "       event-engine session study <directory> <replay.json> <policy.json>\n";
    return 2;
}

}  // namespace eme::cli
