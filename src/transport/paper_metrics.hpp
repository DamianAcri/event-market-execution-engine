#pragma once
#include "session/study_json.hpp"
#include <array>
#include <bit>

namespace eme::transport {
// Fixed storage, no retained per-tick samples. Percentiles are upper bounds
// from power-of-two nanosecond buckets, not fabricated exact measurements.
struct PaperLatency final {
    std::array<std::uint64_t, 64U> buckets{};
    std::uint64_t count{}, maximum{};
    void observe(const std::int64_t ns) {
        const auto value = static_cast<std::uint64_t>(std::max<std::int64_t>(0, ns));
        const auto bucket = static_cast<std::size_t>(std::bit_width(value));
        ++buckets[std::min(bucket, buckets.size() - 1U)]; ++count;
        maximum = std::max(maximum, value);
    }
    session::detail::Json json() const {
        const auto percentile = [&](const std::uint64_t percent) {
            std::uint64_t cumulative{};
            for (std::size_t i = 0; i < buckets.size(); ++i) {
                cumulative += buckets[i];
                if (count != 0U && cumulative >= (count * percent + 99U) / 100U) { return (std::uint64_t{1U} << i) - 1U; }
            }
            return std::uint64_t{};
        };
        return {{"samples", count}, {"p50_upper_ns", percentile(50U)}, {"p99_upper_ns", percentile(99U)}, {"max_ns", maximum}};
    }
};
} // namespace eme::transport
