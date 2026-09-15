#include "eme/session/async_capture.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
// C4324 diagnoses the deliberate cache-line padding of Index and its owner.
// Keep /W4 /WX globally; suppress only this expected layout diagnostic here.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

namespace eme::session {
namespace {
using Result = CaptureAppendResult;
// Avoid sharing the producer/consumer publication indices on targets with
// either 64- or 128-byte coherence lines. No target-specific instruction set.
struct alignas(128) Index final { std::atomic<std::size_t> value{}; };
static_assert(sizeof(Index) == 128U && alignof(Index) == 128U);
static_assert(std::atomic<std::size_t>::is_always_lock_free);
}  // namespace

struct AsyncCaptureWriter::Impl final {
    Impl(std::unique_ptr<SessionWriter> writer, const CaptureQueueLimits limits)
        : writer_{std::move(writer)}, limits_{limits}, slots_(limits.records + 1U),
          thread_{[this] { run(); }} {}

    ~Impl() {
        if (thread_.joinable()) {
            fail(Result::closed);
            stop_and_join();
        }
    }
    void fail(const Result reason) noexcept {
        auto expected = Result::queued;
        (void)failure_.compare_exchange_strong(expected, reason, std::memory_order_release, std::memory_order_relaxed);
    }
    void signal() noexcept {
        epoch_.fetch_add(1U, std::memory_order_release);
        epoch_.notify_one();
    }
    void stop_and_join() {
        closing_.store(true, std::memory_order_release);
        signal();
        thread_.join();
    }
    std::size_t next(const std::size_t index) const noexcept {
        return index + 1U == slots_.size() ? 0U : index + 1U;
    }
    Result append(journal::RawMarketRecord&& record) noexcept {
        if (finished_) { return Result::closed; }
        const auto failure = failure_.load(std::memory_order_acquire);
        if (failure != Result::queued) { return failure; }
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto following = next(head);
        // Count retained string CAPACITY, not payload length: a shortened
        // string can still own a large allocation. Bound before adding sizes.
        const auto room = limits_.retained_bytes;
        const auto channel = record.channel.capacity();
        const auto payload = record.payload.capacity();
        const auto consumed = consumed_bytes_.load(std::memory_order_acquire);
        const auto pending = accepted_bytes_ - consumed;
        if (following == tail_.value.load(std::memory_order_acquire) ||
            channel > room || payload > room - channel || sizeof(record) > room - channel - payload ||
            channel + payload + sizeof(record) > room - pending ||
            accepted_bytes_ > std::numeric_limits<std::uint64_t>::max() - room) {
            fail(Result::capacity_exceeded);
            signal();
            return Result::capacity_exceeded;
        }
        const auto charge = channel + payload + sizeof(record);
        slots_[head] = std::move(record);
        accepted_bytes_ += charge;
        ++accepted_records_;
        high_water_ = std::max(high_water_, pending + charge);
        head_.value.store(following, std::memory_order_release);
        signal();
        return Result::queued;
    }
    void run() noexcept {
        try {
            std::uint64_t consumed = 0U;
            std::uint64_t written = 0U;
            for (;;) {
                const auto epoch = epoch_.load(std::memory_order_acquire);
                // Observe closing before the head so the final publication is
                // visible before deciding the queue has drained.
                const auto closing = closing_.load(std::memory_order_acquire);
                if (failure_.load(std::memory_order_acquire) != Result::queued) { return; }
                const auto tail = tail_.value.load(std::memory_order_relaxed);
                if (tail == head_.value.load(std::memory_order_acquire)) {
                    if (closing) { break; }
                    epoch_.wait(epoch, std::memory_order_acquire);
                    continue;
                }
                auto& record = slots_[tail];
                const auto charge = record.channel.capacity() + record.payload.capacity() + sizeof(record);
                if (const auto error = writer_->append(record)) {
                    result_ = *error;
                    fail(Result::writer_failed);
                    return;
                }
                // Release allocations explicitly before advertising free bytes.
                // clear()/assignment need not release a string's capacity.
                std::string{}.swap(record.channel);
                std::string{}.swap(record.payload);
                consumed += charge;
                consumed_bytes_.store(consumed, std::memory_order_release);
                tail_.value.store(next(tail), std::memory_order_release);
                written_records_.store(++written, std::memory_order_release);
            }
            result_ = writer_->finalize();
            if (std::holds_alternative<SessionError>(*result_)) { fail(Result::writer_failed); }
        } catch (...) {
            // No manifest is published by an exception before finalization.
            // Report on the joining caller; never unwind through std::thread.
            fail(Result::writer_failed);
        }
    }
    FinalizeSessionResult finish() {
        if (!finished_) { stop_and_join(); finished_ = true; }
        if (result_) { return *result_; }
        const auto reason = failure_.load(std::memory_order_acquire);
        return SessionError{reason == Result::capacity_exceeded ? SessionErrorCode::input_too_large : SessionErrorCode::io_error,
            reason == Result::capacity_exceeded ? "capture.queue_capacity" : "capture.aborted", accepted_records_};
    }
    std::unique_ptr<SessionWriter> writer_;
    CaptureQueueLimits limits_;
    std::vector<journal::RawMarketRecord> slots_;
    Index head_;
    Index tail_;
    std::atomic<std::uint64_t> epoch_{};
    std::atomic<std::uint64_t> consumed_bytes_{};
    std::atomic<std::uint64_t> written_records_{};
    std::atomic<Result> failure_{Result::queued};
    std::atomic<bool> closing_{};
    std::uint64_t accepted_bytes_{};
    std::uint64_t accepted_records_{};
    std::uint64_t high_water_{};
    bool finished_{};
    std::optional<FinalizeSessionResult> result_; // Worker writes; read after join.
    std::thread thread_; // Last: every shared field is initialized before start.
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

CreateAsyncCaptureResult create_async_capture(const std::filesystem::path& directory,
    const gateway::kalshi::MetadataSnapshot& metadata, const CaptureQueueLimits limits) {
    if (limits.records == 0U || limits.records > 65'536U ||
        limits.retained_bytes < sizeof(journal::RawMarketRecord) + 32U || limits.retained_bytes > 256U * 1024U * 1024U) {
        return SessionError{SessionErrorCode::input_too_large, "capture.queue_limits", 0U};
    }
    auto created = create_session(directory, metadata);
    if (const auto* error = std::get_if<SessionError>(&created)) { return *error; }
    return std::unique_ptr<AsyncCaptureWriter>{new AsyncCaptureWriter{
        std::make_unique<AsyncCaptureWriter::Impl>(std::move(std::get<std::unique_ptr<SessionWriter>>(created)), limits)}};
}
AsyncCaptureWriter::AsyncCaptureWriter(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}
AsyncCaptureWriter::~AsyncCaptureWriter() = default;
CaptureAppendResult AsyncCaptureWriter::try_append(journal::RawMarketRecord&& record) noexcept { return impl_->append(std::move(record)); }
CaptureAppendResult AsyncCaptureWriter::status() const noexcept {
    return impl_->finished_ ? Result::closed : impl_->failure_.load(std::memory_order_acquire);
}
CaptureQueueStats AsyncCaptureWriter::stats() const noexcept {
    return {impl_->accepted_records_, impl_->written_records_.load(std::memory_order_acquire), impl_->high_water_};
}
FinalizeSessionResult AsyncCaptureWriter::finish() { return impl_->finish(); }
}  // namespace eme::session
