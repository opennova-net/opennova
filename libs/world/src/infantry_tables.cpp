// Infantry anim-state tables, extracted from Jointops.exe (IDB 2026-06-10;
// extended to the full 252 entries 2026-07-16 — AnimMap_FindSlotByName @0x40cfa0
// scans exactly 252).
// Names: [orig: g_animStateNameTable @0x8135F0] (the .adm clip keys, "anim_<name>").
// Flags: [orig: g_animStateFlagsTable @0x8139E8]; bit semantics in infantry.h. All
// entries dumped index-by-index from the IDB (173..239 = the uniform death-family
// value 0x82; the wpn_* rows 240..251 are 0).

#include "world/infantry.h"

#include <cmath>

namespace opennova::world {

const char *const kInfantryAnimNames[kInfantryAnimStateCount] = {
    /*  0 */ "reset",
    /*  1 */ "walk_forward", "walk_forwardright", "walk_right", "walk_backright",
    /*  5 */ "walk_back", "walk_backleft", "walk_left", "walk_forwardleft",
    /*  9 */ "run_2", "run_3",
    /* 11 */ "walk_crouch_forward", "walk_crouch_forwardright", "walk_crouch_right",
    /* 14 */ "walk_crouch_backright", "walk_crouch_back", "walk_crouch_backleft",
    /* 17 */ "walk_crouch_left", "walk_crouch_forwardleft",
    /* 19 */ "walk_prone_forward", "walk_prone_forwardright", "walk_prone_right",
    /* 22 */ "walk_prone_backright", "walk_prone_back", "walk_prone_backleft",
    /* 25 */ "walk_prone_left", "walk_prone_forwardleft",
    /* 27 */ "wash_idle", "wash_walk", "wash_run",
    /* 30 */ "jump_start", "jump_loop",
    /* 32 */ "climb_idle", "climb_up", "climb_down", "climb_top",
    /* 36 */ "swim_idle", "swim_forward", "swim_left", "swim_right", "swim_back",
    /* 41 */ "roll_left", "roll_right",
    /* 43 */ "idle", "idle_2", "idle_crouch", "idle_mortar", "parachute",
    /* 48 */ "idle_prone", "idle_3",
    /* 50 */ "knife", "pistol", "grenade", "stinger", "designator",
    /* 55 */ "designator_scoped", "P90", "P90_scoped", "MP7", "MP7_scoped",
    /* 60 */ "javelin", "javelin_scoped", "knife_attack", "grenade_attack",
    /* 64 */ "binoculars", "reload", "reload2",
    /* 67 */ "emplaced", "emplaced_2", "emplaced_3", "emplaced_4", "emplaced_5",
    /* 72 */ "emplaced_6", "emplaced_7", "emplaced_8", "emplaced_9",
    /* 76 */ "sit", "sit_1", "sit_2", "sit_3", "sit_4", "sit_5", "sit_6", "sit_7",
    /* 84 */ "sit_8", "sit_9", "sit_10", "sit_11", "sit_12", "sit_13", "sit_14",
    /* 91 */ "sit_15", "sit_16", "sit_17", "sit_18", "sit_19", "sit_20", "sit_21",
    /* 98 */ "sit_22", "sit_23", "sit_24", "sit_25", "sit_26", "sit_27", "sit_28",
    /*105 */ "sit_29", "sit_30", "sit_24_stop", "sit_24_back", "sit_24_left",
    /*110 */ "sit_24_right",
    /*111 */ "burn", "burn_2", "burn_3", "burn_4",
    /*115 */ "emote_1", "emote_2", "emote_3", "emote_4", "emote_5", "emote_6",
    /*121 */ "emote_7", "emote_8", "emote_9", "emote_10",
    /*125 */ "idle_look", "idle_2_look", "idle_4", "idle_5", "idle_6", "idle_7",
    /*131 */ "idle_8", "idle_9", "idle_10", "idle_11", "idle_12", "hover",
    /*137 */ "dragger_idle", "dragger_walk", "draggee",
    /*140 */ "guard", "guard_look", "guard_attack", "guard_cover", "guard_leave",
    /*145 */ "wounded_walk", "wounded_run", "stop", "jog_forward", "run_forward",
    /*150 */ "hold_rope", "post_attack", "pre_attack", "out_of_ground", "swim_attack",
    /*155 */ "attack", "attack_2", "attack_3", "attack_4",
    /*159 */ "grenade_1r", "grenade_2r", "grenade_1l", "grenade_2l",
    /*163 */ "cover_idle", "cover_run", "cover_attack", "cover_attack_2",
    /*167 */ "run_attack", "run_away",
    /*169 */ "run2crouch", "runl2crouch", "runr2crouch", "run2prone",
    /*173 */ "death_fire", "death_pungi", "death_drown",
    /*176 */ "death_grenade_forward", "death_grenade_right", "death_grenade_back",
    /*179 */ "death_grenade_left",
    /*180 */ "death_bullet_hip_forward", "death_bullet_hip_right",
    /*182 */ "death_bullet_hip_back", "death_bullet_hip_left",
    /*184 */ "death_bullet_torso_forward", "death_bullet_torso_right",
    /*186 */ "death_bullet_torso_back", "death_bullet_torso_left",
    /*188 */ "death_bullet_head_forward", "death_bullet_head_right",
    /*190 */ "death_bullet_head_back", "death_bullet_head_left",
    /*192 */ "death_bullet_rightshoulder_forward", "death_bullet_rightshoulder_right",
    /*194 */ "death_bullet_rightshoulder_back", "death_bullet_rightshoulder_left",
    /*196 */ "death_bullet_leftshoulder_forward", "death_bullet_leftshoulder_right",
    /*198 */ "death_bullet_leftshoulder_back", "death_bullet_leftshoulder_left",
    /*200 */ "death_bullet_rightarm_forward", "death_bullet_rightarm_right",
    /*202 */ "death_bullet_rightarm_back", "death_bullet_rightarm_left",
    /*204 */ "death_bullet_leftarm_forward", "death_bullet_leftarm_right",
    /*206 */ "death_bullet_leftarm_back", "death_bullet_leftarm_left",
    /*208 */ "death_bullet_righthand_forward", "death_bullet_righthand_right",
    /*210 */ "death_bullet_righthand_back", "death_bullet_righthand_left",
    /*212 */ "death_bullet_lefthand_forward", "death_bullet_lefthand_right",
    /*214 */ "death_bullet_lefthand_back", "death_bullet_lefthand_left",
    /*216 */ "death_bullet_rightthigh_forward", "death_bullet_rightthigh_right",
    /*218 */ "death_bullet_rightthigh_back", "death_bullet_rightthigh_left",
    /*220 */ "death_bullet_leftthigh_forward", "death_bullet_leftthigh_right",
    /*222 */ "death_bullet_leftthigh_back", "death_bullet_leftthigh_left",
    /*224 */ "death_bullet_rightcalf_forward", "death_bullet_rightcalf_right",
    /*226 */ "death_bullet_rightcalf_back", "death_bullet_rightcalf_left",
    /*228 */ "death_bullet_leftcalf_forward", "death_bullet_leftcalf_right",
    /*230 */ "death_bullet_leftcalf_back", "death_bullet_leftcalf_left",
    /*232 */ "death_bullet_rightfoot_forward", "death_bullet_rightfoot_right",
    /*234 */ "death_bullet_rightfoot_back", "death_bullet_rightfoot_left",
    /*236 */ "death_bullet_leftfoot_forward", "death_bullet_leftfoot_right",
    /*238 */ "death_bullet_leftfoot_back", "death_bullet_leftfoot_left",
    /*240 */ "wpn_reset", "wpn_idle", "wpn_empty_idle", "wpn_fire", "wpn_recoil",
    /*245 */ "wpn_reload", "wpn_empty", "wpn_switchto", "wpn_switchfrom",
    /*249 */ "wpn_switchrank", "wpn_scopeup", "wpn_scopedown",
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
