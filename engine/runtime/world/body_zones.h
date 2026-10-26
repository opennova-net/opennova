// The mission's player-body zones: the BMS header's two rates and the type 1..3 bounding boxes
// the player body's health-adjust block walks (AiSystem::infantry_health_adjust); the type 4..6
// boxes of the same list ride their own homes (reverb.h, the registry's locations, the music).
#pragma once

#include <cstdint>
#include <vector>

namespace opennova::world {

// One box as the load keeps it: each axis normalized, the type, its value word (+0x10 of the
// walk's row) and the next word (+0x14), which a Mission box's name runs into.
// [orig: Mission_LoadBMSFile @0x40FCC3 (the normalize), the copy @0x40FCDC; the rows walked
//  from unk_A76444, 0x24 bytes apart, count dword_A77640]
struct BodyZoneBox {
    int32_t min[3] = {};
    int32_t max[3] = {};
    int32_t type = 0;
    uint32_t value = 0;
    uint32_t value_next = 0;
};

struct BodyZones {
    // The header's per-pass rates, read as words [orig: word_A762D8 (health), word_A762DC (mana)].
    uint16_t header_health = 0;
    uint16_t header_mana = 0;
    std::vector<BodyZoneBox> boxes; // types 1..3, in file order
};

} // namespace opennova::world
