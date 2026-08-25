#include "cli/commands.hpp"

#include "eme/journal/raw_journal.hpp"
#include "eme/version.hpp"

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
