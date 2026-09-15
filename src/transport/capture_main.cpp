#include "eme/transport/readonly_capture.hpp"
#include "session/study_json.hpp"
#include <charconv>
#include <cstdlib>
#include <iostream>

int main(const int argc, const char* const argv[]) {
    using namespace eme;
    if (argc != 5 || (std::string_view{argv[4]} != "production" && std::string_view{argv[4]} != "demo")) {
        std::cerr << "Usage: eme-capture <metadata.json> <new-directory> <seconds 1..86400> <production|demo>\n"
                  << "Configure EME_KALSHI_KEY_ID and EME_KALSHI_PRIVATE_KEY_PATH. Read-only market data.\n";
        return 2;
    }
    std::uint32_t seconds{};
    const std::string_view duration{argv[3]};
    const auto parsed = std::from_chars(duration.data(), duration.data() + duration.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != duration.data() + duration.size() || seconds == 0U || seconds > 86400U) { return 2; }
    const auto* key_id = std::getenv("EME_KALSHI_KEY_ID");
    const auto* key_file = std::getenv("EME_KALSHI_PRIVATE_KEY_PATH");
    if (!key_id || !key_file) { std::cerr << "Market-data credentials are not configured.\n"; return 2; }
    try {
        auto parsed_metadata = gateway::kalshi::parse_metadata_snapshot(session::detail::read_text(argv[1]));
        if (!std::holds_alternative<gateway::kalshi::MetadataSnapshot>(parsed_metadata)) { std::cerr << "Invalid metadata.\n"; return 2; }
        const auto& metadata = std::get<gateway::kalshi::MetadataSnapshot>(parsed_metadata);
        transport::CaptureConfig config;
        config.host = std::string_view{argv[4]} == "production" ? "external-api-ws.kalshi.com" : "external-api-ws.demo.kalshi.co";
        config.key_id = key_id;
        config.private_key = key_file;
        config.directory = argv[2];
        config.duration = std::chrono::seconds{seconds};
        const auto root = session::detail::Json::parse(metadata.canonical_json());
        for (const auto& market : root["markets"]) { config.markets.push_back(market.at("id").get<market::MarketId>()); }
        const auto result = transport::capture_readonly(config, metadata);
        std::cout << session::detail::Json{{"finalized", result.finalized}, {"market_updates", result.market_updates},
            {"connections", result.connections}, {"reason", result.reason}}.dump() << '\n';
        return result.finalized && (result.reason == "duration" || result.reason == "operator_stop") ? 0 : 1;
    } catch (...) { std::cerr << "Capture configuration could not be loaded.\n"; return 2; }
}
