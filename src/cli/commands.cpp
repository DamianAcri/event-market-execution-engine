#include "cli/commands.hpp"

#include "eme/journal/raw_journal.hpp"
#include "eme/version.hpp"

#ifdef EME_CLI_KALSHI
#include "cli/session_commands.hpp"
#include "cli/market_screen.hpp"
#include "eme/gateway/kalshi/metadata_snapshot.hpp"
#include <fstream>
#include <string>
#endif

#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

namespace eme::cli {
namespace {

void print_help() {
    std::cout << "Event Market Execution Engine\n\n"
              << "Usage:\n"
              << "  event-engine status\n"
              << "  event-engine journal verify <path>\n"
#ifdef EME_CLI_KALSHI
              << "  event-engine market screen <metadata.json> <screen.json>\n"
              << "  event-engine metadata verify <path>\n"
              << "  event-engine metadata canonical <path>\n"
              << "  event-engine session pack <metadata> <journal> <new-directory>\n"
              << "  event-engine session verify <directory>\n"
              << "  event-engine session import <metadata> <capture.json> <new-directory>\n"
              << "  event-engine session replay <directory> <replay.json>\n"
              << "  event-engine session study <directory> <replay.json> <policy.json>\n"
#endif
              << "  event-engine --version\n";
}

void print_status() {
    std::cout << "ENGINE          READY\n"
              << "MILESTONE       v0.2 deterministic constraint core\n"
              << "CORE            fixed-point + generation-aware multi-market books\n"
              << "JOURNAL         CHECKSUMMED RAW + DETERMINISTIC REPLAY\n"
              << "CONSTRAINTS     VERSIONED REGISTRY + PAYOFF ORACLE\n"
              << "CONNECTIVITY    DISABLED\n"
              << "EXECUTION       DISABLED\n"
              << "CREDENTIALS     NOT REQUIRED\n";
}

#ifdef EME_CLI_KALSHI
[[nodiscard]] int inspect_metadata(const std::string_view path, const bool canonical) {
    namespace kalshi = gateway::kalshi;
    std::ifstream input{std::string{path}, std::ios::binary};
    if (!input) {
        std::cerr << "Metadata read failed\n";
        return 1;
    }
    // Bound the read itself, including non-seekable or concurrently growing files.
    std::string bytes(kalshi::maximum_metadata_bytes + 1U, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = input.gcount();
    if (input.bad() || (input.fail() && !input.eof())) {
        std::cerr << "Metadata read failed\n";
        return 1;
    }
    bytes.resize(static_cast<std::size_t>(count));
    const auto result = kalshi::parse_metadata_snapshot(bytes);
    if (const auto* error = std::get_if<kalshi::MetadataError>(&result)) {
        std::cerr << "Metadata verification failed: " << kalshi::to_string(error->code)
                  << " at " << error->field << '\n';
        return 1;
    }
    const auto& snapshot = std::get<kalshi::MetadataSnapshot>(result);
    if (canonical) {
        std::cout << snapshot.canonical_json() << '\n';
    } else {
        std::cout << "METADATA        VALID\n"
                  << "VERSION         " << snapshot.markets().metadata_version() << '\n'
                  << "MARKETS         " << snapshot.markets().size() << '\n'
                  << "CONSTRAINTS     " << snapshot.constraints().size() << '\n';
    }
    return std::cout ? 0 : 1;
}
#endif

[[nodiscard]] int verify_journal(const std::string_view path) {
    auto open = journal::open_raw_journal_reader(path);
    const auto* open_error = std::get_if<journal::JournalError>(&open);
    if (open_error != nullptr) {
        std::cerr << "Journal verification failed: "
                  << journal::to_string(open_error->code) << '\n';
        return 1;
    }
    auto reader = std::move(
        std::get<std::unique_ptr<journal::RawJournalReader>>(open));

    std::uint64_t records = 0U;
    std::uint64_t generations = 0U;
    std::optional<market::ConnectionGeneration> previous_generation;
    std::optional<book::SequenceNumber> first_sequence;
    std::optional<book::SequenceNumber> last_sequence;
    while (true) {
        const auto next = reader->read_next();
        if (std::holds_alternative<journal::EndOfJournal>(next)) {
            break;
        }
        if (const auto* error = std::get_if<journal::JournalError>(&next);
            error != nullptr) {
            std::cerr << "Journal verification failed: "
                      << journal::to_string(error->code)
                      << " at record " << error->record_index << '\n';
            return 1;
        }

        const auto& record = std::get<journal::RawMarketRecord>(next);
        if (!previous_generation.has_value() ||
            *previous_generation != record.connection_generation) {
            ++generations;
            previous_generation = record.connection_generation;
        }
        if (!first_sequence.has_value()) {
            first_sequence = record.sequence;
        }
        last_sequence = record.sequence;
        ++records;
    }

    std::cout << "JOURNAL         VALID\n"
              << "RECORDS         " << records << '\n'
              << "GENERATIONS     " << generations << '\n';
    if (first_sequence.has_value()) {
        std::cout << "FIRST_SEQUENCE  " << *first_sequence << '\n'
                  << "LAST_SEQUENCE   " << *last_sequence << '\n';
    }
    return 0;
}

}  // namespace

int run(const int argc, const char* const argv[]) {
    if (argc == 1) {
        print_help();
        return 0;
    }

    const std::string_view command{argv[1]};
    if (command == "status") {
        print_status();
        return 0;
    }
    if (command == "journal" && argc == 4 && std::string_view{argv[2]} == "verify") {
        return verify_journal(argv[3]);
    }
#ifdef EME_CLI_KALSHI
    if (command == "session") { return run_session_command(argc, argv); }
    if (command == "market") { return run_market_screen_command(argc, argv); }
    if (command == "metadata" && argc == 4) {
        const std::string_view action{argv[2]};
        if (action == "verify" || action == "canonical") {
            return inspect_metadata(argv[3], action == "canonical");
        }
    }
#endif
    if (command == "--version" || command == "version") {
        std::cout << "event-engine " << version << '\n';
        return 0;
    }
    if (command == "--help" || command == "-h" || command == "help") {
        print_help();
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n\n";
    print_help();
    return 2;
}

}  // namespace eme::cli
