// The runtime-built quantized direction table shared by the infantry motor and the
// collision resolver. [orig: Math_BuildSinTable @ 0x613050 — 1281 entries, value =
// trunc(sin(angle) * 2^22) with the angle ACCUMULATED per entry (angle +=
// dbl_7DF578 = 0.006135923151542565, the double nearest 2pi/1024) and truncated
// toward zero (_ftol2_sse). The cos consumer reads the same table +256 entries
// (off_849934 = outMillis + 0x400).] (D-INF-4 CLOSED — the generator is witnessed.)
#ifndef OPENNOVA_WORLD_DIR_TABLE_H
#define OPENNOVA_WORLD_DIR_TABLE_H

#include <cmath>
#include <cstdint>

namespace opennova::world {

struct DirTable {
    int32_t sin22[1281];
    DirTable() {
        double angle = 0.0;
        constexpr double kStep = 0.006135923151542565; // [orig: dbl_7DF578]
        for (int i = 0; i < 1281; ++i) {
            sin22[i] = static_cast<int32_t>(std::sin(angle) * 4194304.0); // [orig: dbl_7C3600]
            angle += kStep;
        }
    }
};

inline const DirTable &dir_table() {
    static const DirTable t;
    return t;
}

// Quantized heading -> direction vector, 22-bit scale. [orig: idx = (h + 0x200000) >> 22
// into Math_BuildSinTable's table; sin = outMillis[idx], cos = (outMillis+0x400)[idx]]
inline void quantized_dir(int32_t heading, int32_t &cos22, int32_t &sin22) {
    uint32_t idx = (static_cast<uint32_t>(heading) + 0x200000u) >> 22;
    const DirTable &t = dir_table();
    sin22 = t.sin22[idx];
    cos22 = t.sin22[idx + 256u];
}

} // namespace opennova::world

#endif // OPENNOVA_WORLD_DIR_TABLE_H
