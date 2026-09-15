#pragma once

#include "eme/session/capture_session.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <variant>

namespace eme::session {

struct CaptureQueueLimits final {
    std::size_t records{1024U};
    std::size_t retained_bytes{16U * 1024U * 1024U};
};
enum class CaptureAppendResult : std::uint8_t { queued, closed, capacity_exceeded, writer_failed };
struct CaptureQueueStats final {
    std::uint64_t accepted_records{};
    std::uint64_t written_records{};
    std::uint64_t retained_bytes_high_water{};
};

class AsyncCaptureWriter;
using CreateAsyncCaptureResult = std::variant<std::unique_ptr<AsyncCaptureWriter>, SessionError>;

// Starts one writer thread; creates a new session exclusively. No network or
// order submission. Queue limits include in-flight work, not only waiting slots.
[[nodiscard]] CreateAsyncCaptureResult create_async_capture(
    const std::filesystem::path& directory,
    const gateway::kalshi::MetadataSnapshot& metadata,
    CaptureQueueLimits limits = {});

// SINGLE producer: append, stats, finish and destruction belong to one caller.
// The worker exclusively owns journal encoding, I/O and finalization. No caller
// may mutate a record after successful ownership transfer. Enqueue acceptance
// is not persistence; only successful finish publishes a verified manifest.
class AsyncCaptureWriter final {
public:
    ~AsyncCaptureWriter(); // An unfinished capture is aborted, never finalized.
    AsyncCaptureWriter(const AsyncCaptureWriter&) = delete;
    AsyncCaptureWriter& operator=(const AsyncCaptureWriter&) = delete;
    [[nodiscard]] CaptureAppendResult try_append(journal::RawMarketRecord&& record) noexcept;
    // Poll from the producer's health/heartbeat path even when no frames arrive.
    [[nodiscard]] CaptureAppendResult status() const noexcept;
    [[nodiscard]] CaptureQueueStats stats() const noexcept;
    // Drain and join; operator-side blocking is intentional. Idempotent.
    [[nodiscard]] FinalizeSessionResult finish();

private:
    friend CreateAsyncCaptureResult create_async_capture(const std::filesystem::path&,
        const gateway::kalshi::MetadataSnapshot&, CaptureQueueLimits);
    struct Impl;
    explicit AsyncCaptureWriter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
}  // namespace eme::session
