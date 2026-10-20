// The infantry state selection and commit (the org1 think's selector and the
// org2 player-body selector, the shared flag-table arbitration and the
// availability fallbacks), split out of infantry.cpp to keep that TU under the
// 2500-line size ratchet. Bodies moved verbatim.
// Witness record: docs/world/world-wac-ai-re.md.

#include <runtime/world/ai.h>
#include <runtime/world/collision_force.h> // select_infantry_burn
#include <runtime/world/infantry_internal.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>
#include <base/io/bam.h>

namespace opennova::world {

namespace {

// [orig: turn-in-place gates; dump 2940-2952]
constexpr int32_t kTurnStopGate = 536870880;  // > 45 deg -> state 147 (stop)
constexpr int32_t kTurnWalkGate = 357913920;  // > 30 deg -> state 1 (walk turn)

int32_t abs_bam(int32_t v) { return opennova::io::bam_abs(v); } // x86 neg: INT32_MIN stays put, never UB

int player_directional_state(int base, int move_dir_index) {
    static constexpr int kOffsetFromInputIndex[8] = {0, 7, 6, 5, 4, 3, 2, 1};
    return base + kOffsetFromInputIndex[move_dir_index & 7];
}

} // namespace

// ----------------------------------------------------------------------------
// State selection + commit (every think). [orig: 0x4b9910 dump 2896-3110, 3693-3710]
// ----------------------------------------------------------------------------
int AiSystem::infantry_resolve_state(int adm_id, int state) const {
    return world::infantry_resolve_state(root_motion, adm_id, state);
}

int infantry_resolve_state(const IRootMotionSource *source, int adm_id, int state) {
    if (source == nullptr) return -1;
    auto has = [&](int s) { return source->has_clip(adm_id, s); };
    if (has(state)) return state;
    // Cited fallback chains. [orig: availability = animMap[id] != animMap[0]; jog<->run
    // mutual fallback dump 3010-3025; wounded falls back to the base gait]
    switch (state) {
        case anim_state::kJogForward:
            if (has(anim_state::kRunForward)) return anim_state::kRunForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kRunForward:
            if (has(anim_state::kJogForward)) return anim_state::kJogForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        // (145/146 have no fallback: the wounded gate only substitutes them when the
        //  adm authors them [orig: @0x4bd70f..0x4bd73f], so the base gait survives.)
        case anim_state::kStop:
            // [orig: Entity_UpdateInfantryAI @0x4b9910 post-pass, kong 154811:
            //  animStateId == 147 && animMap[147] == animMap[0] -> 43]. The
            // org1 selector commits 147 raw and the think's post-commit fix-up
            // converts it; the other callers resolve it here. Data-driven per
            // body: 29 of the 219 retail .adms author anim_stop and play 147;
            // Eindo_R.adm does not.
            break;
        case anim_state::kIdle2:
        case anim_state::kIdle3:
            if (has(anim_state::kIdle)) return anim_state::kIdle;
            break;
        // Stance-clip fallbacks for a model lacking crouch/prone clips [orig: animMap[id] !=
        // animMap[0]]: crouch-walk -> stand walk; prone-walk -> crouch -> stand; stance idle -> idle.
        case anim_state::kWalkCrouchForward:
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kWalkProneForward:
            if (has(anim_state::kWalkCrouchForward)) return anim_state::kWalkCrouchForward;
            if (has(anim_state::kWalkForward)) return anim_state::kWalkForward;
            break;
        case anim_state::kIdleCrouch:
        case anim_state::kIdleProne:
            if (has(anim_state::kIdle)) return anim_state::kIdle;
            break;
        default:
            break;
    }
    // [orig: targetAnimState seeds 43 with no availability test; an unauthored
    //  idle plays slot 0's node through the registration fill]
    return anim_state::kIdle;
}

// The flag-table arbitration both selectors share [orig: org2
// @0x4b7356..0x4b7396; org1 @0x4bd845..0x4bd874]: an uninterruptible current
// (bit 0x4) queues the target to pending; an exit-gated current (0x20) commits
// only a movement-flagged (bit 0) target; else commit now. Returns the state
// the channel commits (the current one when the target was queued).
static int arbitrate_body_state(InfantryState &inf, int resolved) {
    const uint32_t curf = infantry_anim_flags(inf.anim_state);
    if ((curf & 0x4u) != 0) {
        inf.anim_pending = resolved;
        return inf.anim_state;
    }
    if ((curf & 0x20u) == 0 || (infantry_anim_flags(resolved) & 0x1u) != 0) {
        inf.anim_pending = 0;
        return resolved;
    }
    inf.anim_pending = resolved;
    return inf.anim_state;
}

// Commit a resolved org1 target state. The two selectors differ by one test:
// org1 compares the target with the current state first and skips the whole
// arbitration on equality, retaining pending [orig: Entity_UpdateInfantryAI
// @0x4bd837..0x4bd843 cmp/jz loc_4BD87E].
// The channel retarget with the gait->stance transition insert [orig:
// AnimMap_UpdateEntity @0x40b662..0x40b737]: with no deferral armed, a forward
// gait committing to its crouch/prone walk plays the 169-172 transition clip
// first and re-arms the real target as pending for the clip-end promotion,
// gated on the adm actually carrying the clip. The replication replica channel runs
// the same insert through the shared pair map (D-NET-209 / D-INF-23).

void commit_body_state(InfantryState &inf, int resolved) {
    if (resolved < 0) return; // no clips at all: hold the current state
    if (resolved == inf.anim_state) return;
    const int committed = arbitrate_body_state(inf, resolved);
    if (committed != inf.anim_state)
        inf.request_body_animation(committed);
}

// Commit a resolved org2 (player-body) target state. The player arbitrates
// unconditionally against the old state captured before the selection, so a
// re-selection of the current state overwrites a queued pending with itself
// (locked / exit-gated legs) or clears it (the middle leg) [orig:
// Entity_UpdateInfantryPlayerBody old state @0x4b70e5, arbitration
// @0x4b7356..0x4b7396]. The rotor-wash substitution then rewrites the
// committed state [orig: @0x4b73a0..0x4b73e5].
void commit_player_body_state(InfantryState &inf, int resolved,
                              const IRootMotionSource *root_motion, bool wash) {
    if (resolved < 0) return; // no clips at all: hold the current state
    int committed = arbitrate_body_state(inf, resolved);
    if (wash && root_motion) {
        if (committed == anim_state::kWalkForward &&
                root_motion->has_clip(inf.adm_id, anim_state::kWashWalk))
            committed = anim_state::kWashWalk;
        if ((committed == anim_state::kIdle || committed == anim_state::kIdle2) &&
                root_motion->has_clip(inf.adm_id, anim_state::kWashIdle))
            committed = anim_state::kWashIdle;
    }
    // One final channel commit: intermediate land poses must not restart
    // an unchanged wash blend every fourth body tick.
    if (committed != inf.anim_state)
        inf.request_body_animation(committed);
}

int infantry_alerted_idle(int target, int32_t damage_timer, uint8_t alert, bool target_held) {
    // [orig: Entity_UpdateInfantryAI — `cmp [esi+2F8h],0` @0x4BD2F0, `cmp byte [eax+88h],0`
    //  @0x4BD2FD, `cmp edi,2Bh` @0x4BD306, the slot+0x0C pick @0x4BD30F..0x4BD319]
    if ((damage_timer == 0 && alert == 0) || target != anim_state::kIdle) return target;
    return target_held ? anim_state::kIdle3 : anim_state::kIdle2;
}

void AiSystem::infantry_select(AiEntity &e, World &world, int selected_state) {
    InfantryState &inf = e.inf;
    const Entity *self = world.registry.get(e.handle);

    // EMPLACED. A mounted body in a GUNNER seat takes the emplaced state and
    // skips the gait/idle selection entirely:
    //
    //     v99 = entity->parentSlot == 3;
    //     if ( v99 ) {
    //         Entity_AttachToBoneAndUpdateTransform(entity, v455);
    //         entity->pendingAnimStateId = nullptr;
    //         entity->animStateId = 67;
    //         if ( itemDef->gap_86c == 1 && animMap[68] != *animMap ) = 68;
    //         if ( itemDef->gap_86c == 2 && animMap[69] != *animMap ) = 69;
    //     }
    //
    // [orig: Entity_UpdateInfantryAI @0x4b9910, the isInVehicle branch; the same
    //  block appears in the player body @0x4b40e0.] SeatType::Gunner IS retail's
    //  parentSlot 3, and `gap_86c` is the itemDef mount config our ItemSeatSpec
    //  carries as mount_config and promote stamps onto the occupant as
    //  mounted_config.
    //
    // Without this a seated gunner fell through to the not-moving branch and
    // idled: measured on the wire, retail spends 21.3% of all infantry rows in
    // state 67 and we emitted it 0.0% of the time, while idling 47% against
    // retail's 9.7%.
    //
    // DIVERGENCE, declared: retail tests `animMap[68] != *animMap`, i.e. the
    // variant row differs from the default entry. We ask the root-motion source
    // whether the clip exists, which is the same question our anim registry can
    // answer; the bone attach and pendingAnimStateId clear that retail does here
    // belong to the presentation/attach path and are not reproduced in the
    // selector.
    if (self != nullptr && self->mounted && self->mount_type == SeatType::Gunner) {
        int emplaced = anim_state::kEmplaced; // 67
        if (self->mounted_config_valid && root_motion != nullptr) {
            if (self->mounted_config == 1 && root_motion->has_clip(inf.adm_id, 68))
                emplaced = 68;
            if (self->mounted_config == 2 && root_motion->has_clip(inf.adm_id, 69))
                emplaced = 69;
        }
        commit_body_state(inf, infantry_resolve_state(inf.adm_id, emplaced));
        return;
    }
    // The WALK-vs-RUN gate, ported 1:1 from the move-state selection:
    //
    //     v99 = entity->damageTimer == 0;
    //     v327 = 1; targetAnimState = 1;                                  // WALK
    //     if ( !v99 || *((_BYTE *)playerSlotPtr + 136) || entity->wasHit )
    //         { v327 = 149; targetAnimState = 149; }                      // RUN
    //
    // [orig: Entity_UpdateInfantryAI @0x4b9910, the targetAnimState 1/149 block.]
    //
    // RETRACTS the previous reading ("entity[190] || slot+136 || combat-reaction
    // byte +875"). Two of those three terms were wrong, and the first was inert:
    // `alert_timer` (entity[190]) was read here and is WRITTEN NOWHERE in the
    // whole engine, so the term was always false. Retail's first term is damageTimer,
    // which IS live on both legs that raise it -- damage (+10, capped 25) and
    // SIGHT (+12, capped 15) -- so a soldier who merely sees an enemy runs. Ours
    // kept walking, which is why route followers covered a fraction of their
    // channel: node 7 of 30 in 430 s against retail finishing it.
    const bool alerted = inf.damage_timer != 0 ||
                         e.slot.bytes()[AiSlot::kAlertByte] != 0 || inf.was_hit;

    int target = selected_state > 0 ? selected_state : anim_state::kIdle;
    const bool moving = inf.move_mode != 0 && inf.target_dist > 0;
    const bool dragging = infantry_is_dragger(e, world);
    if (moving) {
        // Only a moving selection persists the goal Z [orig: @0x4BD3F7].
        inf.goal_z = inf.move_target[2];
        // A dragger searches and compares turn error while facing away from
        // its travel goal. Restore body heading after the gait choice below.
        // [orig: @0x4BD468, @0x4BD5B1]
        if (dragging) inf.body_heading = io::bam_add(inf.body_heading, INT32_MIN);
        infantry_detour(*this, e, world);
        target = alerted ? anim_state::kRunForward : anim_state::kWalkForward; // [dump 2898-2906]
        // Final-node approach gait. [orig: dump 2907-2924, ported literally incl. the skip]
        if ((inf.move_mode == 2 || inf.move_mode == 3) && inf.at_final_oneshot) {
            bool skip = false;
            if (inf.target_dist > 139264) {
                if (inf.target_dist > 270336) skip = true;
            } else if (inf.arrival_radius < 73728) {
                target = anim_state::kWalkForward;
            }
            if (!skip && inf.arrival_radius < 139264) target = anim_state::kJogForward;
        }
    }

    if (!moving) inf.path_state = 0; // [orig: zero targetDist -> 0x4BD2E9]

    // Turn-in-place overrides only in the moving branch. [orig: @0x4BD4AC]
    if (moving) {
        const int32_t err = abs_bam(opennova::io::bam_sub(inf.target_heading, inf.body_heading));
        if (err > kTurnStopGate) target = anim_state::kStop;
        else if (err > kTurnWalkGate) target = anim_state::kWalkForward;
        if (dragging) {
            inf.body_heading = io::bam_add(inf.body_heading, INT32_MIN);
            inf.target_heading = io::bam_add(inf.target_heading, INT32_MIN);
        }
    }

    auto has = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // Still inside the moving branch, ahead of every substitution below, the
    // jog/run pair falls back: a missing jog runs, a missing run jogs, else
    // walks. [orig: Entity_UpdateInfantryAI @0x4BD5C5..0x4BD61D]
    if (moving) {
        if (target == anim_state::kJogForward && !has(anim_state::kJogForward))
            target = anim_state::kRunForward;
        if (target == anim_state::kRunForward && !has(anim_state::kRunForward))
            target = has(anim_state::kJogForward) ? anim_state::kJogForward
                                                   : anim_state::kWalkForward;
    }

    // Alerted idle: damageTimer or the slot alert byte (NOT wasHit) promotes 43 to the
    // armed idle 49 when the combat pass holds a target in slot[3], else 44.
    // [orig: Entity_UpdateInfantryAI @0x4bd2f0..0x4bd31c]
    target = infantry_alerted_idle(target, inf.damage_timer, e.slot.bytes()[AiSlot::kAlertByte],
                                   e.slot.f[3] != 0);

    // The previous-think move mode the pre_attack reaction reads [orig: @0x4bd356
    // `pad_368[2] = moveMode`, after the idle collapse].
    inf.prev_move_mode = inf.move_mode;

    // [orig: @0x4BD35C..0x4BD393] Drag pose wins after the ordinary gait
    // fallbacks, and suppresses idle gaze without changing the live look yaw.
    if (dragging) {
        inf.aim_override = true;
        inf.aim_pitch = 0;
        const bool walking = target == anim_state::kJogForward ||
                target == anim_state::kRunForward || target == anim_state::kWalkForward;
        const int drag_state = walking ? anim_state::kDraggerWalk : anim_state::kDraggerIdle;
        if (has(drag_state)) target = drag_state;
    }

    // Swimming (Flags 0x8000) swaps the gaits, the idles and the two close
    // attacks for the swim clips [orig: Entity_UpdateInfantryAI
    // @0x4BD635..0x4BD6A3].
    if (self != nullptr && ((self->flags | self->engine_flags) & kEntityFlagDrowning) != 0) {
        if ((target == anim_state::kJogForward || target == anim_state::kRunForward ||
                target == anim_state::kWalkForward) && has(anim_state::kSwimForward))
            target = anim_state::kSwimForward;
        else if ((target == anim_state::kIdle || target == anim_state::kIdle2 ||
                target == anim_state::kIdle3) && has(anim_state::kSwimIdle))
            target = anim_state::kSwimIdle;
        else if ((target == anim_state::kAttack || target == anim_state::kAttack2) &&
                has(anim_state::kSwimAttack))
            target = anim_state::kSwimAttack;
    }

    // Hit flinch: a body hit since the last think swaps its idle for cover_idle (163)
    // or its run/jog for cover_run (164) when the adm authors them, and wasHit is
    // consumed here, on the think cadence. [orig: Entity_UpdateInfantryAI
    // @0x4bd6a7..0x4bd6ee]
    if (inf.was_hit) {
        if ((target == anim_state::kIdle || target == anim_state::kIdle2) &&
            has(anim_state::kCoverIdle))
            target = anim_state::kCoverIdle;
        else if ((target == anim_state::kRunForward || target == anim_state::kJogForward) &&
                 has(anim_state::kCoverRun))
            target = anim_state::kCoverRun;
        inf.was_hit = false;
    }

    // Wounded gaits at half max-health, substituted only when the adm authors the
    // wounded clip: a jogger without wounded_run stays a jogger.
    // [orig: Entity_UpdateInfantryAI @0x4bd6f5..0x4bd744 — def+380 >> 1;
    //  animMap[146]/[145] != *animMap]
    if (e.health <= static_cast<int16_t>(inf.max_health / 2)) {
        if ((target == anim_state::kRunForward || target == anim_state::kJogForward) &&
            has(anim_state::kWoundedRun))
            target = anim_state::kWoundedRun;
        else if (target == anim_state::kWalkForward && has(anim_state::kWoundedWalk))
            target = anim_state::kWoundedWalk;
    }

    // A combat approach (moveMode 1) runs as run_attack (167) [orig:
    // @0x4BD748..0x4BD763]; the moveMode 5 run_away arm after it
    // (@0x4BD765..0x4BD780) has no producer.
    if (inf.move_mode == 1 && target == anim_state::kRunForward && has(anim_state::kRunAttack))
        target = anim_state::kRunAttack;

    // The NPC applies wash before animation arbitration. Unlike the player
    // path, its jog/run can select wash_run. [orig: @0x4BD78D..0x4BD7E6]
    if (world.rotor_wash.nearby_zone(e.self->position.words().data(), 983040)) {
        if (target == anim_state::kWalkForward && has(anim_state::kWashWalk))
            target = anim_state::kWashWalk;
        else if ((target == anim_state::kRunForward || target == anim_state::kJogForward) &&
                has(anim_state::kWashRun))
            target = anim_state::kWashRun;
        else if ((target == anim_state::kIdle || target == anim_state::kIdle2) &&
                has(anim_state::kWashIdle))
            target = anim_state::kWashIdle;
    }

    // A forced state stays numerically selected even if the ADM aliases that
    // slot to RESET. The ordinary gait fallbacks still apply to a later override.
    // [orig: raw forced store @0x4BD266, common arbitration @0x4B9910]
    // A stop (147) is arbitrated raw as well: the post-commit fix-up turns an
    // unauthored stop into 43 after the commit [orig: the arbitration
    // @0x4BD837..0x4BD874 on the raw selection; the fix-up @0x4BE080..0x4BE09A].
    const bool raw = target == anim_state::kStop ||
            (world.script.forced_animation != 0 && target == world.script.forced_animation);
    const int resolved = raw ? target : infantry_resolve_state(inf.adm_id, target);
    commit_body_state(inf, resolved);
}

// The witnessed org2 player-body selection — see the ai.h declaration. One function
// for the local player AND the authority's remote-player path, exactly as the
// original runs the same @0x4b40e0 body for both. Inputs are the already-deposited
// entity+0x12C mirrors (player_moving / dir index / stance / lean bits) plus the
// per-tick weapon mirrors (scope_raised, wpn_run_anim, wpn_force_crouch).
// [orig: Entity_UpdateInfantryPlayerBody @0x4b7183-0x4b7396]
void AiSystem::player_body_select(AiEntity &e, World &world, uint32_t entity_flags,
        uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    // Burn selection bypasses the ordinary movement/swim/lean selector, including
    // the pass that clears the timer. [orig: Entity_UpdateInfantryPlayerBody @0x4B70D9..0x4B717E -> @0x4B729D]
    if (inf.burn_state != 0) {
        const int target = select_infantry_burn(inf, root_motion, true, logic_tick);
        commit_player_body_state(inf, infantry_resolve_state(inf.adm_id, target), root_motion,
                world.rotor_wash.nearby_zone(e.self->position.words().data(), 983040) != 0);
        return;
    }
    auto has = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // No in-air branch here: the original SKIPS this selection while airborne
    // (@0x4b70b8) — the jump block stamps 30/31 and the fall edge stamps 31 (47
    // under a deployed chute, Flags 0x20) directly [orig: @0x4b7ef2/@0x4B7E61].
    int target;
    if (inf.player_moving) {
        inf.idle_counter = 0;                        // [orig: @0x4b719b]
        int base = anim_state::kWalkForward;         // [orig: @0x4b7196]
        if (inf.stance == InfantryState::Stance::kProne)
            base = anim_state::kWalkProneForward;    // [orig: @0x4b71ad]
        else if (inf.stance == InfantryState::Stance::kCrouch)
            base = anim_state::kWalkCrouchForward;   // [orig: @0x4b71bd]
        target = player_directional_state(base, inf.player_move_dir_index); // [orig: @0x4b71c7]
    } else if (inf.stance == InfantryState::Stance::kProne) {
        target = anim_state::kIdleProne;             // [orig: @0x4b722f]
    } else if (inf.stance == InfantryState::Stance::kCrouch) {
        target = anim_state::kIdleCrouch;            // [orig: @0x4b724b]
        // ForceCrouch (0x40000) weapons promote the crouch idle to idle_mortar when
        // the clip exists. [orig: @0x4b723f-0x4b7279 — Entity_CheckWeaponSeatFlags
        // (equipped, 0x40000) then animMap[46] != animMap[0]]
        if (inf.wpn_force_crouch && has(anim_state::kIdleMortar))
            target = anim_state::kIdleMortar;
    } else {
        // Standing idle: 43 until 62 selection passes have elapsed, then 44.
        // [orig: @0x4b727b-0x4b7293 state = 0x2B + (++entity[0x148] >= 0x3E)]
        ++inf.idle_counter;
        target = inf.idle_counter >= 62 ? anim_state::kIdle2 : anim_state::kIdle;
    }

    // Run promotion: pure-forward standing walk only, suppressed while scoped.
    // tier = weight band + run_anim. The band reads entity+0x37C = the LOADOUT
    // WEIGHT the S2C 0x5A apply sums (weaponweight + clips x clipweight over the
    // slots; Terrain_AccumulateSectorScores @0x425220, store @0x425310, called
    // @0x4296f9) — the "no writer" reading behind the old constant 2 (D-INF-16)
    // missed that store: > 0x430000 (67 u) or negative -> 0 (no run), >= 0x210000
    // (33 u) -> 1 (run_2), else 2 (run_3) [orig: @0x4b72aa..0x4b72cf]. Heavy kits
    // jog or walk. tier 1 -> run_2 if authored; tier >= 2 -> run_3 if authored,
    // ELSE the same run_2 test: the run_3-absent compare `jz short loc_4B730A`
    // @0x4b72f8 lands on the tier-1 arm's `animMap[9] != animMap[0]` test
    // @0x4b730a, so a body adm without run_3 runs at run_2 when it has one.
    // [orig: @0x4b729d-0x4b731b; scope Flags&0x10 test @0x4b72e2; run_3 test
    //  @0x4b72f3-0x4b72f8, store @0x4b72fa; run_2 test @0x4b730a, store @0x4b7311]
    if (target == anim_state::kWalkForward && !inf.scope_raised) {
        const int32_t w = inf.loadout_weight_fp16;
        const int band = (w > 0x430000 || w < 0) ? 0 : (w >= 0x210000 ? 1 : 2);
        const int tier = band + inf.wpn_run_anim;
        if (tier >= 2 && has(anim_state::kRun3))
            target = anim_state::kRun3;              // [orig: @0x4b72fa]
        else if (tier >= 1 && has(anim_state::kRun2))
            target = anim_state::kRun2;              // [orig: @0x4b7311]
    }

    // Prone lean rolls from the lean bits; right (bit 7) wins when both are held.
    // The original gates on !(Flags & 0x112002): dead 0x2 and in-air 0x2000 are
    // modeled (health/airborne); the 0x10000 leg (the terrain-slide bit, which
    // infantry_terrain_motion writes) is not applied here (D-INF-17), and the
    // 0x100000 leg is subsumed by the climb block's same-tick state override.
    // [orig: @0x4b731b-0x4b7354]
    if (inf.stance == InfantryState::Stance::kProne && e.health > 0 && !inf.airborne) {
        if (inf.lean_left) target = anim_state::kRollLeft;   // [orig: @0x4b7335]
        if (inf.lean_right) target = anim_state::kRollRight; // [orig: @0x4b734c]
    }

    // SWIM. After the land/wash selection, a body on the float latch
    // (0x8000) that is not dead takes the
    // swim state STRAIGHT -- no availability test, no flag-table arbitration,
    // pending cleared: moving -> the direction index picks 37 forward / 38
    // left / 40 back / 39 right; idle -> 36. The same MoveOrder&7 index the
    // land gaits use, collapsed to four strokes.
    //
    // Retail computes the land state into the same plain field and then
    // OVERWRITES it here; only the final value reaches the anim layer. Our
    // crossfade channel restarts on every intermediate stamp (the same reason
    // the ladder override skips the selection), so the swim override REPLACES
    // the land commit instead of following it -- one commit per pass, the same
    // final value. Committing both thrashed the blend at the 4-tick cadence:
    // the weight never passed ~0.4, the pose looked frozen, and the never-
    // evicted blend SOURCE (the entry tick's jump_loop) kept feeding its
    // root-motion forward step -- the "auto swims forward" defect.
    // [orig: Entity_UpdateInfantryPlayerBody, kong 148422-148452:
    //  `(Flags & 0x8000) && !(Flags & 2)`; switch(MoveOrder & 7) case 0: 37;
    //  1,2: 38; 3,4,5: 40; 6,7: 39; else 36; pendingAnimStateId = 0]
    if ((entity_flags & kEntityFlagDrowning) != 0 && (entity_flags & kEntityFlagDead) == 0) {
        int swim = anim_state::kSwimIdle;
        if (inf.player_moving) {
            switch (inf.player_move_dir_index & 7) {
                case 0: swim = anim_state::kSwimForward; break;
                case 1: case 2: swim = anim_state::kSwimLeft; break;
                case 3: case 4: case 5: swim = anim_state::kSwimBack; break;
                case 6: case 7: swim = anim_state::kSwimRight; break;
                default: break;
            }
        }
        inf.request_body_animation(swim);
        inf.anim_pending = 0;
        return;
    }

    commit_player_body_state(inf, infantry_resolve_state(inf.adm_id, target), root_motion,
            world.rotor_wash.nearby_zone(e.self->position.words().data(), 983040) != 0);
}

} // namespace opennova::world
