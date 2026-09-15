#include "eme/session/async_capture.hpp"
#include "test_support.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
namespace session = eme::session;
namespace journal = eme::journal;
const auto metadata = std::get<eme::gateway::kalshi::MetadataSnapshot>(
    eme::gateway::kalshi::parse_metadata_snapshot(R"({"schema_version":1,"metadata_version":11,"venue":"kalshi",
    "markets":[{"id":42,"ticker":"TEST"}],"constraints":[]})"));
struct Directory final {
    Directory() : path{std::filesystem::temp_directory_path() / ("eme-async-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))} {
        if (!std::filesystem::create_directory(path)) { throw std::runtime_error{"temporary directory"}; }
    }
    ~Directory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    std::filesystem::path path;
};
journal::RawMarketRecord record(const std::uint64_t sequence) {
    journal::RawMarketRecord value;
    value.metadata_version = 11U;
    value.connection_generation = 1U;
    value.sequence = sequence;
    value.received_at = eme::market::ReceiveTime{std::chrono::nanoseconds{sequence}};
    value.channel = "orderbook_delta";
    // Deliberately preserve whitespace and a byte that would change if parsed
    // and reserialized. The writer is an opaque transport/persistence boundary.
    value.payload = " { \"seq\" : " + std::to_string(sequence) + ", \"raw\":\"\\u0041\" }\n";
    return value;
}
std::unique_ptr<session::AsyncCaptureWriter> create(const std::filesystem::path& directory,
    session::CaptureQueueLimits limits = {}) {
    return std::move(std::get<std::unique_ptr<session::AsyncCaptureWriter>>(
        session::create_async_capture(directory, metadata, limits)));
}
std::string bytes(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
}  // namespace

int main() {
    eme::test::Context test;
    Directory directory;
    const auto async_path = directory.path / "async";
    const auto sync_path = directory.path / "sync";
    auto output = create(async_path, {8192U, 8U * 1024U * 1024U});
    auto sync = std::move(std::get<std::unique_ptr<session::SessionWriter>>(session::create_session(sync_path, metadata)));
    for (std::uint64_t i = 1U; i <= 8000U; ++i) {
        auto frame = record(i);
        test.expect(!sync->append(frame), "synchronous reference accepts frame");
        test.expect(output->try_append(std::move(frame)) == session::CaptureAppendResult::queued, "bounded burst transfers frame ownership");
    }
    const auto completed = output->finish();
    const auto reference = sync->finalize();
    test.expect(std::holds_alternative<session::SessionManifest>(completed) &&
        std::get<session::SessionManifest>(completed) == std::get<session::SessionManifest>(reference),
        "background output has identical hashes and record count to synchronous reference");
    test.expect(bytes(async_path / session::journal_filename) == bytes(sync_path / session::journal_filename),
        "every raw byte and input ordering survives asynchronous capture");
    const auto stats = output->stats();
    test.expect(stats.accepted_records == 8000U && stats.written_records == 8000U && stats.retained_bytes_high_water <= 8U * 1024U * 1024U,
        "finish drains accepted records within retained memory budget");
    test.expect(std::get<session::SessionManifest>(output->finish()) == std::get<session::SessionManifest>(completed) &&
        output->try_append(record(8001U)) == session::CaptureAppendResult::closed, "finish is idempotent and closes admission");
    test.expect(std::holds_alternative<session::VerifiedSession>(session::verify_session(async_path)), "published session passes independent verification");

    // Capacity accounts for retained allocations, even after logical shrinking.
    auto bounded = create(directory.path / "bounded", {2U, 1024U});
    auto oversized = record(1U); oversized.payload.reserve(4096U); oversized.payload.resize(1U);
    test.expect(bounded->try_append(std::move(oversized)) == session::CaptureAppendResult::capacity_exceeded && oversized.payload.size() == 1U,
        "oversized retained allocation aborts admission without moving input");
    test.expect(bounded->try_append(record(2U)) == session::CaptureAppendResult::capacity_exceeded,
        "a lost frame permanently stops this capture; no silent drop and resume");
    test.expect(std::holds_alternative<session::SessionError>(bounded->finish()) &&
        !std::filesystem::exists(directory.path / "bounded" / session::manifest_filename), "capacity failure cannot publish a complete manifest");

    auto bad = create(directory.path / "bad");
    auto wrong = record(1U); wrong.metadata_version = 12U;
    test.expect(bad->try_append(std::move(wrong)) == session::CaptureAppendResult::queued, "enqueue is not a persistence guarantee");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (bad->status() == session::CaptureAppendResult::queued && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    test.expect(bad->status() == session::CaptureAppendResult::writer_failed,
        "health polling observes worker failure without requiring another market frame");
    test.expect(std::holds_alternative<session::SessionError>(bad->finish()) &&
        !std::filesystem::exists(directory.path / "bad" / session::manifest_filename), "worker validation failure reaches the joining caller");
    auto tampered = create(directory.path / "tampered");
    test.expect(tampered->try_append(record(1U)) == session::CaptureAppendResult::queued, "enqueue before finalization fault");
    { std::ofstream file{directory.path / "tampered" / session::metadata_filename}; file << "{}"; }
    test.expect(std::holds_alternative<session::SessionError>(tampered->finish()) &&
        !std::filesystem::exists(directory.path / "tampered" / session::manifest_filename), "failed finalization publishes no manifest");
    {
        auto abandoned = create(directory.path / "abandoned");
        test.expect(abandoned->try_append(record(1U)) == session::CaptureAppendResult::queued, "enqueue then abandon");
    }
    test.expect(!std::filesystem::exists(directory.path / "abandoned" / session::manifest_filename), "destruction joins and aborts an unfinished capture");

    // Repeated sleeps, wakeups, ring wraparound and closes catch lost wakeups and
    // publication errors. The record count bound can hold each complete burst.
    for (std::size_t pass = 0U; pass < 32U; ++pass) {
        auto ring = create(directory.path / ("ring-" + std::to_string(pass)), {8U, 8192U});
        for (std::uint64_t i = 1U; i <= 32U; ++i) {
            test.expect(ring->try_append(record(i)) == session::CaptureAppendResult::queued, "ring accepts paced record");
            while (ring->stats().written_records < i) { std::this_thread::yield(); }
        }
        const auto finished = ring->finish();
        test.expect(std::holds_alternative<session::SessionManifest>(finished) &&
            std::get<session::SessionManifest>(finished).records == 32U, "ring drain/close loses no accepted records");
    }
    test.expect(std::holds_alternative<session::SessionError>(session::create_async_capture(directory.path / "invalid", metadata, {0U, 1024U})) &&
        !std::filesystem::exists(directory.path / "invalid"), "invalid queue limits fail before creating artifacts");
    return test.result();
}
