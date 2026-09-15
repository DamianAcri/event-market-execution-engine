#include "eme/transport/readonly_capture.hpp"
#include "session/study_json.hpp"
#include <iostream>
int main(int argc, char** argv) {
    using namespace eme;
    if (argc != 9) { return 2; }
    auto parsed = gateway::kalshi::parse_metadata_snapshot(session::detail::read_text(argv[1]));
    const auto& metadata = std::get<gateway::kalshi::MetadataSnapshot>(parsed);
    transport::CaptureConfig config;
    config.host = "localhost";
    config.port = argv[2];
    if (std::string_view{argv[8]} != "untrusted") { config.ca_file = argv[3]; }
    if (std::string_view{argv[8]} == "hostname") { config.host = "127.0.0.1"; }
    config.private_key = argv[4];
    config.key_id = "fixture";
    config.directory = argv[5];
    config.markets = {1U, 2U};
    config.duration = std::chrono::milliseconds{std::stoi(argv[6])};
    config.maximum_connections = static_cast<std::size_t>(std::stoul(argv[7]));
    config.idle_timeout = std::chrono::milliseconds{400};
    config.handshake_timeout = std::chrono::milliseconds{500};
    config.retry_delay = std::chrono::milliseconds{50};
    config.synthetic = true;
    if (std::string_view{argv[8]} == "overflow") { config.queue.retained_bytes = 4096U; }
    const auto result = transport::capture_readonly(config, metadata);
    std::cout << session::detail::Json{{"finalized", result.finalized}, {"market_updates", result.market_updates},
        {"connections", result.connections}, {"reason", result.reason}}.dump() << '\n';
    return 0;
}
