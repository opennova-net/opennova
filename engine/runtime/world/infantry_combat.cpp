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
#include <runtime/world/collision_force.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>

namespace opennova::world {

// ----------------------------------------------------------------------------
// The infantry combat pass. [orig: Entity_UpdateInfantryAI @0x4b9910; witness
// docs/world/world-wac-ai-re.md §17.1-17.5 (D-AI-4).] Perception every 32 ticks,
// behavior + aim per authority tick, fire on the .bad anim-event triggers.
// ----------------------------------------------------------------------------

namespace {

// Infantry's context-7/8/9 target walk. Range/FOV validation, target policy,
// bounded stable scoring and final LOS follow the shared retail target query.
// [orig: Entity_FindNearestThreat @0x4B0990 -> Entity_FindTargets @0x53A610]
EntityHandle infantry_scan_nearest_threat(AiSystem &sys, World &world, AiEntity &e,
                                          int32_t range) {
    const Entity *self = world.registry.get(e.handle);
    if (self == nullptr || (e.slot.f[1] & 1) != 0) return {};
    const bool scanner_berserk = (e.slot.f[1] & 0x200) != 0;
    // [orig: scanner-team setup @0x4B0A02]
    if (e.team == 0 && !scanner_berserk) return {};
    const uint32_t self_flags = self->flags | self->engine_flags;
    int32_t radius = std::min(range >> 1, 0x280000); // [orig: @0x4B09A1]
    if ((self_flags & 0x40) != 0) radius = 0;
    const int context = self->mounted ? 9 : 7 + ((self->item_attrib & 0x400) != 0);
    int32_t pose[6] = {0, 0, 0, e.heading, e.pitch, e.roll};
    sys.weapon_aim_origin(world, e, pose);
    struct Candidate { EntityHandle handle; int32_t score; };
    std::vector<Candidate> candidates;
    candidates.reserve(128);
    const auto descending = [](const Candidate &a, const Candidate &b) { return a.score > b.score; };
    for (int pool = 0; pool <= 2; ++pool) {
        for (size_t slot = 0; slot < world.registry.pool_capacity(pool); ++slot) {
            const EntityHandle h = EntityHandle::make(pool, static_cast<int>(slot));
            const Entity *c = world.registry.get(h);
            if (c == nullptr || c->item_id == 0 || h == e.handle) continue;
            const uint32_t flags = c->flags | c->engine_flags;
            if ((flags & 0x8000001u) != 0) continue;
            if (((flags & 2) != 0 || c->health <= 0) &&
                    int32_t(world.logic_tick - c->death_tick) > 16) continue;
            if ((flags & kEntityFlagPlayer) != 0 && world.match.outcome().ended) continue;
            const bool selected = self->target_selectors.admits_team(c->net_id, c->group_id);
            const AiEntity *candidate_ai = sys.for_handle(h);
            const bool candidate_berserk = candidate_ai != nullptr && (candidate_ai->slot.f[1] & 0x200) != 0;
            if (!selected && !scanner_berserk && !candidate_berserk &&
                    (c->team == 0 || c->team == e.team)) continue;
            if (!self->target_selectors.allows(c->net_id, c->group_id)) continue;
            // Pool 2's explicitly selected targets bypass the armor-pair gate.
            // [orig: @0x53A878, @0x53AA86, @0x53AC3F]
            if (!(pool == 2 && selected) && c->armor_impact == -1 && c->armor_kz == -1) continue;

            int32_t aim[3], metrics[6];
            sys.weapon_aim_origin(world, *c, aim);
            const uint32_t arc = AiSystem::weapon_relative_metrics(pose, aim, metrics);
            // Context flags 497: radar range within 70 degrees, plus the
            // half-range omnidirectional arm. No heat-signature arm.
            // [orig: Entity_ValidateWeaponTarget @0x53A54A..0x53A5B5]
            const bool radar = arc <= 835132480u && metrics[2] <= range &&
                    (c->radar_sig == 0 || metrics[2] < int32_t(uint32_t(c->radar_sig) << 16));
            if (!radar && metrics[2] > radius) continue;

            // [orig: Weapon_CalcDamageByType @0x539140, cases 7/8/9]
            int32_t score = io::bam_sub(0, metrics[2]);
            const auto twice = [&] { score = io::bam_add(score, score); };
            if (arc > 0x2AAAAA80u) twice();
            if (self->item_type == 3 && e.inf.combat_target == h && e.inf.same_target_ticks > 60) twice();
            if (c->health < 0) twice();
            if (context == 8) {
                if (c->item_type == 1) score >>= 2;
                if (c->item_type == 3) {
                    twice();
                    if ((flags & 0x8000) != 0) twice();
                }
            } else if (c->item_type == 3) {
                if ((flags & 0x8000) != 0) twice();
                if (candidate_ai != nullptr && (candidate_ai->slot.f[1] & 8) != 0)
                    score = INT32_MAX;
            }
            if ((c->item_attrib & 0x40) != 0 && !c->primary_occupant.valid()) score = INT32_MAX;
            score = self->target_selectors.adjust_score(score, c->net_id, c->group_id);
            if (score == INT32_MAX) continue;
            candidates.push_back({h, score});
            // Retail sorts at 128 and resumes insertion at 127, discarding
            // the last entry even if this was the final candidate.
            if (candidates.size() == 128) {
                std::stable_sort(candidates.begin(), candidates.end(), descending);
                candidates.pop_back();
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), descending);
    for (const Candidate &candidate : candidates) {
        const Entity *target = world.registry.get(candidate.handle);
        int32_t aim[3];
        sys.weapon_aim_origin(world, *target, aim);
        if (sys.line_of_sight_clear(world, pose, aim, e.handle, candidate.handle)) return candidate.handle;
        // Alternate ray, 3/8 unit forward, only for the retail flag arm.
        // [orig: Entity_FindTargets @0x53AE4C..0x53AF2B]
        if ((self_flags & 0x800000) != 0) {
            const int32_t zero[3] = {}, offset[3] = {24576, 0, 0};
            int32_t shifted[3];
            collision_matrix_from_euler(e.heading, 0, 0, zero).rotate_point(offset, shifted);
            for (int axis = 0; axis < 3; ++axis) shifted[axis] = io::bam_add(pose[axis], shifted[axis]);
            if (sys.line_of_sight_clear(world, shifted, aim, e.handle, candidate.handle)) return candidate.handle;
        }
    }
    return {};
}

} // namespace

int AiSystem::infantry_combat_think(AiEntity &e, World &world, uint32_t key) {
    InfantryState &inf = e.inf;
    int selected_state = 0;
    inf.combat_reaction = false;
    inf.aim_override = false;
    AiSlot &slot = e.slot;
    auto avail = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // damageTimer decays once per 16-tick think. [orig: LABEL_373 @0x4BBE24]
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
        const bool scan_hit = found.valid();

        // A short-phase miss KEEPS the held target: on phases 1-3 the empty
        // result re-commits slot[3] itself [orig: `test ebp,ebp; jz` @0x4bbee0
        // ..0x4bbee2 sends only phase 0 to the fallback; `mov edi,[ecx+0Ch];
        // test edi,edi; jnz loc_4BBF2A` @0x4bbee4..0x4bbeed carries the held
        // target into the commit]. Only a phase-0 miss, or a body holding no
        // target, reaches the fallback and the clear [orig: @0x4bbf7c..0x4bbf85].
        if (!found.valid() && phase != 0 && inf.combat_target.valid())
            found = inf.combat_target;

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
                // The scan hit seeds the aimPoint (+0x30C..+0x314) with the
                // target's Position [orig: @0x4BBF36..0x4BBF54]; the lead reads
                // the target's own savedLivePose, never this word.
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
                // The authority relation quads ride the scan HIT alone [orig: the
                // Entity_FindNearestThreat authority block @0x4b0a6f..0x4b0ae2 --
                // inside the scan, so neither the re-committed held target nor
                // the lastAttacker fallback reaches it].
                if (is_authority && scan_hit) {
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

    // [orig: @0x4BBF8F..0x4BC047] The nonzero burn branch skips the
    // ENTIRE behavior/aim/gait block, reaching animation arbitration at LABEL_754.
    if (inf.burn_state != 0) {
        selected_state = select_infantry_burn(inf, root_motion, false, key);
        if (inf.burn_state != 0) return selected_state;
    }

    // Behavior and aim share the 16-tick think gate. [orig: §17.3/§17.5]
    Entity *tent =
        inf.combat_target.valid() ? world.registry.get(inf.combat_target) : nullptr;
    if (tent != nullptr && tent->health <= 0) {
        // Target died: play post_attack when close + clear. [orig: anim 151 + focus clear]
        const int64_t ddx = static_cast<int64_t>(tent->position.x * io::kFp16One) - e.pos[0];
        const int64_t ddy = static_cast<int64_t>(tent->position.y * io::kFp16One) - e.pos[1];
        if (ddx * ddx + ddy * ddy < static_cast<int64_t>(196608) * 196608 &&
            avail(anim_state::kPostAttack)) {
            selected_state = anim_state::kPostAttack;
            inf.combat_reaction = true;
            inf.move_mode = 7;
            inf.target_dist = 0;
            inf.ai_focus = EntityHandle{};
        }
        inf.combat_target = EntityHandle{};
        slot.f[3] = 0;
        tent = nullptr;
    }
    if (tent == nullptr) {
        if (inf.combat_move_timer > 0) --inf.combat_move_timer;
        inf.aim_valid = false;
        // Unported here: the attack-stance aim block's no-target arm. Its gate
        // [orig: @0x4bc94c..0x4bc973] carries no target term; with slot+12 empty
        // the block skips the lead [orig: @0x4bca95 -> @0x4bcbeb], re-seats the
        // aim point from savedLivePose for a carried body on tick byte 32
        // [orig: @0x4bcbeb..0x4bccc1], solves toward the retained aim point and
        // runs the same tail [orig: @0x4bcfa3..0x4bcff5].
        return selected_state;
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
        // region runs [orig: hasCombatReaction is the region's per-think local -> +875].
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
            selected_state = reaction;
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
                selected_state = anim_state::kIdle3;
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
            selected_state = anim_state::kReload;
        }
    }

    // Hold-timer decay, faster when the enemy is close. [orig: LABEL_499]
    if (inf.combat_move_timer > 0) {
        --inf.combat_move_timer;
        if (dist16 < 196608 && inf.combat_move_timer > 0) --inf.combat_move_timer;
        if (dist16 < 655360 && inf.combat_move_timer > 0) --inf.combat_move_timer;
    }

    // --- The aim solution. [orig: §17.5 — lead + sawtooth error] ---
    // Retail runs TWO aim blocks over the same lead + sawtooth math, keyed by the
    // anim's g_animStateFlagsTable bits @0x8139e8 (no state carries both):
    //   block 1 [orig: @0x4bc555..0x4bc948] on a flag-0x8 anim (the walks, the
    //     plain idles 43/44): the aim writes, aimFlag @0x4bc894 and the
    //     walking-fire latch; it never writes the detour byte +0x369;
    //   block 2 [orig: @0x4bc94c..0x4bcff5] on a flag-0x10 anim (idle3 49, the
    //     attack clips 155-158, emplaced 67-75): the aim writes, then the body
    //     re-face, the detour-state clear and the mode-7 tail.
    // Both are skipped while the focus entity is the body itself [orig:
    // @0x4bc53d..0x4bc54f -> LABEL_584, re-tested @0x4bc94c..0x4bc952]; the aim
    // heading is re-seated on the target heading ahead of the test
    // [orig: @0x4bc543..0x4bc549].
    inf.aim_heading = inf.target_heading;
    const bool focus_is_self = inf.ai_focus == e.handle;
    const uint32_t sflags = infantry_anim_flags(inf.anim_state);
    // Block 1's gate [orig: @0x4bc555..0x4bc596]: the target (slot+12, held
    // above) and flag 0x8. Its itemDef attrib 0x400 skip [orig: @0x4bc560..0x4bc56a]
    // and its parentSlot 2/5 skip [orig: @0x4bc570..0x4bc582] are unported.
    const bool moving_fire = !focus_is_self && (sflags & 0x8u) != 0;
    // Block 2's gate [orig: @0x4bc94c..0x4bc973]: not self-focused, Flags
    // 0x80000 clear [orig: @0x4bc958], flag 0x10 [orig: @0x4bc96b]. It has no
    // target term; the no-target arm is named at the return above.
    const Entity *self_entity = world.registry.get(e.handle);
    const uint32_t self_flags = self_entity != nullptr
            ? (self_entity->flags | self_entity->engine_flags) : 0u;
    const bool attack_stance = !focus_is_self &&
            (self_flags & kEntityFlagNoEngage) == 0 && (sflags & 0x10u) != 0;
    if (!moving_fire && !attack_stance) {
        inf.aim_valid = false;
        return selected_state;
    }
    // Block 2 re-arms the hold timer for a teamless (slot+4 & 8) body ahead of
    // its aim writes. [orig: @0x4bca44..0x4bca76]
    if (attack_stance && (slot.f[1] & 8) != 0) inf.combat_move_timer = slot.f[22] >> 4;
    // Lead the target by its OWN last-tick displacement: target Position minus
    // its savedLivePose (+0x80..+0x88, which every mover stamps at its head),
    // times lead = dist/0x81074 + 1 (a reciprocal multiply), the vertical by
    // lead >> 1, all wrapping 32-bit; the led point is stored as the entity
    // aimPoint (+0x30C..+0x314). A target never stamped leads by zero.
    // [orig: block 1 @0x4BC6FB..0x4BC798 (`sub ecx,[edi+80h]` @0x4BC72A);
    //  block 2 @0x4BCAFE..0x4BCB69 (`sub ecx,[ebp+80h]` @0x4BCB2D)]
    const int32_t lead = dist16 / 0x81074 + 1;
    const int32_t *saved = tent->saved_live_valid ? tent->saved_live_pos : tpos;
    const auto lead_axis = [](int32_t now, int32_t before, int32_t scale) {
        return opennova::io::bam_add(now, static_cast<int32_t>(
                static_cast<uint32_t>(opennova::io::bam_sub(now, before)) *
                static_cast<uint32_t>(scale)));
    };
    int32_t led[3];
    led[0] = lead_axis(tpos[0], saved[0], lead);
    led[1] = lead_axis(tpos[1], saved[1], lead);
    led[2] = lead_axis(tpos[2], saved[2], lead >> 1); // vertical lead halved
    inf.aim_point[0] = led[0];
    inf.aim_point[1] = led[1];
    inf.aim_point[2] = led[2];

    // The sawtooth aim error: accuracy A when this target was already fired at
    // (aiRef0 == target), else B; scaled by the difficulty global [orig: err =
    // (119304 * dword_C6EAE8 * acc) >> 5 -- block 1 @0x4bc5ea..0x4bc60e, block 2
    // @0x4bc9ce..0x4bc9f2]. Two phases of the think key, both blocks computing
    // both: the HEADING error rides the (key>>2)-only phase [orig: block 1
    // `and ebp,3Fh` @0x4bc630 -> [esp+60h] @0x4bc669; block 2 @0x4bca03..0x4bca11
    // -> ebx], the PITCH error the (key>>2 + key>>9) phase [orig: block 1
    // @0x4bc61e..0x4bc62d -> [esp+20h] @0x4bc64a; block 2 @0x4bca14..0x4bca2c ->
    // [esp+20h]]. The prone-in-foliage +40 concealment term needs the
    // foliage-mask seam -- D-AI-6.
    const int32_t acc = (inf.aim_ref0 == inf.combat_target) ? slot.f[10] : slot.f[11];
    const int64_t err_unit =
        (static_cast<int64_t>(119304) * world.script.wac_values.accuracy_spread * acc) >> 5;
    const int32_t err_heading = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>((key >> 2) & 0x3Fu)));
    const int32_t err_pitch = static_cast<int32_t>(
        err_unit * (32 - static_cast<int32_t>(((key >> 2) + (key >> 9)) & 0x3Fu)));

    // The aim EYE rides the muzzle seam — retail's combat-pass aim anchor IS
    // the posed launch bone [orig: Entity_GetAttachmentWorldPosition @0x4b2670
    // on bone +0x366, §21.1]; a row without a resolvable point keeps the raw
    // entity origin (retail's copy @0x4b2767). The horizontal eye components
    // shift with the pose too, as retail's do.
    int32_t eye[6];
    organic_fire_pose(world, e, 1, eye);
    // The aim TARGET point is the target's aim origin, not its ground origin
    // [orig: §17.5 — target chest point via Entity_ComputeWeaponFireOrigin
    // @0x43b4b0]. The lead is computed over the raw positions above; the origin
    // offset is added on top.
    int32_t t_origin[3];
    weapon_aim_origin(world, *tent, t_origin);
    // The aim delta in wrapping 32-bit integers, as both blocks store it.
    const int32_t adx = opennova::io::bam_sub(
            opennova::io::bam_add(led[0], opennova::io::bam_sub(t_origin[0], tpos[0])), eye[0]);
    const int32_t ady = opennova::io::bam_sub(
            opennova::io::bam_add(led[1], opennova::io::bam_sub(t_origin[1], tpos[1])), eye[1]);
    const int32_t adz = opennova::io::bam_sub(
            opennova::io::bam_add(led[2], opennova::io::bam_sub(t_origin[2], tpos[2])), eye[2]);
    // Bearing = truncated fpatan(dy, dx) in BAM; elevation = truncated fpatan(dz,
    // h) where h is the horizontal length already TRUNCATED to an integer (block
    // 2 clamps it to 2147418112.0 first).
    // [orig: block 1 @0x4BC832..0x4BC85C / @0x4BC89B..0x4BC8BE; block 2's world
    //  arm @0x4BCF2D..0x4BCF92]
    const auto solve_bearing_elevation = [](int32_t dx, int32_t dy, int32_t dz,
            int32_t &bearing_out, int32_t &elevation_out) {
        const double fdx = static_cast<double>(dx);
        const double fdy = static_cast<double>(dy);
        const int32_t horiz = static_cast<int32_t>(
                std::min(std::sqrt(fdx * fdx + fdy * fdy), 2147418112.0));
        bearing_out = bearing_to(dx, dy);
        elevation_out = static_cast<int32_t>(
                std::atan2(static_cast<double>(dz), static_cast<double>(horiz)) *
                opennova::io::kBamPerRadian);
    };
    int32_t bearing = 0;
    int32_t elevation = 0;
    solve_bearing_elevation(adx, ady, adz, bearing, elevation);
    // The heading candidate: bearing + the heading error [orig: block 1
    // `mov ecx,[esp+60h]; sub ecx,eax` @0x4bc861..0x4bc865; block 2 `sub ebx,eax`
    // @0x4bcf71]; the pitch: elevation + the pitch error [orig: block 1
    // @0x4bc8cd..0x4bc8da; block 2 @0x4bcf97..0x4bcf9d].
    int32_t candidate = opennova::io::bam_add(bearing, err_heading);
    int32_t pitch = opennova::io::bam_add(elevation, err_pitch);

    if (attack_stance) {
        // A UseGun body (parentSlot 3) with a parent solves in the PARENT's
        // frame: the delta rotates through the parent's yaw, pitch and roll
        // (22-bit cosines, negated sines, the Entity_TransformWorldToLocal
        // sequence), the local bearing/elevation come out of the rotated vector,
        // and the parent's own yaw and pitch are added back. The gun words
        // (parent angles minus the occupant look) then hold the true local
        // solution on a tilted carrier. Everyone else takes the world-frame arm.
        // [orig: gate `cmp [esi+168h],3` @0x4BCD1C, parent `[esi+16Ch]`
        //  @0x4BCD31..0x4BCD39; trig @0x4BCD3F..0x4BCDB5 (dbl_7C3608 angle
        //  scale, dbl_7C3600 cosines, dbl_7C57B0 = -2^22 sines); rotation
        //  @0x4BCDB9..0x4BCEAA; bearing + parent Yaw @0x4BCECD..0x4BCEF1; pitch +
        //  parent Pitch @0x4BCEF7..0x4BCF1E; the world arm @0x4BCF29..0x4BCF9D]
        const Entity *parent = self_entity != nullptr && self_entity->mounted &&
                self_entity->mount_type == SeatType::Gunner
                ? world.registry.get(self_entity->mount_target) : nullptr;
        if (parent != nullptr) {
            int32_t parent_pos[3];
            int32_t parent_yaw = 0, parent_pitch = 0, parent_roll = 0;
            carrier_pose_fixed(*parent, parent_pos, parent_yaw, parent_pitch, parent_roll);
            const auto cos22 = [](int32_t angle) {
                return static_cast<int32_t>(
                        std::cos(static_cast<double>(angle) * 1.4629627251502471e-9) * 4194304.0);
            };
            const auto neg_sin22 = [](int32_t angle) {
                return static_cast<int32_t>(
                        std::sin(static_cast<double>(angle) * 1.4629627251502471e-9) * -4194304.0);
            };
            const auto mul22 = [](int32_t a, int32_t b) {
                return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 22);
            };
            const int32_t cy = cos22(parent_yaw), cp = cos22(parent_pitch),
                          cr = cos22(parent_roll);
            const int32_t sy = neg_sin22(parent_yaw), sp = neg_sin22(parent_pitch),
                          sr = neg_sin22(parent_roll);
            const int32_t x1 = opennova::io::bam_sub(mul22(cy, adx), mul22(sy, ady));
            const int32_t y1 = opennova::io::bam_add(mul22(sy, adx), mul22(cy, ady));
            const int32_t x2 = opennova::io::bam_sub(mul22(cp, x1), mul22(sp, adz));
            const int32_t z1 = opennova::io::bam_add(mul22(sp, x1), mul22(cp, adz));
            const int32_t y2 = opennova::io::bam_sub(mul22(cr, y1), mul22(sr, z1));
            const int32_t z2 = opennova::io::bam_add(mul22(sr, y1), mul22(cr, z1)); // [orig: @0x4BCEA8]
            solve_bearing_elevation(x2, y2, z2, bearing, elevation);
            candidate = opennova::io::bam_add( // [orig: `add ebx,[edi+10h]` @0x4BCEEE]
                    opennova::io::bam_add(bearing, err_heading), parent_yaw);
            pitch = opennova::io::bam_add( // [orig: `add eax,[edi+14h]` @0x4BCF1B]
                    opennova::io::bam_add(elevation, err_pitch), parent_pitch);
        }
        inf.aim_heading = candidate; // [orig: @0x4BCEF1 / @0x4bcf75]
        inf.aim_pitch = pitch;       // [orig: @0x4BCF1E / @0x4bcf9d]
        inf.aim_established = true;
        inf.aim_override = true;
        inf.aim_valid = true;        // aimFlag [orig: @0x4bcfb1]
        // Block 2's tail [orig: @0x4bcfa3..0x4bcff5]. The body re-face when the
        // aim drifts far off the body (> 262470208, ~22 deg) [orig:
        // @0x4bcfa3..0x4bcfcf]; the detour-state clear, whether or not the
        // re-face fired, ahead of the selector's ai_find_cover_position calls
        // [orig: @0x4bcfdb; the calls @0x4bd490..0x4bd5a4]: an attack-stance
        // body walks straight at its enemy, never at a cached side-step point;
        // then the hold: every move mode but the combat approach (1) and 5
        // collapses to 7 with a zero goal distance [orig: @0x4bcfd5..0x4bcff5 —
        // `cmp al, 5` @0x4bcfd9 and `cmp al, 1` @0x4bcfe4, the decompiler folds
        // the 5 test].
        if (opennova::io::bam_abs(opennova::io::bam_sub(inf.aim_heading, inf.target_heading)) > 262470208)
            inf.target_heading = inf.aim_heading;
        inf.path_state = 0;
        if (inf.move_mode != 1 && inf.move_mode != 5) {
            inf.move_mode = 7;
            inf.target_dist = 0;
        }
        return selected_state;
    }

    // Block 1's body cone: the candidate must lie within ~85 deg of the BODY
    // heading (+0x8C) or the block writes nothing -- no aim, no aimFlag, no
    // latch -- and falls on the fstp pair straight into block 2's gate [orig:
    // `sub eax,[esi+8Ch]` @0x4bc869, cdq/xor/sub, `cmp eax,3C71C6E0h; jge
    // 0x4bc948` @0x4bc874..0x4bc879]. The aim heading keeps the re-seat above.
    if (opennova::io::bam_abs(opennova::io::bam_sub(candidate, inf.body_heading)) >= 0x3c71c6e0) {
        inf.aim_valid = false;
        return selected_state;
    }
    // Inside the cone the heading lands with the heading error added a SECOND
    // time: retail re-reads [esp+60h] into the `lea ebp,[ecx+edx]` that stores
    // +0x2EC [orig: @0x4bc883..0x4bc88e]; aimFlag @0x4bc894; the pitch @0x4bc8da.
    inf.aim_heading = opennova::io::bam_add(candidate, err_heading);
    inf.aim_valid = true;
    inf.aim_pitch = pitch;
    inf.aim_established = true;
    inf.aim_override = true;

    // Block 1's walking-fire latch: muzzle within ~5 deg of the solution, inside
    // the attack range, on the slot[22] cadence. [orig: §17.4, @0x4bc8fa..0x4bc946 —
    // shouldFireSecondary = 1; moveTimer = slot[22] >> 4; the itemDef attrib 4
    // gate @0x4bc930 unmodeled]
    if (opennova::io::bam_abs(opennova::io::bam_sub(inf.aim_heading, e.heading)) < 59652320 &&
        dist16 < slot.f[15] && inf.combat_move_timer < (slot.f[22] >> 5)) {
        inf.combat_move_timer = slot.f[22] >> 4;
        inf.fire_secondary_latch = true;
    }
    return selected_state;
}

void AiSystem::infantry_fire_pass(AiEntity &e, World &world, uint32_t logic_tick) {
    InfantryState &inf = e.inf;
    const auto &ammo = e.profile.organic.ammo;
    Entity *entity = world.registry.get(e.handle);
    const auto shoot = [&](uint8_t id, const int32_t pose[6]) {
        if (entity) entity->equipped_adm_index = id;
        if (id == 0) return;
        // WeaponSlot_FireAndSpawnEffects owns this session gate. The
        // caller's marks and magazine decrement still occur on a client.
        // [orig: @0x53F440, @0x4BF345..0x4BF4AD]
        if (!is_in_session || is_authority) {
            if (inf.aim_established) ++inf.dbg_fires_aimed; else ++inf.dbg_fires_body;
            world.round_sim.fire_npc_ammo(world, e.handle,
                    FixedVec3{pose[0], pose[1], pose[2]}, pose[3], pose[4], id);
        }
        if (entity) {
            entity->equipped_adm_index = 0;
            entity->flags |= kEntityFlagPriorityTarget;
            entity->engine_flags |= kEntityFlagPriorityTarget;
        }
    };
    // Only animation event bits have the odd-tick gate. The walking-fire
    // latch below is consumed every tick. [orig: @0x4BF15C..0x4BF406;
    // primary ammo load @0x4BF326, secondary call @0x4BF425]
    if ((logic_tick & 1u) != 0) {
        if ((inf.last_events & 0x4u) != 0) {
            int32_t pose[6];
            organic_fire_pose(world, e, 0, pose);
            shoot(ammo[0], pose);
            inf.aim_ref0 = inf.combat_target;
        }
        if ((inf.last_events & 0x8u) != 0) inf.fire_secondary_latch = true;
        if ((inf.last_events & 0x10u) != 0) {
            int32_t pose[6];
            organic_fire_pose(world, e, 2, pose);
            shoot(ammo[3], pose);
            inf.aim_ref0 = inf.combat_target;
        }
    }
    if (inf.fire_secondary_latch) {
        inf.fire_secondary_latch = false;
        int32_t pose[6];
        organic_fire_pose(world, e, 1, pose);
        if (ammo[1] != 0) {
            shoot(ammo[1], pose);
            // No empty-magazine or accepted-round test here: selection
            // handles reload later. The original word wraps on decrement.
            // [orig: @0x4BF42A..0x4BF45A]
            inf.magazine = retail_signed_i16(int32_t(uint16_t(inf.magazine)) - 1);
        }
        if (ammo[2] != 0 && ammo[2] != ammo[1]) {
            inf.last_advanced_ammo = ammo[2]; // entity+0x26C, @0x4BF481
            shoot(ammo[2], pose);
        }
        inf.aim_ref0 = inf.combat_target;
    }
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

    // Only a null target ends the request: a held target at zero health keeps
    // drawing FIRE until the next think clears it.
    // [orig: `mov eax,[edi+0Ch]; test eax,eax; jz` @0x4BF4CF..0x4BF4D4]
    const Entity *target = world.registry.get(inf.combat_target);
    if (target == nullptr) return;

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
