#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <thread>

namespace eme::session::detail {
// Sparse economic events only, never one output line per market tick. A short
// queue lock publishes each completed line; all file I/O belongs to the worker.
// Bounded retained bytes include the line currently being written. Overflow or
// disk failure sets badbit on the producer and must stop the live experiment.
class AsyncJsonl final : public std::streambuf {
public:
    explicit AsyncJsonl(const std::filesystem::path& path, const std::size_t budget = 4U * 1024U * 1024U)
        : file_{path, std::ios::binary | std::ios::out}, budget_{budget}, output_{this} {
        if (!file_) { throw std::runtime_error{"paper output unavailable"}; }
        pending_.reserve(4096U);
        thread_ = std::thread{[this] { run(); }};
    }
    ~AsyncJsonl() override { (void)finish(); }
    AsyncJsonl(const AsyncJsonl&) = delete;
    AsyncJsonl& operator=(const AsyncJsonl&) = delete;
    std::ostream& stream() { return output_; }
    bool healthy() const { return !failed_.load(std::memory_order_acquire) && static_cast<bool>(output_); }
    bool finish() {
        if (thread_.joinable()) {
            if (!pending_.empty()) { failed_.store(true, std::memory_order_release); }
            { std::lock_guard lock{mutex_}; closing_ = true; }
            ready_.notify_one();
            thread_.join();
        }
        return healthy();
    }
protected:
    std::streamsize xsputn(const char* bytes, const std::streamsize count) override {
        if (!healthy()) { return 0; }
        for (std::streamsize i = 0; i < count; ++i) {
            if (pending_.size() >= 256U * 1024U) { failed_.store(true, std::memory_order_release); return i; }
            pending_.push_back(bytes[i]);
            if (bytes[i] == '\n' && !publish()) { return i; }
        }
        return count;
    }
    int_type overflow(const int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) { return traits_type::not_eof(value); }
        const auto byte = traits_type::to_char_type(value);
        return xsputn(&byte, 1) == 1 ? value : traits_type::eof();
    }
private:
    bool publish() {
        {
            std::lock_guard lock{mutex_};
            const auto charge = pending_.capacity() + sizeof(std::string);
            if (closing_ || failed_.load(std::memory_order_acquire) || charge > budget_ || retained_ > budget_ - charge) {
                failed_.store(true, std::memory_order_release); return false;
            }
            retained_ += charge;
            queue_.push_back(std::move(pending_));
        }
        pending_ = std::string{};
        ready_.notify_one();
        return true;
    }
    void run() noexcept {
        try {
            for (;;) {
                std::string line;
                {
                    std::unique_lock lock{mutex_};
                    ready_.wait(lock, [this] { return closing_ || !queue_.empty(); });
                    if (queue_.empty()) { break; }
                    line = std::move(queue_.front()); queue_.pop_front();
                }
                const auto charge = line.capacity() + sizeof(std::string);
                file_.write(line.data(), static_cast<std::streamsize>(line.size()));
                // Sparse intents/status are visible to the operator immediately.
                file_.flush();
                if (!file_) { failed_.store(true, std::memory_order_release); return; }
                std::string{}.swap(line);
                { std::lock_guard lock{mutex_}; retained_ -= charge; }
            }
            file_.close();
            if (!file_) { failed_.store(true, std::memory_order_release); }
        } catch (...) { failed_.store(true, std::memory_order_release); }
    }
    std::ofstream file_;
    const std::size_t budget_;
    std::ostream output_;
    std::string pending_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::string> queue_;
    std::size_t retained_{};
    bool closing_{};
    std::atomic<bool> failed_{};
    std::thread thread_;
};
} // namespace eme::session::detail
