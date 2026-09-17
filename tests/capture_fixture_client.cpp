#include "eme/transport/readonly_capture.hpp"
#include "session/study_json.hpp"
#include <iostream>
int main(int argc, char** argv) {
    using namespace eme;
    if ((argc != 9 && argc != 10)) { return 2; }
    auto parsed = gateway::kalshi::parse_metadata_snapshot(session::detail::read_text(argv[1]));
    const auto& metadata = std::get<gateway::kalshi::MetadataSnapshot>(parsed);
    transport::CaptureConfig config;
    // Match the fixture's IPv4 listener; localhost may prefer an absent IPv6
    // listener and exhaust the test deadline before IPv4 fallback on Windows.
    config.host = "127.0.0.1";
    config.port = argv[2];
    config.ca_file = argv[3];
    config.private_key = argv[4];
    config.key_id = "fixture";
    config.directory = argv[5];
    const std::string_view scenario{argv[8]};
    const auto metadata_json = session::detail::Json::parse(metadata.canonical_json());
    for (const auto& market : metadata_json["markets"]) {
        const auto id = market.at("id").get<market::MarketId>();
        if (scenario.starts_with("basket") || id <= 2U) { config.markets.push_back(id); }
    }
    config.duration = std::chrono::milliseconds{std::stoi(argv[6])};
    config.maximum_connections = static_cast<std::size_t>(std::stoul(argv[7]));
    config.idle_timeout = std::chrono::milliseconds{600};
    config.handshake_timeout = std::chrono::milliseconds{1500};
    config.retry_delay = std::chrono::milliseconds{50};
    config.synthetic = true;
    config.public_trades = scenario == "paper_trades" || scenario == "trade_gap" || scenario == "trade_ack_timeout" || scenario == "basket_trades";
    if (argc == 10) {
        if (scenario.starts_with("basket")) { config.basket_policy = argv[9]; }
        else { config.paper_policy = argv[9]; }
    }
    if (scenario == "overflow") { config.queue.retained_bytes = 4096U; }
    const auto result = transport::capture_readonly(config, metadata);
    std::cout << session::detail::Json{{"finalized", result.finalized}, {"market_updates", result.market_updates},
        {"public_trades", result.public_trades}, {"connections", result.connections}, {"reason", result.reason}}.dump() << '\n';
    return 0;
}
