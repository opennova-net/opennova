#pragma once

#include <cstdint>

namespace opennova::io {

// The MSVC CRT rand() recurrence, owned rather than process-global.
//
// Retail calls the CRT's process-wide rand() (import @0x76B00A; xrefs 0x405149
// 0x405785 0x411EA9 0x411EBB 0x42A3D0 0x44055F 0x4405E8 0x4C4D8C 0x4DBB9F
// 0x4DBC45 0x502A95 0x507EAE) and seeds it once from the clock in
// Server_AllocatePlayerSlotTable (`srand(_time64(0))` @0x51C1AA; also @0x560128
// and @0x5CC234); Game_StartMission never re-seeds it. The generator is the
// 32-bit LCG `state = state * 214013 + 2531011`, returning bits 16..30:
// srand(1) yields 41, 18467, 6334, 26500, 19169, ... The default state 1 is the
// CRT's own (a process that never calls srand). Each consumer that retail routes
// through rand() draws from ONE owner so the draw order stays structural.
// [orig: CRT rand @0x76B00A; srand @0x51C1AA]
struct CrtRand {
    uint32_t state = 1;
    void seed(uint32_t value) { state = value; }
    uint32_t next() {
        state = state * 214013u + 2531011u;
        return (state >> 16) & 0x7FFFu;
    }
};

} // namespace opennova::io
