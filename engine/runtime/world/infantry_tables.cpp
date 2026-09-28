// Infantry anim-state tables, extracted from Jointops.exe (IDB 2026-06-10;
// extended to the full 252 entries 2026-07-16 — AnimMap_FindSlotByName @0x40cfa0
// scans exactly 252). The names themselves are runtime/anim/anim_slot_names.h.
// Flags: [orig: g_AnimStateFlagsTable @0x8139E8]; bit semantics in infantry.h. All
// entries dumped index-by-index from the IDB (173..239 = the uniform death-family
// value 0x82; the wpn_* rows 240..251 are 0).

#include <runtime/world/infantry.h>

#include <cmath>

namespace opennova::world {

std::string infantry_anim_key(int state) {
    if (state < 0 || state >= kInfantryAnimStateCount) return std::string();
    const char *name = anim::kAnimSlotNames[state];
    if (name == nullptr || name[0] == '\0') return std::string();
    return std::string("anim_") + name;
}

// Body-state facial expressions. [orig: byte_813DE0; producer @0x4BE0C2]
const uint8_t kInfantryFacialExpressions[kInfantryAnimStateCount] = {
    /*   0 */ 0, 8, 0, 0, 7, 7, 7, 0, 0, 4, 4, 0, 0, 0, 7, 7,
    /*  16 */ 7, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 6, 6, 5, 7,
    /*  32 */ 7, 0, 0, 0, 0, 0, 0, 0, 0, 8, 8, 0, 8, 8, 8, 1,
    /*  48 */ 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 7,
    /*  64 */ 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 0, 0, 8, 0,
    /*  80 */ 8, 0, 8, 0, 8, 0, 8, 0, 8, 0, 8, 0, 8, 0, 8, 0,
    /*  96 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7,
    /* 112 */ 7, 7, 7, 3, 6, 3, 6, 3, 6, 3, 6, 3, 6, 3, 5, 0,
    /* 128 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 6, 2, 8, 6, 4, 7,
    /* 144 */ 0, 2, 7, 7, 8, 8, 7, 1, 7, 6, 4, 6, 4, 4, 4, 4,
    /* 160 */ 4, 4, 4, 7, 7, 7, 7, 8, 7, 8, 8, 8, 8, 7, 7, 7,
    /* 176 */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /* 192 */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /* 208 */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /* 224 */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    /* 240 */ 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
};

const uint32_t kInfantryAnimFlags[kInfantryAnimStateCount] = {
    /*   0 */ 0x000,
    /*   1 */ 0x449, 0x449, 0x449, 0x449, 0x449, 0x449, 0x449, 0x449,
    /*   9 */ 0x449, 0x449,
    /*  11 */ 0x549, 0x549, 0x549, 0x549, 0x549, 0x549, 0x549, 0x549,
    /*  19 */ 0x603, 0x603, 0x603, 0x603, 0x603, 0x603, 0x603, 0x603,
    /*  27 */ 0x048, 0x449, 0x449,
    /*  30 */ 0x441, 0x441,
    /*  32 */ 0x000, 0x401, 0x401, 0x004,
    /*  36 */ 0x040, 0x401, 0x401, 0x401, 0x441,
    /*  41 */ 0x285, 0x285,
    /*  43 */ 0x048, 0x048, 0x148, 0x008, 0x009, 0x202, 0x050,
    /*  50 */ 0x080, 0x000, 0x080, 0x000, 0x080, 0x000, 0x000, 0x000, 0x000, 0x000,
    /*  60 */ 0x000, 0x000, 0x094, 0x094, 0x080, 0x084, 0x084,
    /*  67 */ 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010, 0x010,
    /*  76 */ 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048,
    /*  86 */ 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048,
    /*  96 */ 0x048, 0x048, 0x048, 0x048,
    /* 100 */ 0x448, 0x048, 0x048, 0x048, 0x048, 0x048, 0x048, 0x448, 0x448, 0x448,
    /* 110 */ 0x448, 0x004, 0x004, 0x004, 0x004,
    /* 115 */ 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020, 0x020,
    /* 125 */ 0x040, 0x040, 0x014, 0x014, 0x014,
    /* 130 */ 0x004, 0x004, 0x004, 0x004, 0x004, 0x004, 0x004,
    /* 137 */ 0x002, 0x003, 0x002,
    /* 140 */ 0x000, 0x000, 0x014, 0x004, 0x004,
    /* 145 */ 0x049, 0x049, 0x041, 0x049, 0x049,
    /* 150 */ 0x000, 0x015, 0x015, 0x004, 0x014,
    /* 155 */ 0x014, 0x014, 0x014, 0x014,
    /* 159 */ 0x014, 0x014, 0x014, 0x014,
    /* 163 */ 0x050, 0x041, 0x014, 0x014,
    /* 167 */ 0x049, 0x049,
    /* 169 */ 0x18d, 0x18d, 0x18d, 0x28d,
    /* 173 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 180 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 190 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 200 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 210 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 220 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 230 */ 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082, 0x082,
    /* 240 */ 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    /* 250 */ 0x000, 0x000,
};

uint32_t infantry_anim_flags(int state) {
    if (state < 0 || state >= kInfantryAnimStateCount) return 0;
    return kInfantryAnimFlags[state];
}

// [orig: Entity_ComputeAnimSlotIndex @0x43a690] Hit-bone index -> bullet death-anim
// group. Groups index the death_bullet families in table order: 0 hip, 1 torso,
// 2 head, 3 rightshoulder, 4 leftshoulder, 5 rightarm, 6 leftarm, 7 righthand,
// 8 lefthand, 9 rightthigh, 10 leftthigh, 11 rightcalf, 12 leftcalf, 13 rightfoot,
// 14 leftfoot.
static const int kDeathBoneGroup[32] = {
    /* 0  hips        */ 0,
    /* 1-4 spine/torso*/ 1, 1, 1, 1,
    /* 5  R shoulder  */ 3,
    /* 6  L shoulder  */ 4,
    /* 7  R thigh     */ 9,
    /* 8  L thigh     */ 10,
    /* 9  R arm       */ 5,
    /* 10 L arm       */ 6,
    /* 11 R calf      */ 11,
    /* 12 L calf      */ 12,
    /* 13 neck        */ 2,
    /* 14 head        */ 2,
    /* 15 L hand      */ 8,
    /* 16 R hand      */ 7,
    /* 17 R foot      */ 13,
    /* 18 L foot      */ 14,
    /* 19-21 R fingers*/ 7, 7, 7,
    /* 22-24 L fingers*/ 8, 8, 8,
    /* 25-26 R hand   */ 7, 7,
    /* 27-28 L hand   */ 8, 8,
    /* 29-31 torso    */ 1, 1, 1,
};

int compute_death_anim_state(int bone_index, int quadrant, int cause) {
    if (bone_index < 0 || bone_index >= 32) bone_index = 0; // [orig: >=32 -> 0]
    if (quadrant < 0 || quadrant >= 4) quadrant = 0;        // [orig: >=4 -> 0]
    switch (cause) {                                        // [orig: switch(entityType)]
        case death_cause::kBullet:
            return anim_state::kDeathBulletBase + quadrant + 4 * kDeathBoneGroup[bone_index];
        case death_cause::kExplosive:
            return anim_state::kDeathGrenadeBase + quadrant;
        case death_cause::kFire:
            return anim_state::kDeathFire;
        case death_cause::kDrown:
            return anim_state::kDeathDrown;
        default:
            return anim_state::kDeathPungi; // [orig: slotIndex preset 174]
    }
}

int death_quadrant_from_round(int32_t victim_heading_bam, float round_vel_x, float round_vel_y) {
    // BAM bearing of the round's horizontal travel [orig: atan2(vel.y, vel.x) *
    // 683565275.5764316 @0x407478 — arg scale cancels inside atan2].
    const double bam = std::atan2(static_cast<double>(round_vel_y),
                                  static_cast<double>(round_vel_x)) * 683565275.5764316;
    const uint32_t bearing = static_cast<uint32_t>(static_cast<int64_t>(bam));
    return static_cast<int>((static_cast<uint32_t>(victim_heading_bam) - bearing -
                             0x60000000u) >> 30);
}

} // namespace opennova::world
