#include "eme/book/order_book.hpp"

#include <iostream>
#include <string_view>

namespace {

constexpr std::string_view version = "0.1.0-dev";

void print_help() {
    std::cout << "Event Market Execution Engine\n\n"
              << "Usage:\n"
              << "  event-engine status\n"
              << "  event-engine --version\n";
}

void print_status() {
    std::cout << "ENGINE          READY\n"
              << "MILESTONE       v0.1 market-data correctness\n"
              << "CORE            fixed-point + sequenced local book\n"
              << "CONNECTIVITY    DISABLED\n"
              << "EXECUTION       DISABLED\n"
              << "CREDENTIALS     NOT REQUIRED\n";
}

}  // namespace

int main(const int argc, const char* const argv[]) {
    if (argc == 1) {
        print_help();
        return 0;
    }

    const std::string_view command{argv[1]};
    if (command == "status") {
        print_status();
        return 0;
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
