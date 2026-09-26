// The local player's presentation law (player_present.h carries the witness
// map).
#include <runtime/world/player_present.h>

namespace opennova::world {

// [orig: the FP viewmodel gate g_camera_mode == 0 in
//  Entity_ComputeActionTransform @0x40133e -- the death lerp camera (4)
//  presents like the chase (1)]
bool presents_third_person(bool third_person, int camera_mode) {
    return third_person || camera_mode == 4;
}

// [orig: the alive gate Player_RenderViewModelIfAlive @0x4E0145/@0x4E014B;
//  the card switch @0x5ca299..0x5ca304 / @0x5caaf3..0x5cab15 / @0x5ca32c;
//  the seat gate inside the draw; the Emplaced skip of the showhud bit
//  @0x4DEDD9..0x4DEDF1; the Inset gate @0x4DEDF7..0x4DEE19]
bool fp_viewmodel_retail_submit(const FpViewmodelSubmitGates &gates) {
    if (gates.local_dead || gates.round_winner_set) return false;
    if (gates.third_person || gates.scope_card_active || gates.binoculars_view_active)
        return false;
    if (gates.seat_hides_weapon) return false;
    if (!gates.emplaced && !gates.fp_weapon_view_flag) return false;
    return !gates.inset_scoped;
}

// [orig: Player_RenderFirstPersonViewModel @0x4DEEA4..0x4DEF52 -- the FP
//  leg drops the sun sample; the world body stacks it @0x5c7bff]
LocalPlayerLightingContext local_player_lighting_context(int interior_item_id,
                                                         float light_transfer,
                                                         float body_sun_factor) {
    LocalPlayerLightingContext out;
    out.interior = interior_item_id != 0;
    out.light_transfer = out.interior ? light_transfer : 0.0f;
    out.body_effect_scale = body_sun_factor;
    out.fp_effect_scale = 1.0f;
    return out;
}

// [orig: ActionSlot_ExecuteActionTick @0x541a70 -- the FIRE-only shim pick
//  @0x541b17 and the settled-scope gate @0x541aba]
bool local_fire_effect_admitted(int action_started, int fire_action, bool has_particle,
                                bool scope_settled, bool third_person,
                                bool vehicle_attack_context) {
    if (!has_particle) {
        return false;
    }
    if (action_started != fire_action) {
        return false; // local non-fire begins are the no-effect shim [orig: @0x541b17]
    }
    if (scope_settled && !third_person && !vehicle_attack_context) {
        return false; // settled-scoped FP fire shows no muzzle flash [orig: @0x541aba]
    }
    return true;
}

// [orig: the FP bit gate @0x540e8c..0x540eca; the gfx1/gfx3 resolvers
//  @0x54039e/@0x54040f]
bool action_particle_uses_third_person_gun(bool third_person) {
    return third_person;
}

bool action_particle_uses_mounted_gun(bool borrowed_usegun_slot,
                                     bool third_person, bool first_person_action_model) {
    return borrowed_usegun_slot && (third_person || !first_person_action_model);
}

// [orig: ActionSlot_BeginActivePhase @0x53f830; the gate
//  ActionSlot_ExecuteActionNoEffect @ 0x541a4d; the rescope block @0x54139e]
void weapon_batch_plan(bool view_active, int32_t view_play_serial, int32_t play_serial,
                       const WeaponBatchEvent *events, size_t count,
                       WeaponBatchPlan &out) {
    out.steps.clear();
    out.play_serial = play_serial;
    if (!view_active) {
        // Slot selection is control state, not viewmodel presentation. In
        // particular, an unarmed player has no view until UseGun installs one.
        for (size_t i = 0; i < count; ++i) {
            if (events[i].clear_weapon) {
                out.steps.push_back({WeaponPresentOp::kClearWeapon, static_cast<int>(i)});
            } else if (events[i].switch_weapon) {
                out.steps.push_back({WeaponPresentOp::kSwitchWeapon, static_cast<int>(i)});
            }
        }
        out.play_serial = -1;
        return;
    }
    bool batch_started_clip = false;
    for (size_t i = 0; i < count; ++i) {
        if (events[i].starts_clip) {
            batch_started_clip = true;
            break;
        }
    }
    if (!batch_started_clip) {
        // Pose the already-playing clip at the sim's channel position before
        // same-tick direct effects sample an action user point. A clip event
        // below replaces this pose first.
        out.steps.push_back({WeaponPresentOp::kPoseChannel, -1});
    }
    for (size_t i = 0; i < count; ++i) {
        const WeaponBatchEvent &event = events[i];
        const int index = static_cast<int>(i);
        if (event.starts_clip) {
            // A batch's clip events are superseded by later ones; the
            // surviving clip's position is the snapshot's channel position (0
            // on the production tick itself).
            out.steps.push_back({WeaponPresentOp::kPlayClip, index});
        }
        if (event.action_started) {
            out.steps.push_back({WeaponPresentOp::kActionBegin, index});
        }
        if (event.action_effect) {
            out.steps.push_back({WeaponPresentOp::kDirectEffect, index});
        }
        if (event.action_finished) {
            out.steps.push_back({WeaponPresentOp::kActionEnd, index});
        }
        if (event.clear_weapon) {
            out.steps.push_back({WeaponPresentOp::kClearWeapon, index});
        } else if (event.switch_weapon) {
            out.steps.push_back({WeaponPresentOp::kSwitchWeapon, index});
        }
        if (event.switch_denied) {
            out.steps.push_back({WeaponPresentOp::kSwitchDenied, index});
        }
    }
    if (batch_started_clip) {
        out.play_serial = view_play_serial;
    }
    // First adoption and a fresh viewmodel both synchronize to the latest
    // snapshot, but never replay the snapshot's historical sound/effect
    // payloads.
    if (view_play_serial != out.play_serial) {
        out.play_serial = view_play_serial;
        out.steps.push_back({WeaponPresentOp::kPoseChannel, -1});
    }
}

// [orig: Player_RenderFirstPersonViewModel @0x4DEE96..0x4DEE9F (TEX_TEAM),
//  @0x4DEEC2..0x4DEEF5 (HEAT_GLOW); Avatar_SetArmsCamoCtrl @0x57a3b0 at
//  @0x4df008/@0x4df070]
FpCtrlRegisterWrites fp_ctrl_register_writes(bool submit, bool has_weapon_view,
                                             bool emplaced_controls_valid, bool arms_part) {
    FpCtrlRegisterWrites out;
    out.team = submit;
    out.heat = submit && has_weapon_view;
    out.emplaced = submit && has_weapon_view && emplaced_controls_valid;
    out.arms_camo = submit && arms_part;
    return out;
}

// [orig: the @0x49d1f0 scan's per-row down latch reads raw key state]
bool latched_key_edge(bool down, bool active, bool &was_down) {
    const bool edge = active && down && !was_down;
    was_down = down;
    return edge;
}

// [orig: the equipped AdmDef drives the FP model pick,
//  Player_RenderFirstPersonViewModel @0x4ded60 via the mounted slot]
ViewmodelDefPick viewmodel_def_pick(bool cleared, const std::string &override_name,
                                    const char *bringup_fallback) {
    ViewmodelDefPick out;
    if (cleared) {
        return out;
    }
    out.resolves = true;
    out.name = override_name.empty()
            ? std::string(bringup_fallback != nullptr ? bringup_fallback : "")
            : override_name;
    return out;
}

bool viewmodel_def_memo_hit(const std::string &name, const std::string &memo_name,
                            bool memo_present) {
    return memo_present && name == memo_name;
}

SpawnLoadoutPlan spawn_loadout_plan(const SpawnLoadoutInput &input,
                                    bool has_explicit_spawn_loadout) {
    SpawnLoadoutPlan plan;
    bool has_loadout = false;
    for (const SpawnLoadoutSlot &slot : input.slots) {
        if (slot.present) {
            has_loadout = true;
            break;
        }
    }
    plan.set_player_class = input.has_player_class;
    plan.player_class = input.has_player_class ? input.player_class : 0;
    // Mission-authored kits outrank the profile selection.
    if (has_explicit_spawn_loadout) {
        plan.action = SpawnLoadoutAction::kSyncInventory;
        return plan;
    }
    if (!has_loadout) {
        plan.action = SpawnLoadoutAction::kNone;
        return plan;
    }
    plan.action = SpawnLoadoutAction::kApplyKit;
    for (const SpawnLoadoutSlot &slot : input.slots) {
        if (!slot.present || slot.name.empty()) {
            continue;
        }
        plan.kit.push_back({slot.name, slot.clips});
    }
    plan.after_apply = plan.kit.empty() ? SpawnLoadoutAfterApply::kClearWeapon
                                        : SpawnLoadoutAfterApply::kSyncInventory;
    return plan;
}

FireEffectPlan fire_effect_plan(const FirePresentationRow &row) {
    FireEffectPlan plan;
    plan.userpoint = row.action_userpoint;
    // The MF_Light muzzle glow re-arms per shot for EVERY shooter — retail
    // spawns it on both fire arms, the local player's included [orig:
    // WeaponSlot_FireAndSpawnEffects @ 0x53f597 at the fire position;
    // ActionSlot_SpawnEffect @ 0x402080 at the action-transform muzzle;
    // both gate on ammo +36 MF_Light]. Owner = shooter, so the witnessed
    // group gate scopes it to the shooter's own skinned person draws
    // (renderer::submit_owner_group). The adm arm anchors it
    // on the rendered gun's userpoint.
    plan.glow = row.mf_light != 0;
    plan.glow_at_muzzle = plan.glow && row.adm_arm;
    // The local player's own fire is presented by the action-slot legs
    // [orig: ActionSlot_ExecuteActionTick @ 0x541A70 routing]; everyone
    // else's rides the ammo-def legs below. (The SOUND legs of every arm
    // run in the sim — world/fire_sound.h — and arrive through
    // drain_fire_sounds; this plan owns the EFFECT legs.)
    if (row.is_local_player) return plan;
    // THE ARM SPLIT. Retail's round-event receive path has two mutually exclusive
    // arms and only one of them is the ammo-def pair. The adm-indexed arm spawns
    // no ammo-def effect: it executes the ADDRESSED def's FIRE action row
    // instead, at that weapon's own userpoint on the gfx3 model.
    // This matters because the wire position is the shooter's EYE — retail sends
    // Position + CameraOffset [orig: Entity_CalcWeaponFirePosition @0x4dc750] — so
    // running the ammo-def leg on this arm draws every remote muzzle flash out of
    // the shooter's face, roughly a metre behind the barrel.
    // [orig: arms @0x42f521 / @0x42f6ce; ammo effect @0x42f6c2;
    //  the fire row @0x42f777 / @0x42f98f]
    plan.effect = row.adm_arm ? row.action_effect : row.effect;
    // The anchor: this shooter's held weapon, not the wire point — the
    // rendered gun's own userpoint, which is what retail spawns at (the
    // authority DECISION closing the S12a shadow seam: the rendered-node
    // anchor is permanent, the sim-posed re-derivation is gone). Falling
    // back to the wire eye position would reintroduce the very bug this
    // fixes, so an unresolvable anchor takes the provider's own
    // body-origin fallback — retail's deepest fallback is the entity
    // origin [orig: @0x401867..0x401887].
    plan.spawn_at_muzzle = row.adm_arm;
    // The muzzle effect at the fire origin along the fire direction
    // [orig: the 56-B spawn descriptor -> CEffectWorld_SpawnEmitterAtPosition
    // @ 0x5F6DF0; every fire spawns one — no per-shooter guard on this leg].
    plan.spawn = !plan.effect.empty();
    return plan;
}

} // namespace opennova::world
