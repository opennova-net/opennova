// The infantry combat pass, split out of infantry.cpp by leg (engine/CLAUDE.md
// size ratchet; precedent: infantry_ladder.cpp). Shares bearing_to /
// commit_body_state through infantry_internal.h.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include <base/io/bam.h>
#include <base/io/fixed.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>

namespace opennova::world {

// ----------------------------------------------------------------------------
// The infantry combat pass. [orig: Entity_UpdateInfantryAI @0x4b9910; witness
// docs/world/world-wac-ai-re.md §17.1-17.5 (D-AI-4).] Perception every 32 ticks,
// behavior + aim per authority tick, fire on the .bad anim-event triggers.
// ----------------------------------------------------------------------------

namespace {

// The infantry threat scan: nearest visible enemy over pools 0/1 within the staged
// radius. [orig: Entity_FindNearestThreat @0x4b0990 -> Entity_FindTargets @0x53a610,
// ctx type 7 — the -fwd_dist nearest-first walk; §17.2.] Slice deviations (ledger
// D-AI-4 status): the fresh-corpse (<=16-tick) inclusion, the drowning/far x2
// penalties, the forced-target words, and heat/radar signatures are unmodeled; the
// candidate set is alive enemies, nearest LOS-clear first.
EntityHandle infantry_scan_nearest_threat(AiSystem &sys, World &world, AiEntity &e,
                                          int32_t range) {
    // [orig: @0x4b09a1 — visual radius = min(range/2, 40u); Flags&0x40 -> 0]
    int32_t radius = range >> 1;
    if (radius > 0x280000) radius = 0x280000;
    if (radius <= 0) return EntityHandle{};
    if ((e.slot.f[1] & 1) != 0) return EntityHandle{}; // [orig: aiSlot byte+4 & 1 -> no scan]
    // [orig: @0x4b0a02 — teamless scanners pose as team 2 when slot+4 & 8]
    uint8_t own_team = e.team;
    if (own_team == 0 && (e.slot.f[1] & 8) != 0) own_team = 2;
    const bool scanner_berserk = (e.slot.f[1] & 0x200) != 0;
    if (own_team == 0 && !scanner_berserk) return EntityHandle{};

    EntityHandle best{};
    int64_t best_d2 = static_cast<int64_t>(radius) * radius;
    int32_t best_pos[3] = {};
    for (int pool = 0; pool <= 1; ++pool) {
        const size_t cap = world.registry.pool_capacity(pool);
        for (size_t s = 0; s < cap; ++s) {
            const EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
            if (h == e.handle) continue;
            const Entity *c = world.registry.get(h);
            if (c == nullptr || c->health <= 0) continue;      // in-use + alive
            if ((c->engine_flags & 0x8000001u) != 0) continue; // [orig: flags skip]
            if (c->team == 0 || c->team == own_team) {
                // The shared infantry feed accepts the candidate when either side
                // carries AiSlot[1] bit 0x200. This is the authored Berserk
                // attack-anyone exception; ordinary same-team candidates still skip.
                // [orig: Entity_FindTargets @0x53a7ea..0x53a824]
                const AiEntity *candidate_ai = sys.for_handle(h);
                const bool candidate_berserk = candidate_ai != nullptr &&
                        (candidate_ai->slot.f[1] & 0x200) != 0;
                if (!scanner_berserk && !candidate_berserk)
                    continue;
            }
            const int32_t cpos[3] = {static_cast<int32_t>(c->position.x * io::kFp16One),
                                     static_cast<int32_t>(c->position.y * io::kFp16One),
                                     static_cast<int32_t>(c->position.z * io::kFp16One)};
            const int64_t ddx = static_cast<int64_t>(cpos[0]) - e.pos[0];
            const int64_t ddy = static_cast<int64_t>(cpos[1]) - e.pos[1];
            const int64_t d2 = ddx * ddx + ddy * ddy;
            if (d2 >= best_d2) continue; // nearest-first [orig: -fwd_dist descending sort]
            // Aim-origin -> aim-origin endpoints (Entity_ComputeWeaponFireOrigin
            // at both ends) [orig: Entity_CheckMutualLineOfSight @0x539be0].
            int32_t sa[3];
            sys.weapon_aim_origin(world, e, sa);
            int32_t sb[3];
            sys.weapon_aim_origin(world, *c, sb);
            if (!sys.line_of_sight_clear(world, sa, sb, e.handle, h))
                continue; // LOS last, in order
            best = h;
            best_d2 = d2;
            best_pos[0] = cpos[0]; best_pos[1] = cpos[1]; best_pos[2] = cpos[2];
        }
    }
    (void)best_pos;
    return best;
}

} // namespace

void AiSystem::infantry_combat_think(AiEntity &e, World &world, uint32_t key) {
    InfantryState &inf = e.inf;
    AiSlot &slot = e.slot;
    auto avail = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // damageTimer decays once per tick. [orig: the LABEL_373 block]
    if (inf.damage_timer > 0) --inf.damage_timer;

    // --- Perception (every 32 ticks). [orig: tick & 0x1F == 0; §17.1] ---
    if ((key & 0x1Fu) == 0) {
        int32_t range = slot.f[17]; // sight range, 16.16 [orig: slot+68]
        const bool calm = inf.damage_timer == 0 &&
                          slot.bytes()[AiSlot::kAlertByte] == 0 && !inf.was_hit;
        if (calm) range >>= 1; // calm NPCs see half as far
        // The 4-phase range schedule by (tick>>5)&3: full / 6u / half / 6u.
        const uint32_t phase = (key >> 5) & 3u;
        int32_t staged = std::min(range, 0x60000);
        if (phase == 0) staged = range;
        else if (phase == 2) staged = std::max(range >> 1, std::min(range, 0x60000));

        EntityHandle found = infantry_scan_nearest_threat(*this, world, e, staged);

        // Fallback: the last attacker, enemy + LOS-gated; consumed + cleared every scan.
        // [orig: @0x4bbf20-era block — slot+4 & 1 suppresses retaliation]
        if (!found.valid() && (phase == 0 || !inf.combat_target.valid()) &&
            inf.last_attacker.valid() && (slot.f[1] & 1) == 0) {
            if (const Entity *att = world.registry.get(inf.last_attacker)) {
                if (att->health > 0 && att->team != e.team) {
                    // The mutual-LOS aim-origin endpoints stand in here too —
                    // @0x53b130's own endpoint recipe is unwitnessed.
                    int32_t sa[3];
                    weapon_aim_origin(world, e, sa);
                    int32_t sb[3];
                    weapon_aim_origin(world, *att, sb);
                    if (line_of_sight_clear(world, sa, sb, e.handle, inf.last_attacker))
                        found = inf.last_attacker;
                }
            }
        }
        inf.last_attacker = EntityHandle{};

        if (found.valid()) {
            const Entity *t = world.registry.get(found);
            if (t != nullptr) {
                inf.aim_point[0] = static_cast<int32_t>(t->position.x * io::kFp16One);
                inf.aim_point[1] = static_cast<int32_t>(t->position.y * io::kFp16One);
                inf.aim_point[2] = static_cast<int32_t>(t->position.z * io::kFp16One);
                inf.ai_focus = found;
                if (inf.damage_timer < 15) inf.damage_timer += 12; // stay alerted on sight
                if (inf.combat_target == found) ++inf.same_target_ticks;
                else inf.same_target_ticks = 0;
                inf.combat_target = found;
                slot.f[3] = static_cast<int32_t>(found.packed) + 1; // raw slot[3] write
                                                                    // [orig: @0x4bbf83 —
                                                                    // no refcount here]
                // The authority relation quads ride the scan hit [orig: the
                // Entity_FindNearestThreat authority block @0x4b0a6f..0x4b0ae2].
                if (is_authority) {
                    if (const Entity *se = world.registry.get(e.handle))
                        apply_engage_relations(world, *se, *t);
                }
            }
        } else {
            inf.same_target_ticks = 0;
            inf.combat_target = EntityHandle{};
            slot.f[3] = 0;
        }
        // The own priority-target mark decays each scan; firing re-arms it.
        // [orig: Flags &= ~0x4000 @0x4bbfa4]
        if (Entity *se = world.registry.get(e.handle)) se->engine_flags &= ~kEntityFlagPriorityTarget;
    }

    // --- Behavior + aim (per tick with a live target). [orig: §17.3/§17.5] ---
    Entity *tent =
        inf.combat_target.valid() ? world.registry.get(inf.combat_target) : nullptr;
    if (tent != nullptr && tent->health <= 0) {
        // Target died: play post_attack when close + clear. [orig: anim 151 + focus clear]
        const int64_t ddx = static_cast<int64_t>(tent->position.x * io::kFp16One) - e.pos[0];
        const int64_t ddy = static_cast<int64_t>(tent->position.y * io::kFp16One) - e.pos[1];
        if (ddx * ddx + ddy * ddy < static_cast<int64_t>(196608) * 196608 &&
            avail(anim_state::kPostAttack)) {
            commit_body_state(inf, anim_state::kPostAttack, root_motion);
            inf.ai_focus = EntityHandle{};
        }
        inf.combat_target = EntityHandle{};
        slot.f[3] = 0;
        tent = nullptr;
    }
    if (tent == nullptr) {
        if (inf.combat_move_timer > 0) --inf.combat_move_timer;
        inf.aim_valid = false;
        return;
    }

    const int32_t tpos[3] = {static_cast<int32_t>(tent->position.x * io::kFp16One),
                             static_cast<int32_t>(tent->position.y * io::kFp16One),
                             static_cast<int32_t>(tent->position.z * io::kFp16One)};
    // The witnessed distance metric: sqrt(dx^2 + dy^2 + (dz/2)^2), 16.16.
    // [orig: outPitch[0] = dZ >> 1 into the fsqrt chain @0x4bd0xx]
    const double fdx = static_cast<double>(tpos[0]) - e.pos[0];
    const double fdy = static_cast<double>(tpos[1]) - e.pos[1];
    const double fdz = (static_cast<double>(tpos[2]) - e.pos[2]) * 0.5;
    const int32_t dist16 = static_cast<int32_t>(
        std::min(std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz), 2147418112.0));

    // THE APPROACH / HOLD ARM. Retail runs this when the enemy is NOT inside
    // attack range, or while the move timer is still up:
    //
    //     v7 = enemyDist < slot[15];
    //     if ( !v7 || entity->moveTimer ) {
    //         if ( slot[16] < slot[17] && cmd != 126 ) {
    //             if ( enemyDist > slot[16] && Entity_CheckGroundHeightAtPosition(...) )
    //                  { moveMode = 1; targetDist = enemyDist; arrivalRadius = 655360; }
    //             else if ( animMap[49] != *animMap )
    //                  { targetAnimState = 49; moveMode = 7; targetDist = 0; } } }
    //
    // [orig: Entity_UpdateInfantryAI @0x4b9910 ~2496-2530, which sits BEFORE the
    //  reaction block at ~2530 so the reactions override the anim last.]
    //
    // This arm was previously nested INSIDE the in-attack-range branch as a
    // fallback for "no reaction clip available", which inverted its meaning: an AI
    // whose enemy was out of attack range did nothing at all, so our infantry never
    // closed the distance and stood where they spawned. The two blocks are mutually
    // exclusive by construction -- this one needs (!in_range || timer > 0), the
    // reaction block needs (in_range && timer <= 0) -- so exactly one runs per think
    // and neither can overwrite the other's committed body state.
    //
    // The target-column ground gate [orig: Entity_CheckGroundHeightAtPosition
    // @0x4aff70 -- CalcAverageGroundHeight at the target position (radius 0); true
    // when the column is not the water surface and target.z - ground <= 3.0 u].
    auto ground_reachable = [&](const int32_t p[3]) -> bool {
        if (terrain == nullptr || !terrain->valid()) return true;
        GroundClearance clearance = ground_clearance;
        clearance.has_physics = true; // [orig: the target's entity+368 physics block]
        clearance.use_dead = false;
        const int32_t ground = calc_average_ground_height(*terrain, p, 0, clearance);
        if (ground == INT32_MIN) return true;
        if (terrain->has_water && ground == terrain->water_y) return false;
        return p[2] - ground <= 196608;
    };

    const bool in_attack_range = dist16 < slot.f[15];
    bool run_approach = !in_attack_range || inf.combat_move_timer > 0;

    if (!run_approach) { // inside attack range, hold timer expired
        // The combat reactions ARE the attack anims, availability-gated in the witnessed
        // order (each later hit overrides). The reaction flag re-derives only when this
        // region runs [orig: hasCombatReaction is the region's per-tick local -> +875].
        inf.combat_reaction = false;
        int reaction = 0;
        if (avail(anim_state::kAttack)) reaction = anim_state::kAttack;              // 155
        if (inf.was_hit && avail(anim_state::kCoverAttack)) reaction = anim_state::kCoverAttack; // 165
        if (e.health <= static_cast<int16_t>(inf.max_health / 2) &&
            avail(anim_state::kAttack4)) reaction = anim_state::kAttack4;            // 158
        if (dist16 < 589824 && avail(anim_state::kAttack3)) reaction = anim_state::kAttack3; // 157, 9u
        if (dist16 < 196608) {                                                       // 3 u
            if (avail(anim_state::kAttack2)) reaction = anim_state::kAttack2;        // 156
            if (inf.was_hit && avail(anim_state::kCoverAttack2))
                reaction = anim_state::kCoverAttack2;                                // 166
        }
        // pre_attack (152) wins over every attack clip when the previous think's
        // move mode was idle (0) or route walking (3/4) [orig: @0x4bc23c..0x4bc25e].
        if ((inf.prev_move_mode == 0 || inf.prev_move_mode == 3 || inf.prev_move_mode == 4) &&
            avail(anim_state::kPreAttack))
            reaction = anim_state::kPreAttack;
        // The move timer stamps UNCONDITIONALLY [orig: moveTimer = slot[22]>>4
        // @0x4bc2a6]; a body with no reaction clip then falls into the approach
        // arm [orig: @0x4bc2ba -> LABEL_471].
        inf.combat_move_timer = slot.f[22] >> 4;
        if (reaction != 0) {
            inf.combat_reaction = true;
            inf.move_mode = 7; // hold + fight
            inf.target_dist = 0;
            commit_body_state(inf, infantry_resolve_state(inf.adm_id, reaction), root_motion);
        } else {
            run_approach = true;
        }
    }

    if (run_approach) {
        // A hold-position command (126) never approaches [orig: slot+148 != 126
        // @0x4bc2c5]; the target column must be reachable ground [orig: @0x4bc2e3].
        if (slot.f[16] < slot.f[17] && slot.f[37] != 126) {
            if (dist16 > slot.f[16] && ground_reachable(tpos)) {
                inf.move_mode = 1;
                inf.target_dist = dist16;
                inf.arrival_radius = 655360;
                inf.move_target[0] = tpos[0];
                inf.move_target[1] = tpos[1];
                inf.move_target[2] = tpos[2];
                inf.target_heading =
                        bearing_to(tpos[0] - e.pos[0], tpos[1] - e.pos[1]);
            } else if (avail(anim_state::kIdle3)) {
                inf.move_mode = 7;
                inf.target_dist = 0;
                commit_body_state(inf, anim_state::kIdle3, root_motion);
            }
        }
    }

    // The reload override: empty magazine + a clipsize + the reload clip -> anim 65;
    // while 65 plays the magazine refills. [orig: @0x4bc7xx — targetAnimState = 65,
    // moveMode 0; playing 65 -> word +0x35C = clipsize]
    if (e.profile.clip_size > 0) {
        if (inf.anim_state == anim_state::kReload) {
            inf.magazine = static_cast<int16_t>(e.profile.clip_size);
        } else if (inf.magazine <= 0 && avail(anim_state::kReload)) {
            inf.move_mode = 0;
            inf.target_dist = 0;
            commit_body_state(inf, anim_state::kReload, root_motion);
        }
    }

    // Hold-timer decay, faster when the enemy is close. [orig: LABEL_499]
    if (inf.combat_move_timer > 0) {
        --inf.combat_move_timer;
        if (dist16 < 196608 && inf.combat_move_timer > 0) --inf.combat_move_timer;
        if (dist16 < 655360 && inf.combat_move_timer > 0) --inf.combat_move_timer;
    }

    // --- The aim solution. [orig: §17.5 — lead + sawtooth error] ---
    // Gate: an aim-capable anim (flag bits 0x8 moving-fire / 0x10 attack stance).
    const uint32_t sflags = infantry_anim_flags(inf.anim_state);
    if ((sflags & 0x18u) == 0) {
        inf.aim_valid = false;
        return;
    }
    // Lead the target by its per-tick delta x (dist/0x81074 + 1). The previous-position
    // sample lives in aim_point between think ticks [orig: target savedLivePose +0x80..].
    const int32_t lead = dist16 / 0x81074 + 1;
    int32_t led[3];
    led[0] = tpos[0] + lead * (tpos[0] - inf.aim_point[0]);
    led[1] = tpos[1] + lead * (tpos[1] - inf.aim_point[1]);
    led[2] = tpos[2] + (lead >> 1) * (tpos[2] - inf.aim_point[2]); // vertical lead halved
    inf.aim_point[0] = tpos[0];
    inf.aim_point[1] = tpos[1];
    inf.aim_point[2] = tpos[2];

    // The sawtooth aim error: accuracy A when this target was already fired at
    // (aiRef0 == target), else B; scaled by the difficulty global; two phases.
    // [orig: (119304 * dword_C6EAE8 * acc) >> 5, x (32 - ((tick>>2 [+ tick>>9]) & 0x3F));
    // the prone-in-foliage +40 concealment term needs the foliage-mask seam — D-AI-6.]
    const int32_t acc = (inf.aim_ref0 == inf.combat_target) ? slot.f[10] : slot.f[11];
    const int64_t err_unit =
        (static_cast<int64_t>(119304) * world.script.wac_values.accuracy_spread * acc) >> 5;
    const int32_t err_a = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>(((key >> 2) + (key >> 9)) & 0x3Fu)));
    const int32_t err_b = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>((key >> 2) & 0x3Fu)));

    // The aim EYE rides the muzzle seam — retail's combat-pass aim anchor IS
    // the posed launch bone [orig: Entity_GetAttachmentWorldPosition @0x4b2670
    // on bone +0x366, §21.1]; a row without a resolvable point keeps the raw
    // entity origin (retail's copy @0x4b2767). The horizontal eye components
    // shift with the pose too, as retail's do.
    int32_t eye[3];
    weapon_fire_origin(world, e, eye);
    // The aim TARGET point is the target's aim origin, not its ground origin
    // [orig: §17.5 — target chest point via Entity_ComputeWeaponFireOrigin
    // @0x43b4b0]. The lead stays computed over the raw positions (inf.aim_point
    // is also the movement sample); the origin offset is added on top.
    int32_t t_origin[3];
    weapon_aim_origin(world, *tent, t_origin);
    const double adx = static_cast<double>(led[0]) + (t_origin[0] - tpos[0]) - eye[0];
    const double ady = static_cast<double>(led[1]) + (t_origin[1] - tpos[1]) - eye[1];
    const double adz = static_cast<double>(led[2]) + (t_origin[2] - tpos[2]) - eye[2];
    const double horiz = std::sqrt(adx * adx + ady * ady);
    inf.aim_heading = bearing_to(static_cast<int32_t>(adx), static_cast<int32_t>(ady)) + err_a;
    inf.aim_established = true;
    inf.aim_pitch = static_cast<int32_t>(std::atan2(adz, horiz) * opennova::io::kBamPerRadian) + err_b;
    inf.aim_valid = true;

    // Body re-face when the aim drifts far off the body. [orig: > 262470208 (~22 deg)]
    if (opennova::io::bam_abs(opennova::io::bam_sub(inf.aim_heading, inf.target_heading)) > 262470208)
        inf.target_heading = inf.aim_heading;

    // The walking-fire latch: muzzle within ~5 deg of the solution, inside the attack
    // range, on the slot[22] cadence. [orig: §17.4 — shouldFireSecondary = 1;
    // moveTimer = slot[22] >> 4; def attrib & 4 gate unmodeled]
    if (opennova::io::bam_abs(opennova::io::bam_sub(inf.aim_heading, e.heading)) < 59652320 &&
        dist16 < slot.f[15] && inf.combat_move_timer < (slot.f[22] >> 5)) {
        inf.combat_move_timer = slot.f[22] >> 4;
        inf.fire_secondary_latch = true;
    }
}

void AiSystem::infantry_fire_pass(AiEntity &e, World &world, uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    // The trigger word is consumed on ODD ticks. [orig: v489 & 1 @0x4bf15c]
    if ((logic_tick & 1u) == 0) return;
    if (e.profile.ammo_primary < 0) { // unarmed (the D-AI-5 seed is absent)
        inf.fire_secondary_latch = false;
        return;
    }
    const uint32_t ev = inf.last_events;
    const bool fire_primary = (ev & 0x4u) != 0;   // weapon +0x358, bone +0x365
    const bool fire_c = (ev & 0x10u) != 0;        // weapon +0x35B, bone +0x367
    if ((ev & 0x8u) != 0) inf.fire_secondary_latch = true;
    if (!fire_primary && !fire_c && !inf.fire_secondary_latch) return;

    // The muzzle origin: the launch userpoint on the sim's own posed skeleton,
    // resolved now by the world's muzzle-pose provider, else the raw entity
    // origin — the shared seam helper (the D-AI-6 seam — [orig:
    // Entity_GetAttachmentWorldPosition @0x4b2670 transforms the fire-bone
    // userpoint's local position by the ANIMATED bone matrix, called from the
    // anim-event fire block @0x4bf326..0x4bf425]).
    int32_t origin[3];
    weapon_fire_origin(world, e, origin);
    // Fire along the LAST computed aim, not the body heading: retail's
    // aimHeading is a persistent entity field (set from targetHeading while
    // engaging; only the dragged-body branch at animState 139 assigns it the
    // body heading) and the round leaves along the posed weapon bone that
    // follows it. Discarding the solution on any tick without a fresh one sent
    // 61% of AI rounds off along the body facing - measured live, half of them
    // a full 180 deg from the target.
    // [orig: entity->aimHeading writes in Entity_UpdateInfantryAI @0x4b9910
    //  (= targetHeading while engaging, = bodyHeading only in the drag branch);
    //  the fire site passes the posed bone matrix to
    //  WeaponSlot_FireAndSpawnEffects, never a body-heading scalar]
    const int32_t yaw = inf.aim_established ? inf.aim_heading : e.heading;
    if (inf.aim_established) ++inf.dbg_fires_aimed; else ++inf.dbg_fires_body;
    const int32_t pitch = io::bam_add(
            inf.aim_valid ? inf.aim_pitch : 0, inf.recoil_pitch);

    if (fire_primary)
        fire_ai_round(world, e, origin, yaw, pitch, e.profile.ammo_primary);
    if (fire_c)
        fire_ai_round(world, e, origin, yaw, pitch, e.profile.ammo_primary);
    if (inf.fire_secondary_latch) {
        inf.fire_secondary_latch = false;
        // Only the secondary path spends the magazine [orig: word +0x35C-- @0x4bf45a];
        // an empty one holds this leg until the reload refill (§17.3).
        if (e.profile.clip_size <= 0 || inf.magazine > 0) {
            if (fire_ai_round(world, e, origin, yaw, pitch, e.profile.ammo_primary) &&
                e.profile.clip_size > 0)
                --inf.magazine;
        }
    }
    inf.aim_ref0 = inf.combat_target; // [orig: aiRef0 = slot[3] after the fire block]
}

void AiSystem::infantry_mounted_fire_pass(AiEntity &e, World &world,
                                          uint32_t logic_tick, uint32_t key) {
    (void)logic_tick;
    InfantryState &inf = e.inf;
    Entity *occ = world.registry.get(e.handle);
    if (occ == nullptr || !occ->mounted || occ->mount_type != SeatType::Gunner)
        return;
    Entity *mount = world.registry.get(occ->mount_target);
    if (mount == nullptr) return;
    const bool slot_bound = world.vehicles.bind_use_gun_slot(*occ, *mount);
    if (!inf.combat_target.valid() || !slot_bound) return;

    // The dedicated request runs on its four-tick infantry cadence, then a
    // coordinate/entity stagger admits one 64-tick half-window and rejects the next.
    // [orig: Entity_UpdateInfantryAI @0x4bf4cf..0x4bf4ee]
    if ((key & 3u) != 0) return;

    const Entity *target = world.registry.get(inf.combat_target);
    if (target == nullptr || target->health <= 0) return;

    const int32_t target_pos[3] = {
        static_cast<int32_t>(target->position.x * io::kFp16One),
        static_cast<int32_t>(target->position.y * io::kFp16One),
        static_cast<int32_t>(target->position.z * io::kFp16One)};
    const uint32_t stagger = static_cast<uint32_t>(target_pos[0]) -
            static_cast<uint32_t>(target_pos[1]) + key;
    if ((stagger & 0x40u) != 0) return;

    // UseGun already swapped EquippedSlot to the parent's persistent embedded
    // MountSlot at attach. This request never touches the personal magazine.
    // [orig: Entity_AttachToUseGunSlot @0x546c42..0x546c73]
    const uint8_t adm = mount->primary_weapon_slot_adm;
    const WeaponTableEntry *weapon =
            world.tables.weapons.by_index(adm);
    if (weapon == nullptr || weapon->ammo_index < 0) return;

    const int32_t dx = io::bam_sub(target_pos[0], e.pos[0]);
    const int32_t dy = io::bam_sub(target_pos[1], e.pos[1]);
    // Retail deliberately halves the vertical component before the 3-D range test.
    const int32_t dz = io::bam_sar(io::bam_sub(target_pos[2], e.pos[2]), 1);
    const double fdx = static_cast<double>(dx);
    const double fdy = static_cast<double>(dy);
    const double fdz = static_cast<double>(dz);
    const double distance = std::sqrt(fdx * fdx + fdy * fdy + fdz * fdz);
    if (distance >= static_cast<double>(e.slot.f[15])) return;

    const int32_t target_heading = bearing_to(dx, dy);
    constexpr int32_t kMountedFireArc = 178956960;
    if (opennova::io::bam_abs(io::bam_sub(target_heading, e.heading)) >= kMountedFireArc) return;

    // The AI leg checks only currentAction and queues FIRE. The later global action
    // pump owns timing, recoil, ammo, and the actual round spawn.
    // [orig: currentAction/nextAction @0x4bf583..0x4bf59e]
    if (mount->primary_weapon_slot.current != weapon_action::kIdle) return;
    mount->primary_weapon_slot.next = weapon_action::kFire;
}

// Authority org2 subset: C2S owns pose; the retail player-body function still
// advances animation and its shared movement-collision tail.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B40E0; resolver call @0x4B7CF4]

} // namespace opennova::world
