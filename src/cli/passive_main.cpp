#include "eme/session/passive_probe.hpp"
#include "session/study_json.hpp"
#include <iostream>
#include <sstream>
#include <string_view>

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        std::cout << "Usage: eme-passive-probe SESSION_DIRECTORY PROBE_POLICY\n"
                     "       eme-passive-probe --validate METADATA BASKET_POLICY PROBE_POLICY\n"
                     "Offline simulation only; no account access or orders.\n"; return 0;
    }
    if (argc == 5 && std::string_view{argv[1]} == "--validate") {
        try {
            const auto parsed = eme::gateway::kalshi::parse_metadata_snapshot(eme::session::detail::read_text(argv[2]));
            if (!std::holds_alternative<eme::gateway::kalshi::MetadataSnapshot>(parsed)) { throw std::runtime_error("invalid metadata"); }
            std::ostringstream ignored;
            const auto probe = eme::session::make_passive_probe(std::get<eme::gateway::kalshi::MetadataSnapshot>(parsed), argv[3], argv[4], ignored);
            std::cout << "{\"valid\":true,\"orders_sent\":0}\n"; return 0;
        } catch (...) { std::cerr << "Invalid passive study configuration\n"; return 1; }
    }
    if (argc != 3) { std::cerr << "Usage: eme-passive-probe SESSION_DIRECTORY PROBE_POLICY\n"; return 2; }
    const std::filesystem::path root{argv[1]};
    auto loaded = eme::session::load_replay(root, root / "replay.json");
    if (const auto* error = std::get_if<eme::session::ReplayError>(&loaded)) { std::cerr << error->reason << '\n'; return 1; }
    const auto error = eme::session::run_passive_probe(std::get<eme::session::ReplayInput>(loaded), root / "basket-policy.json", argv[2], std::cout);
    if (error) { std::cerr << error->reason << '\n'; return 1; }
    return 0;
}
