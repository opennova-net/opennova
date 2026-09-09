// Compiled WAC program: the faithful bytecode stream plus the rebased operand /
// string pools and the diagnostics gathered during compilation.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace opennova::wac {

struct Diagnostic {
    int line = 0;
    int col = 0;
    std::string message;
    bool error = true;
};

struct InstructionSource {
    uint32_t word = 0;
    uint32_t source_index = 0;
    int line = 0;
};

struct Program {
    std::vector<uint32_t> code;        // instructions + inline operand refs; ends in 0x7A7A7A7A
    std::vector<int32_t> operands;     // resolved literal/handle/id pool [orig: dword_C6AA30]
    std::vector<std::string> strings;  // string literals [orig: byte_C69A20]
    // Effect handles are stable 1-based integers, separate from text-pool offsets.
    // The presentation consumer resolves these names in its mounted effect scene.
    std::vector<std::string> effect_names;
    std::vector<std::string> sound_names; // rebased nonzero SOUNDSET handles
    int event_count = 0;              // includes nested IF/event rules
    // Source nesting for previous/chain's backward event walk.
    // [orig: WacCmd_Chain @0x4ECF20; WacCmd_Previous @0x4ECF90]
    std::vector<uint16_t> event_depths;
    uint32_t loop_count = 0;
    std::vector<std::string> source_names;
    std::vector<InstructionSource> instruction_sources;
    std::vector<Diagnostic> diagnostics;

    bool ok() const {
        for (const Diagnostic &d : diagnostics) {
            if (d.error) return false;
        }
        return true;
    }
    int error_count() const {
        int n = 0;
        for (const Diagnostic &d : diagnostics) {
            if (d.error) ++n;
        }
        return n;
    }
};

} // namespace opennova::wac
