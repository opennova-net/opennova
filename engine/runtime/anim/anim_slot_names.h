// The 252 anim slot names, the leaf every slot lookup shares: the body states
// (0..239, 180..239 the 15-group bullet death matrix) and the wpn_* FP
// viewmodel states (240..251). A slot's .adm key is "anim_" + its name
// (world::infantry_anim_key); a row's key names its slot past its first five
// characters, compared without case (anim::adm_slot_index).
// [orig: g_AnimStateNameTable @0x8135F0, scanned over exactly 252 entries by
//  AnimMap_FindSlotByName @0x40cfa0]
// Extracted from Jointops.exe (IDB 2026-06-10; extended to the full 252
// entries 2026-07-16), dumped index by index.
#pragma once

namespace opennova::anim {

inline constexpr int kAnimSlotCount = 252;

inline constexpr const char *const kAnimSlotNames[kAnimSlotCount] = {
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

} // namespace opennova::anim
