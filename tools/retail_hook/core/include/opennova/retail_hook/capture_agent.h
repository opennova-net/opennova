#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace opennova::retail_hook {

struct CaptureAgentStats {
    std::uint64_t accepted{};
    std::uint64_t dropped_full{};
    std::uint64_t dropped_contended{};
};

// A bounded producer/consumer handoff for capture boundaries. Neither producer
// nor consumer waits for the other: a producer drops immediately when the
// short queue gate is contended or the ring is full. Record movement under the
// gate must therefore be non-throwing.
template <typename Record, std::size_t Capacity>
class CaptureAgent {
    static_assert(Capacity > 0, "a capture ring must have at least one slot");
    static_assert(std::is_default_constructible_v<Record>);
    static_assert(std::is_nothrow_move_assignable_v<Record>);

public:
    CaptureAgent() = default;
    CaptureAgent(const CaptureAgent&) = delete;
    CaptureAgent& operator=(const CaptureAgent&) = delete;

    [[nodiscard]] bool try_capture(Record record) noexcept {
        if (gate_.test_and_set(std::memory_order_acquire)) {
            dropped_contended_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (count_ == Capacity) {
            gate_.clear(std::memory_order_release);
            dropped_full_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        records_[write_index_] = std::move(record);
        write_index_ = (write_index_ + 1) % Capacity;
        ++count_;
        gate_.clear(std::memory_order_release);
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool try_pop(Record& record) noexcept {
        if (gate_.test_and_set(std::memory_order_acquire)) {
            return false;
        }
        if (count_ == 0) {
            gate_.clear(std::memory_order_release);
            return false;
        }

        record = std::move(records_[read_index_]);
        read_index_ = (read_index_ + 1) % Capacity;
        --count_;
        gate_.clear(std::memory_order_release);
        return true;
    }

    [[nodiscard]] CaptureAgentStats stats() const noexcept {
        return CaptureAgentStats{
            accepted_.load(std::memory_order_relaxed),
            dropped_full_.load(std::memory_order_relaxed),
            dropped_contended_.load(std::memory_order_relaxed),
        };
    }

private:
    std::array<Record, Capacity> records_{};
    std::size_t read_index_{};
    std::size_t write_index_{};
    std::size_t count_{};
    mutable std::atomic_flag gate_ = ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> accepted_{};
    std::atomic<std::uint64_t> dropped_full_{};
    std::atomic<std::uint64_t> dropped_contended_{};
};

}  // namespace opennova::retail_hook
