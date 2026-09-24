#include "eme/session/basket_execution.hpp"
#include <iostream>
#include <string_view>
int main(int argc, char **argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: eme-basket-execution SESSION_DIRECTORY STUDY_POLICY\nOffline only: no "
                     "credentials, network or real orders.\n";
        return 0;
    }
    if (argc != 3) {
        std::cerr << "Usage: eme-basket-execution SESSION_DIRECTORY STUDY_POLICY\n";
        return 2;
    }
    const std::filesystem::path root{argv[1]};
    auto input = eme::session::load_replay(root, root / "replay.json");
    if (const auto *e = std::get_if<eme::session::ReplayError>(&input)) {
        std::cerr << e->reason << '\n';
        return 1;
    }
    const auto error = eme::session::run_basket_execution_study(
        std::get<eme::session::ReplayInput>(input), root / "basket-policy.json", argv[2], std::cout);
    if (error) {
        std::cerr << error->reason << '\n';
        return 1;
    }
    return 0;
}
