// Compiled WAC program: the faithful bytecode stream plus the rebased operand /
// string pools and the diagnostics gathered during compilation.
#ifndef OPENNOVA_WAC_PROGRAM_H
#define OPENNOVA_WAC_PROGRAM_H

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

struct Program {
    std::vector<uint32_t> code;        // instructions + inline operand refs; ends in 0x7A7A7A7A
    std::vector<int32_t> operands;     // resolved literal/handle/id pool [orig: dword_C6AA30]
    std::vector<std::string> strings;  // string literals [orig: byte_C69A20]
    int event_count = 0;               // number of top-level IF/event rules
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

#endif // OPENNOVA_WAC_PROGRAM_H
