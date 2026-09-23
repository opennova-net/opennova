// Cumulative evidence of runtime behavior that has no implemented consumer.
// This is port diagnostics, not gameplay state or a retail behavior substitute.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::world {

enum class RuntimeGapKind : uint8_t {
    WacCommand, WacOpcode, WacInstructionLimit, BmsAction
};

struct RuntimeGapSite {
    RuntimeGapKind kind = RuntimeGapKind::WacCommand;
    int32_t code = 0;
    int32_t subcode = 0;
    int32_t event = -1;
    int32_t site = -1; // bytecode word, BMS action index, or entity handle
    std::string source;
    int32_t line = 0;

    bool operator==(const RuntimeGapSite &other) const {
        return kind == other.kind && code == other.code && subcode == other.subcode &&
                event == other.event && site == other.site && source == other.source && line == other.line;
    }
};

struct RuntimeGap {
    RuntimeGapSite origin;
    uint64_t count = 0;
    uint32_t first_tick = 0;
    uint32_t last_tick = 0;
    std::array<int32_t, 4> arguments{};
};

class MissionDiagnostics {
public:
    void record(const RuntimeGapSite &origin, uint32_t tick,
                const std::array<int32_t, 4> &arguments = {}) {
        ++total_calls_;
        for (RuntimeGap &gap : gaps_) {
            if (!(gap.origin == origin)) continue;
            ++gap.count;
            gap.last_tick = tick;
            gap.arguments = arguments;
            return;
        }
        gaps_.push_back({origin, 1, tick, tick, arguments});
    }

    const std::vector<RuntimeGap> &gaps() const { return gaps_; }
    uint64_t total_calls() const { return total_calls_; }
    bool empty() const { return gaps_.empty(); }
    void clear() { gaps_.clear(); total_calls_ = 0; }

private:
    std::vector<RuntimeGap> gaps_;
    uint64_t total_calls_ = 0;
};

} // namespace opennova::world
