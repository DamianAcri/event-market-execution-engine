#pragma once

#include "eme/session/async_capture.hpp"
#include <chrono>
#include <vector>

namespace eme::transport {
// Production CLI chooses one of the two venue endpoints. Tests inject loopback
// here, never through a production command-line endpoint override.
struct CaptureConfig final {
    std::string host;
    std::string port{"443"};
    std::string key_id;
    std::filesystem::path private_key;
    std::filesystem::path ca_file;
    std::filesystem::path directory;
    // Empty means capture only. Nonempty enables local simulated orders only.
    std::filesystem::path paper_policy;
    std::vector<market::MarketId> markets;
    std::chrono::milliseconds duration{60'000};
    std::chrono::milliseconds handshake_timeout{10'000};
    std::chrono::milliseconds idle_timeout{30'000};
    std::chrono::milliseconds retry_delay{1'000};
    std::size_t maximum_connections{8U};
    session::CaptureQueueLimits queue;
    bool synthetic{};
    bool public_trades{};
};
struct CaptureResult final {
    bool finalized{};
    std::uint64_t market_updates{};
    std::uint64_t connections{};
    std::string reason;
    std::uint64_t public_trades{};
};
[[nodiscard]] CaptureResult capture_readonly(
    const CaptureConfig&, const gateway::kalshi::MetadataSnapshot&);
} // namespace eme::transport
