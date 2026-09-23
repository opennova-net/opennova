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
#include <runtime/world/collision.h>
#include <runtime/world/collision_force.h>
#include <runtime/world/infantry_internal.h>
#include <runtime/world/world.h>

namespace opennova::world {

// ----------------------------------------------------------------------------
// The infantry combat pass. [orig: Entity_UpdateInfantryAI @0x4b9910; witness
// docs/world/world-wac-ai-re.md §17.1-17.5 (D-AI-4).] Perception every 32 ticks,
// behavior + aim per think, fire on the .bad anim-event triggers.
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
    // A coward (AiSlot[1] & 8) with no team scans as team 2: the think swaps
    // the team byte around the scan [orig: Entity_UpdateInfantryAI
    // @0x4BBEB5..0x4BBEC3, restored @0x4BBED8].
    const uint8_t team = (e.team == 0 && (e.slot.f[1] & 8) != 0) ? uint8_t(2) : e.team;
    // [orig: scanner-team setup @0x4B0A02]
    if (team == 0 && !scanner_berserk) return {};
    const uint32_t self_flags = self->flags | self->engine_flags;
    int32_t radius = std::min(range >> 1, 0x280000); // [orig: @0x4B09A1]
    if ((self_flags & 0x40) != 0) radius = 0;
    const int context = self->mounted ? 9 : 7 + ((self->item_attrib & 0x400) != 0);
    // The range/arc metrics frame is the scanner's own position and
    // orientation (ctx[0] = entity+4) [orig: Entity_FindNearestThreat
    // @0x4B09D0..0x4B09D3; Entity_FindTargets @0x53A67C..0x53A687;
    // compute_relative_position_metrics @0x545723..0x545735]. The LOS rays
    // start at its weapon fire position, which for a UseGun gunner is the
    // gun's own point, not the eye inside the hull; anything but a posed
    // point takes the fire-origin recipe [orig: @0x53A658..0x53A679].
    const int32_t pose[6] = {e.pos[0], e.pos[1], e.pos[2], e.heading, e.pitch, e.roll};
    int32_t origin[3];
    if (sys.weapon_fire_position(world, e, origin) != 1) sys.weapon_aim_origin(world, e, origin);
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
                    (c->team == 0 || c->team == team)) continue;
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
        if (sys.line_of_sight_clear(world, origin, aim, e.handle, candidate.handle)) return candidate.handle;
        // Alternate ray, 3/8 unit forward, only for the retail flag arm.
        // [orig: Entity_FindTargets @0x53AE4C..0x53AF2B]
        if ((self_flags & 0x800000) != 0) {
            const int32_t zero[3] = {}, offset[3] = {24576, 0, 0};
            int32_t shifted[3];
            collision_matrix_from_euler(e.heading, 0, 0, zero).rotate_point(offset, shifted);
            for (int axis = 0; axis < 3; ++axis) shifted[axis] = io::bam_add(origin[axis], shifted[axis]);
            if (sys.line_of_sight_clear(world, shifted, aim, e.handle, candidate.handle)) return candidate.handle;
        }
    }
    return {};
}

// The low dword of the x87 _ftol2 chop: an out-of-range double wraps through
// the 64-bit result instead of C++'s undefined narrowing.
int32_t ftol32(double value) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(value)));
}

// sqrt over wrapping Q16 deltas under the flt_7C19E0 upper clamp, chopped
// (the _ftol2 and the RC=chop fistp sites both truncate).
int32_t clamped_distance(int32_t dx, int32_t dy, int32_t dz) {
    return static_cast<int32_t>(std::min(
            std::sqrt(double(dx) * dx + double(dy) * dy + double(dz) * dz), 2147418112.0));
}

// The registry Position in the Q16 lanes the target's savedLivePose stamp uses
// (vehicle_motor.h carrier_pose_fixed).
void registry_position(const Entity &entity, int32_t out[3]) {
    out[0] = static_cast<int32_t>(entity.position.x * io::kFp16One);
    out[1] = static_cast<int32_t>(entity.position.y * io::kFp16One);
    out[2] = static_cast<int32_t>(entity.position.z * io::kFp16One);
}

// [orig: Entity_CheckGroundHeightAtPosition @0x4AFF70] The PROBE entity's own
// radius-0 ground at a foreign point: the model-aware centre probe of
// Entity_CalcAverageGroundHeight @0x457230 with that function's +0x170 water
// clamp (@0x45731E..0x457331; persons carry no +0x64 brain offsets), refused
// on the water plane, else the point no more than 3 u above the ground.
bool ground_at_position(AiSystem &sys, World &world, const Entity &probe, const int32_t p[3]) {
    const bool occupant_link = probe.primary_occupant.valid();
    int32_t ground;
    if (sys.collision != nullptr && sys.collision->instance_count() != 0) {
        ground = sys.collision->raycast_ground(world, probe.handle, p, 0, 0, 0x10000, 0x300000,
                                               nullptr);
        if (occupant_link && world.env.water_z > ground) ground = world.env.water_z;
    } else {
        const terrain::TerrainHeightField *field =
                world.tables.terrain ? world.tables.terrain : sys.terrain;
        if (field == nullptr || !field->valid()) return true;
        GroundClearance clearance = sys.ground_clearance;
        clearance.has_physics = occupant_link;
        clearance.use_dead = false;
        ground = calc_average_ground_height(*field, p, 0, clearance);
        if (ground == INT32_MIN) return true;
    }
    if (ground == world.env.water_z) return false;     // [orig: @0x4AFFB7..0x4AFFC2]
    return io::bam_sub(p[2], ground) <= 0x30000;        // [orig: @0x4AFFC9..0x4AFFD9]
}

// The aim blocks' sawtooth error unit and its two phases, in the 32-bit
// registers retail computes them in: `imul reg,wac_var_accuracyspread; imul
// reg,1D208h; sar reg,5` [orig: block 1 @0x4bc5ea..0x4bc60e; block 2
// @0x4bc9ce..0x4bc9f2], then (32 - phase) * err with the HEADING phase
// (key>>2)&63 [orig: block 1 `and ebp,3Fh` @0x4bc630 -> [esp+60h] @0x4bc669;
// block 2 @0x4bca03..0x4bca11] and the PITCH phase ((key>>9)+(key>>2))&63
// [orig: block 1 @0x4bc61e..0x4bc62d -> [esp+20h] @0x4bc64a; block 2
// @0x4bca14..0x4bca2c].
void aim_error(int32_t accuracy, int32_t spread, uint32_t key, int32_t &heading_error,
               int32_t &pitch_error) {
    const int32_t unit = io::bam_sar(static_cast<int32_t>(
            static_cast<uint32_t>(accuracy) * static_cast<uint32_t>(spread) * 0x1D208u), 5);
    const int32_t heading_phase = static_cast<int32_t>((key >> 2) & 0x3Fu);
    const int32_t pitch_phase = static_cast<int32_t>(((key >> 9) + (key >> 2)) & 0x3Fu);
    heading_error = static_cast<int32_t>(static_cast<uint32_t>(32 - heading_phase) *
                                         static_cast<uint32_t>(unit));
    pitch_error = static_cast<int32_t>(static_cast<uint32_t>(32 - pitch_phase) *
                                       static_cast<uint32_t>(unit));
}

// Both aim blocks' eye: the +0x366 launch point, except that a led point within
// 3 u of the body (planar) moves the eye only a quarter of the way from the body
// origin to that point on X/Y [orig: block 1 @0x4BC7B1..0x4BC825 (clamped h);
// block 2 @0x4BCB7B..0x4BCD0C (unclamped h)].
void aim_eye(const int32_t body[3], const int32_t muzzle[3], const int32_t aim[3], bool clamp,
             int32_t eye[3]) {
    const double hx = double(io::bam_sub(aim[0], body[0]));
    const double hy = double(io::bam_sub(aim[1], body[1]));
    double h = std::sqrt(hx * hx + hy * hy);
    if (clamp) h = std::min(h, 2147418112.0);
    if (ftol32(h) > 0x30000) {
        std::copy_n(muzzle, 3, eye);
        return;
    }
    eye[0] = io::bam_add(body[0], io::bam_sar(io::bam_sub(muzzle[0], body[0]), 2));
    eye[1] = io::bam_add(body[1], io::bam_sar(io::bam_sub(muzzle[1], body[1]), 2));
    eye[2] = muzzle[2];
}

// The Q16 Position of a body: the motor's own lane when it has one.
void body_position(const AiSystem &sys, const Entity &entity, int32_t out[3]) {
    if (const AiEntity *body = sys.for_handle(entity.handle)) {
        std::copy_n(body->pos, 3, out);
        return;
    }
    registry_position(entity, out);
}

// The target lead both blocks store as the aimPoint: the target's own last-tick
// displacement (Position minus its savedLivePose) times dist/528500 + 1, the
// vertical at half that, over the FULL 3-D distance chopped by fistp; the
// ComputeWeaponFireOrigin result both blocks compute next is never read.
// [orig: block 1 @0x4BC697..0x4BC798 (the dead call @0x4BC720); block 2
//  @0x4BCA9D..0x4BCB69 (the dead call @0x4BCB23); `mov eax,7EFAD919h; mul ecx;
//  shr edx,12h; add 1` = the /528500 reciprocal]
void lead_target(const Entity &target, const int32_t target_pos[3], const int32_t body[3],
                 int32_t aim_point[3]) {
    const uint32_t distance = static_cast<uint32_t>(clamped_distance(
            io::bam_sub(target_pos[0], body[0]), io::bam_sub(target_pos[1], body[1]),
            io::bam_sub(target_pos[2], body[2])));
    const uint32_t lead = distance / 528500u + 1u;
    const int32_t *saved = target.saved_live_valid ? target.saved_live_pos : target_pos;
    for (int axis = 0; axis < 3; ++axis) {
        const uint32_t scale = axis == 2 ? (lead >> 1) : lead;
        aim_point[axis] = io::bam_add(target_pos[axis], static_cast<int32_t>(
                static_cast<uint32_t>(io::bam_sub(target_pos[axis], saved[axis])) * scale));
    }
}

} // namespace

// The entity LOS [orig: Entity_CheckLineOfSightTerrainAndEntities @0x53B130]
// over the shared collision world, or its terrain leg alone when the embedder
// wired no model world.
bool infantry_entity_los(AiSystem &ai, World &world, EntityHandle a, EntityHandle b,
                         const int32_t start[3], const int32_t end[3], int32_t height_offset,
                         bool all_types) {
    if (ai.collision != nullptr)
        return ai.collision->entity_los_clear(world, a, b, start, end, height_offset, all_types);
    CollisionWorld terrain_query;
    terrain_query.terrain = world.tables.terrain ? world.tables.terrain : ai.terrain;
    return terrain_query.entity_los_clear(world, a, b, start, end, height_offset, all_types);
}

int AiSystem::infantry_combat_think(AiEntity &e, World &world, uint32_t key) {
    InfantryState &inf = e.inf;
    // The selected-state local carries on from the command legs: their 147 /
    // 140 proposal is what the combat legs override or keep [orig: seeded 43
    // @0x4BAA90 (our 0 = the selector's 43), board writes @0x4BB72A /
    // @0x4BB80E / @0x4BB835].
    int selected_state = inf.board_anim >= 0 ? inf.board_anim : 0;
    inf.board_anim = -1;
    // The think's hasReaction and aim-override frame locals, zeroed at the
    // motor head [orig: Entity_UpdateInfantryAI @0x4B99BF / @0x4B99C6].
    inf.combat_reaction = false;
    inf.aim_override = false;
    AiSlot &slot = e.slot;
    Entity *self_entity = world.registry.get(e.handle);
    auto avail = [&](int s) {
        return root_motion != nullptr && root_motion->has_clip(inf.adm_id, s);
    };

    // damageTimer decays only on a think whose staggered key is a multiple of
    // 64, not on every think [orig: the key & 0x3F local
    // @0x4BA9D8..0x4BA9DB; the decay @0x4BBE24..0x4BBE38].
    if ((key & 63u) == 0 && inf.damage_timer != 0)
        inf.damage_timer = io::bam_sub(inf.damage_timer, 1);

    // --- Perception (every 32 ticks). [orig: tick & 0x1F == 0; §17.1] ---
    if ((key & 0x1Fu) == 0) {
        int32_t range = slot.f[17]; // sight range, 16.16 [orig: slot+68]
        const bool calm = inf.damage_timer == 0 &&
                          slot.bytes()[AiSlot::kAlertByte] == 0 && !inf.was_hit;
        if (calm) range = io::bam_sar(range, 1); // calm NPCs see half as far
        // The 4-phase range schedule by (tick>>5)&3: full / 6u / half / 6u.
        const uint32_t phase = (key >> 5) & 3u;
        int32_t staged = std::min(range, 0x60000);
        if (phase == 0) staged = range;
        else if (phase == 2) staged = std::max(io::bam_sar(range, 1), std::min(range, 0x60000));

        EntityHandle found = infantry_scan_nearest_threat(*this, world, e, staged);
        const bool scan_hit = found.valid();

        // A short-phase miss KEEPS the held target, dead or alive: on phases 1-3
        // the empty result re-commits slot[3] itself [orig: `test ebp,ebp; jz`
        // @0x4bbee0..0x4bbee2 sends only phase 0 to the fallback; `mov
        // edi,[ecx+0Ch]; test edi,edi; jnz loc_4BBF2A` @0x4bbee4..0x4bbeed carries
        // the held target into the commit]. Only a phase-0 miss, or a body holding
        // no target, reaches the fallback and the clear [orig: @0x4bbf7c..0x4bbf85].
        if (!found.valid() && phase != 0 && inf.combat_target.valid())
            found = inf.combat_target;

        // Fallback: the last attacker, consumed + cleared every scan. There is no
        // health test: a different team byte, the blind bit clear and a clear
        // origin-to-origin entity LOS at height 0 with allTypes 0 admit it.
        // [orig: @0x4BBEEF..0x4BBF24 — pushes @0x4BBF0A..0x4BBF16, the call
        //  @0x4BBF18, its result test @0x4bbf20]
        if (!found.valid() && (phase == 0 || !inf.combat_target.valid()) &&
            inf.last_attacker.valid() && (slot.f[1] & 1) == 0) {
            if (const Entity *att = world.registry.get(inf.last_attacker)) {
                if (att->team != e.team) {
                    int32_t att_pos[3];
                    registry_position(*att, att_pos);
                    if (infantry_entity_los(*this, world, e.handle, inf.last_attacker, e.pos,
                                            att_pos, 0, false))
                        found = inf.last_attacker;
                }
            }
        }
        inf.last_attacker = EntityHandle{};

        // A destroyed entity has no pointer left to commit: the shared destroy
        // clears every organic slot[3] that still names it [orig:
        // Entity_ClearAllReferences @0x465670 (the +0x68 slot+0x0C clears
        // @0x4656DB..0x4656E7 / @0x465751..0x46575D)].
        const Entity *t = found.valid() ? world.registry.get(found) : nullptr;
        if (t != nullptr) {
            // The scan hit seeds the aimPoint (+0x30C..+0x314) with the
            // target's Position [orig: @0x4BBF36..0x4BBF54]; the lead reads
            // the target's own savedLivePose, never this word.
            registry_position(*t, inf.aim_point);
            inf.ai_focus = found;
            if (inf.damage_timer < 15) inf.damage_timer += 12; // stay alerted on sight
            if (inf.combat_target == found) ++inf.same_target_ticks;
            else inf.same_target_ticks = 0;
            inf.combat_target = found;
            slot.f[3] = static_cast<int32_t>(found.packed) + 1; // raw slot[3] write
                                                                // [orig: @0x4BBF85 —
                                                                // no refcount here]
            // The authority relation quads ride the scan HIT alone [orig: the
            // Entity_FindNearestThreat authority block @0x4b0a6f..0x4b0ae2 --
            // inside the scan, so neither the re-committed held target nor
            // the lastAttacker fallback reaches it].
            if (is_authority && scan_hit && self_entity != nullptr)
                apply_engage_relations(world, *self_entity, *t);
        } else {
            inf.same_target_ticks = 0;
            inf.combat_target = EntityHandle{};
            slot.f[3] = 0;
        }
        // The own priority-target mark decays each scan; firing re-arms it.
        // [orig: `and dword ptr [esi+24h],0FFFFBFFFh` @0x4BBF88]
        if (self_entity != nullptr) self_entity->engine_flags &= ~kEntityFlagPriorityTarget;
    }

    // [orig: @0x4BBF8F..0x4BC047] The nonzero burn branch skips the ENTIRE
    // behavior/aim/gait block, reaching the arbitration head [orig:
    // Entity_UpdateInfantryAI @0x4BD7FD, the `jnz` @0x4BC04E].
    // A stage without its clip leaves the proposal standing [orig: the local is
    // written only under the clip tests @0x4BBFAF / @0x4BBFD3 / @0x4BBFF7 /
    // @0x4BC01B].
    if (inf.burn_state != 0) {
        const int burn = select_infantry_burn(inf, root_motion, false, key);
        if (burn != 0) selected_state = burn;
        if (inf.burn_state != 0) return selected_state;
    }

    // The held slot[3] target, alive or a fresh corpse: a dead target stays
    // held until a phase-0 perception miss, so the reaction chain and both aim
    // blocks keep working it (post_attack is that chain's last link).
    const Entity *tent =
        inf.combat_target.valid() ? world.registry.get(inf.combat_target) : nullptr;
    if (tent == nullptr && inf.combat_target.valid()) {
        // Destroyed since the scan [orig: Entity_ClearAllReferences @0x465670].
        inf.combat_target = EntityHandle{};
        slot.f[3] = 0;
    }
    // The target-distance local: 1000 u until a target is measured [orig: outDy
    // seeded @0x4BAA88].
    int32_t target_distance = 0x3E80000;
    int32_t tpos[3] = {};
    if (tent != nullptr) {
        registry_position(*tent, tpos);
        // The witnessed distance metric: sqrt(dx^2 + dy^2 + (dz sar 1)^2), 16.16.
        // [orig: @0x4BC0E1..0x4BC141]
        target_distance = clamped_distance(io::bam_sub(tpos[0], e.pos[0]),
                                           io::bam_sub(tpos[1], e.pos[1]),
                                           io::bam_sar(io::bam_sub(tpos[2], e.pos[2]), 1));
        // THE REACTION CHAIN: inside attack range with the hold timer run out.
        // The combat reactions ARE the attack anims, availability-gated in the
        // witnessed order (each later hit overrides), post_attack last.
        // [orig: `cmp eax,[ecx+3Ch]; jge` @0x4BC13E; `cmp [esi+148h],0; jnz`
        //  @0x4BC14B; the chain @0x4BC158..0x4BC297]
        bool run_approach = target_distance >= slot.f[15] || inf.combat_move_timer != 0;
        if (!run_approach) {
            const auto react = [&](int state) {
                selected_state = state;
                inf.combat_reaction = true;
            };
            if (avail(anim_state::kAttack)) react(anim_state::kAttack);                // 155
            if (inf.was_hit && avail(anim_state::kCoverAttack))
                react(anim_state::kCoverAttack);                                        // 165
            if (e.health <= static_cast<int16_t>(inf.max_health / 2) &&
                avail(anim_state::kAttack4)) react(anim_state::kAttack4);             // 158
            if (target_distance < 589824 && avail(anim_state::kAttack3))
                react(anim_state::kAttack3);                                            // 157, 9u
            if (target_distance < 196608) {                                             // 3 u
                if (avail(anim_state::kAttack2)) react(anim_state::kAttack2);          // 156
                if (inf.was_hit && avail(anim_state::kCoverAttack2))
                    react(anim_state::kCoverAttack2);                                   // 166
            }
            // pre_attack (152) wins over every attack clip when the previous think's
            // move mode was idle (0) or route walking (3/4) [orig: @0x4bc23c..0x4bc25e].
            if ((inf.prev_move_mode == 0 || inf.prev_move_mode == 3 || inf.prev_move_mode == 4) &&
                avail(anim_state::kPreAttack))
                react(anim_state::kPreAttack);
            // A dead target within 3 u plays post_attack and drops the focus
            // [orig: `cmp word [ebp+11Eh],0; jg` @0x4BC269..0x4BC297].
            if (retail_signed_i16(tent->health) <= 0 && target_distance < 196608 &&
                avail(anim_state::kPostAttack)) {
                react(anim_state::kPostAttack);
                inf.ai_focus = EntityHandle{};
            }
            // The move timer stamps UNCONDITIONALLY [orig: moveTimer = slot[22]>>4
            // @0x4bc2a6]; a body with no reaction clip then falls into the approach
            // arm [orig: @0x4bc2ba, falling into the approach arm @0x4BC2C2].
            inf.combat_move_timer = io::bam_sar(slot.f[22], 4);
            run_approach = !inf.combat_reaction;
        }
        // THE APPROACH / HOLD ARM, when the enemy is out of attack range, the
        // hold timer is still up, or no reaction clip exists. A hold-position
        // command (126) never approaches [orig: slot+148 != 126 @0x4bc2c5]; the
        // target column must be reachable ground under the TARGET's own probe
        // [orig: `cmp edi,eax; jle` @0x4bc2e3, then `push ebp; push ebx; call
        //  Entity_CheckGroundHeightAtPosition` @0x4BC2E7..0x4BC2E9].
        // [orig: @0x4BC2C2..0x4BC346]
        if (run_approach && slot.f[16] < slot.f[17] && slot.f[37] != 126) {
            if (target_distance > slot.f[16] && ground_at_position(*this, world, *tent, tpos)) {
                inf.move_mode = 1;
                inf.target_dist = target_distance;
                inf.arrival_radius = 655360;
                inf.move_target[0] = tpos[0];
                inf.move_target[1] = tpos[1];
                inf.move_target[2] = tpos[2];
            } else if (avail(anim_state::kIdle3)) {
                inf.move_mode = 7;
                inf.target_dist = 0;
                selected_state = anim_state::kIdle3;
            }
        }
    } else {
        // THE NO-TARGET ARM: a still-alerted body with a focus walks to the last
        // aim point (the last sighting or spotted position), watches a dead focus
        // with post_attack, and otherwise scans in place with idle_2.
        // [orig: @0x4BC34B..0x4BC4BE]
        const Entity *focus = inf.ai_focus.valid() ? world.registry.get(inf.ai_focus) : nullptr;
        if (inf.damage_timer == 0 || focus == nullptr) {
            inf.ai_focus = EntityHandle{}; // [orig: @0x4BC4BE]
        } else if (slot.f[16] < slot.f[17]) {
            if (focus->handle != e.handle && retail_signed_i16(focus->health) <= 0) {
                // A dead focus: watch the corpse when post_attack exists, else
                // drop it [orig: @0x4BC37E..0x4BC3A8].
                if (avail(anim_state::kPostAttack)) {
                    registry_position(*focus, inf.aim_point);
                } else {
                    inf.ai_focus = EntityHandle{};
                    focus = nullptr;
                }
            }
            // The (dz sar 1) distance to the aim point, fistp-chopped, compared
            // UNSIGNED against 2 u [orig: @0x4BC3AE..0x4BC438 — `jbe`].
            const int32_t distance = clamped_distance(
                    io::bam_sub(inf.aim_point[0], e.pos[0]),
                    io::bam_sub(inf.aim_point[1], e.pos[1]),
                    io::bam_sar(io::bam_sub(inf.aim_point[2], e.pos[2]), 1));
            if (static_cast<uint32_t>(distance) > 0x20000u && self_entity != nullptr &&
                ground_at_position(*this, world, *self_entity, inf.aim_point)) {
                // [orig: @0x4BC448..0x4BC469]
                inf.move_mode = 2;
                inf.target_dist = distance;
                inf.arrival_radius = 0x20000;
                std::copy_n(inf.aim_point, 3, inf.move_target);
            } else {
                // [orig: @0x4BC46F..0x4BC4B4]
                selected_state = anim_state::kIdle2;
                if (focus != nullptr && focus->handle != e.handle &&
                    retail_signed_i16(focus->health) <= 0 && avail(anim_state::kPostAttack)) {
                    selected_state = anim_state::kPostAttack;
                    inf.ai_focus = EntityHandle{};
                }
                inf.move_mode = 8;
                inf.target_dist = 0;
            }
        }
    }

    // The hold-timer tail, on every path: one tick, and for a non-coward body
    // an extra one in idle_3 and one each under 3 u / 10 u of the target (the
    // 1000 u no-target distance never qualifies). [orig: @0x4BC4C4..0x4BC537]
    if (inf.combat_move_timer != 0) inf.combat_move_timer = io::bam_sub(inf.combat_move_timer, 1);
    if ((slot.f[1] & 8) == 0) {
        if (inf.anim_state == anim_state::kIdle3 && inf.combat_move_timer != 0)
            inf.combat_move_timer = io::bam_sub(inf.combat_move_timer, 1);
        if (target_distance < 196608 && inf.combat_move_timer != 0)
            inf.combat_move_timer = io::bam_sub(inf.combat_move_timer, 1);
        if (target_distance < 655360 && inf.combat_move_timer != 0)
            inf.combat_move_timer = io::bam_sub(inf.combat_move_timer, 1);
    }

    // --- The aim solution. [orig: §17.5 — lead + sawtooth error] ---
    // Retail runs TWO aim blocks over the same lead + sawtooth math, keyed by the
    // anim's g_animStateFlagsTable bits @0x8139e8 (no state carries both):
    //   block 1 [orig: @0x4bc555..0x4bc948] on a flag-0x8 anim (the walks, the
    //     plain idles 43/44) with a target: the aim writes, aimFlag @0x4bc894
    //     and the walking-fire latch; it never writes the detour byte +0x369;
    //   block 2 [orig: @0x4bc94c..0x4bcff5] on a flag-0x10 anim (idle3 49, the
    //     attack clips 155-158, emplaced 67-75), target or not: the aim writes,
    //     then the body re-face, the detour-state clear and the mode-7 tail.
    // Both are skipped while the focus entity is the body itself [orig:
    // @0x4bc53d..0x4bc54f -> @0x4BCFF5, re-tested @0x4bc94c..0x4bc952]; the aim
    // heading is re-seated on the target heading ahead of the test
    // [orig: @0x4bc543..0x4bc549]. aimFlag (+0x360) is only ever SET here.
    inf.aim_heading = inf.target_heading;
    const bool focus_is_self = inf.ai_focus == e.handle;
    const uint32_t sflags = infantry_anim_flags(inf.anim_state);
    const uint32_t self_flags = self_entity != nullptr
            ? (self_entity->flags | self_entity->engine_flags) : 0u;
    const int32_t spread = world.script.wac_values.accuracy_spread;
    // Block 1's gate [orig: @0x4bc555..0x4bc596]: the target (slot+12), flag
    // 0x8, and it skips a MISSILE-attrib body and a controller/driver seat
    // [orig: itemDef attrib 0x400 @0x4bc560..0x4bc56a; parentSlot 2/5
    // @0x4BC570..0x4BC582].
    const bool control_seat = self_entity != nullptr && self_entity->mounted &&
            is_vehicle_control_seat(self_entity->mount_type);
    const bool block_one = !focus_is_self && tent != nullptr && (sflags & 0x8u) != 0 &&
            (self_entity == nullptr || (self_entity->item_attrib & 0x400u) == 0) &&
            !control_seat;
    if (block_one) {
        // [orig: accuracy pick @0x4BC5DB..0x4BC5FE — slot+0x28 when aiRef0 is the
        //  held target, slot+0x2C otherwise]
        const int32_t acc = (inf.aim_ref0 == inf.combat_target) ? slot.f[10] : slot.f[11];
        int32_t err_heading = 0, err_pitch = 0;
        aim_error(acc, spread, key, err_heading, err_pitch);
        int32_t muzzle[6];
        organic_fire_pose(world, e, 1, muzzle); // [orig: @0x4BC692, launch byte +0x366]
        lead_target(*tent, tpos, e.pos, inf.aim_point);
        int32_t eye[3];
        aim_eye(e.pos, muzzle, inf.aim_point, true, eye);
        const int32_t adx = io::bam_sub(inf.aim_point[0], eye[0]);
        const int32_t ady = io::bam_sub(inf.aim_point[1], eye[1]);
        const int32_t adz = io::bam_sub(inf.aim_point[2], eye[2]);
        // The heading candidate: the chopped bearing [orig: @0x4BC832..0x4BC85C,
        // dbl_7C57B8 negated] plus the heading error [orig: `mov ecx,[esp+60h];
        // sub ecx,eax` @0x4bc861..0x4bc865].
        const int32_t candidate = io::bam_add(
                ftol32(std::atan2(double(ady), double(adx)) * io::kBamPerRadian), err_heading);
        // The body cone: the candidate must lie within ~85 deg of the BODY heading
        // (+0x8C) or the block writes nothing -- no aim, no aimFlag, no latch
        // [orig: `sub eax,[esi+8Ch]` @0x4bc869, cdq/xor/sub, `cmp eax,3C71C6E0h;
        //  jge 0x4bc948` @0x4bc874..0x4bc879].
        if (io::bam_abs(io::bam_sub(candidate, inf.body_heading)) < 0x3c71c6e0) {
            // Inside the cone the heading lands with the heading error added a
            // SECOND time: retail re-reads [esp+60h] into the `lea ebp,[ecx+edx]`
            // that stores +0x2EC [orig: @0x4bc883..0x4bc88e]; aimFlag @0x4bc894.
            inf.aim_heading = io::bam_add(candidate, err_heading);
            inf.aim_valid = true;
            // The elevation over the truncated, UNCLAMPED planar length
            // [orig: @0x4BC89B..0x4BC8BE]; the pitch store @0x4bc8da.
            const int32_t horiz = ftol32(std::sqrt(double(adx) * adx + double(ady) * ady));
            inf.aim_pitch = io::bam_add(
                    ftol32(std::atan2(double(adz), double(horiz)) * io::kBamPerRadian),
                    err_pitch);
            inf.aim_established = true;
            inf.aim_override = true; // [orig: the override local set @0x4BC8E0]
            // The walking-fire latch: the yaw within ~5 deg of the solution, the
            // eye-to-aim-point (dz sar 1) distance inside the attack range, a
            // NOMOVESHOOT-clear def and the slot[22] cadence. [orig: §17.4,
            // @0x4bc8c3..0x4bc946 — the def attrib 4 test @0x4BC930;
            // shouldFireSecondary = 1; moveTimer = slot[22] >> 4]
            const int32_t latch_distance = clamped_distance(adx, ady, io::bam_sar(adz, 1));
            if (io::bam_abs(io::bam_sub(inf.aim_heading, e.heading)) < 59652320 &&
                inf.combat_move_timer < io::bam_sar(slot.f[22], 5) &&
                latch_distance < slot.f[15] &&
                (self_entity == nullptr || (self_entity->item_attrib & 0x4u) == 0)) {
                inf.combat_move_timer = io::bam_sar(slot.f[22], 4);
                inf.fire_secondary_latch = true;
            }
        }
    }
    // Block 2's gate [orig: @0x4bc94c..0x4bc973]: not self-focused, Flags
    // 0x80000 clear [orig: @0x4bc958], flag 0x10 [orig: @0x4bc96b]. It has no
    // target term.
    const bool attack_stance = !focus_is_self &&
            (self_flags & kEntityFlagNoEngage) == 0 && (sflags & 0x10u) != 0;
    if (attack_stance) {
        // [orig: accuracy pick @0x4BC9C3..0x4BC9E2 — against the held slot[3],
        //  null without a target]
        const int32_t acc = (inf.aim_ref0 == inf.combat_target) ? slot.f[10] : slot.f[11];
        int32_t err_heading = 0, err_pitch = 0;
        aim_error(acc, spread, key, err_heading, err_pitch);
        // Block 2 re-arms the hold timer for a coward (slot+4 & 8) body ahead of
        // its aim writes. [orig: @0x4bca44..0x4bca76]
        if ((slot.f[1] & 8) != 0) inf.combat_move_timer = io::bam_sar(slot.f[22], 4);
        int32_t muzzle[6];
        organic_fire_pose(world, e, 1, muzzle); // [orig: @0x4BCA8D]
        const Entity *parent_ride = self_entity != nullptr && self_entity->mounted
                ? world.registry.get(self_entity->mount_target) : nullptr;
        if (tent != nullptr) {
            lead_target(*tent, tpos, e.pos, inf.aim_point);
        } else if (parent_ride != nullptr && (key & 0xFFu) == 0x20u) {
            // The no-target arm of a carried body, once per 256 staggered ticks:
            // the aim point re-seats 100 u along the carrier's yaw from the body's
            // own savedLivePose, plus 32 ticks of its own displacement.
            // [orig: @0x4BCA95 -> @0x4BCBEB..0x4BCCC1 (Q22 trig through dbl_7C3608 /
            //  dbl_7C3600; `shl 5` @0x4BCC6D/@0x4BCC96/@0x4BCCB6)]
            int32_t parent_pos[3];
            int32_t parent_yaw = 0, parent_pitch = 0, parent_roll = 0;
            carrier_pose_fixed(*parent_ride, parent_pos, parent_yaw, parent_pitch, parent_roll);
            const int32_t *saved = self_entity->saved_live_valid ? self_entity->saved_live_pos
                                                                 : e.pos;
            const double angle = static_cast<double>(parent_yaw) * 1.4629627251502471e-9;
            const int32_t s = static_cast<int32_t>(std::sin(angle) * 4194304.0);
            const int32_t c = static_cast<int32_t>(std::cos(angle) * 4194304.0);
            const auto shl5 = [](int32_t v) {
                return static_cast<int32_t>(static_cast<uint32_t>(v) << 5);
            };
            inf.aim_point[0] = io::bam_add(saved[0], io::bam_add(
                    static_cast<int32_t>((static_cast<int64_t>(c) * 0x640000) >> 22),
                    shl5(io::bam_sub(e.pos[0], saved[0]))));
            inf.aim_point[1] = io::bam_add(saved[1], io::bam_add(
                    static_cast<int32_t>((static_cast<int64_t>(s) * 0x640000) >> 22),
                    shl5(io::bam_sub(e.pos[1], saved[1]))));
            inf.aim_point[2] = io::bam_add(saved[2], shl5(io::bam_sub(e.pos[2], saved[2])));
        }
        int32_t eye[3];
        aim_eye(e.pos, muzzle, inf.aim_point, false, eye);
        const int32_t adx = io::bam_sub(inf.aim_point[0], eye[0]);
        const int32_t ady = io::bam_sub(inf.aim_point[1], eye[1]);
        const int32_t adz = io::bam_sub(inf.aim_point[2], eye[2]);
        // Bearing = chopped fpatan(dy, dx) in BAM; elevation = chopped fpatan(dz,
        // h) where h is the horizontal length clamped to 2147418112.0 and
        // TRUNCATED to an integer first. [orig: the world arm @0x4BCF2D..0x4BCF92]
        const auto solve_bearing_elevation = [](int32_t dx, int32_t dy, int32_t dz,
                int32_t &bearing_out, int32_t &elevation_out) {
            const double fdx = static_cast<double>(dx);
            const double fdy = static_cast<double>(dy);
            const int32_t horiz = ftol32(std::min(std::sqrt(fdx * fdx + fdy * fdy), 2147418112.0));
            bearing_out = ftol32(std::atan2(fdy, fdx) * io::kBamPerRadian);
            elevation_out = ftol32(std::atan2(static_cast<double>(dz),
                                              static_cast<double>(horiz)) * io::kBamPerRadian);
        };
        int32_t bearing = 0;
        int32_t elevation = 0;
        solve_bearing_elevation(adx, ady, adz, bearing, elevation);
        int32_t candidate = io::bam_add(bearing, err_heading); // [orig: `sub ebx,eax` @0x4bcf71]
        int32_t pitch = io::bam_add(elevation, err_pitch);     // [orig: @0x4bcf97..0x4bcf9d]
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
            const int32_t x1 = io::bam_sub(mul22(cy, adx), mul22(sy, ady));
            const int32_t y1 = io::bam_add(mul22(sy, adx), mul22(cy, ady));
            const int32_t x2 = io::bam_sub(mul22(cp, x1), mul22(sp, adz));
            const int32_t z1 = io::bam_add(mul22(sp, x1), mul22(cp, adz));
            const int32_t y2 = io::bam_sub(mul22(cr, y1), mul22(sr, z1));
            const int32_t z2 = io::bam_add(mul22(sr, y1), mul22(cr, z1)); // [orig: @0x4BCEA8]
            solve_bearing_elevation(x2, y2, z2, bearing, elevation);
            candidate = io::bam_add( // [orig: `add ebx,[edi+10h]` @0x4BCEEE]
                    io::bam_add(bearing, err_heading), parent_yaw);
            pitch = io::bam_add( // [orig: `add eax,[edi+14h]` @0x4BCF1B]
                    io::bam_add(elevation, err_pitch), parent_pitch);
        }
        inf.aim_heading = candidate; // [orig: @0x4BCEF1 / @0x4bcf75]
        inf.aim_pitch = pitch;       // [orig: @0x4BCF1E / @0x4bcf9d]
        inf.aim_established = true;
        inf.aim_override = true;     // [orig: the override local set @0x4BCFC2]
        inf.aim_valid = true;        // aimFlag [orig: @0x4bcfb1]
        // Block 2's tail [orig: @0x4bcfa3..0x4bcff5]. The body re-face when the
        // aim drifts far off the target heading (> 262470208, ~22 deg) [orig:
        // @0x4bcfa3..0x4bcfcf]; the detour-state clear, whether or not the
        // re-face fired, ahead of the selector's ai_find_cover_position calls
        // [orig: @0x4bcfdb; the calls @0x4bd490..0x4bd5a4]; then the hold:
        // every move mode but the combat approach (1) and 5 collapses to 7 with
        // a zero goal distance [orig: @0x4bcfd5..0x4bcff5 — `cmp al, 5`
        // @0x4bcfd9 and `cmp al, 1` @0x4bcfe4].
        if (io::bam_abs(io::bam_sub(inf.aim_heading, inf.target_heading)) > 262470208)
            inf.target_heading = inf.aim_heading;
        inf.path_state = 0;
        if (inf.move_mode != 1 && inf.move_mode != 5) {
            inf.move_mode = 7;
            inf.target_dist = 0;
        }
    }

    // A scripted idle (130..136) watches the local player: the aim heading and
    // pitch toward its Position (the planar length truncated, no clamp), aimFlag
    // down and the override up, the body re-faced past 45 degrees, no move and
    // the detour cleared. [orig: @0x4BCFF5..0x4BD0F4]
    if (inf.anim_state >= 130 && inf.anim_state <= 136) {
        if (const Entity *player = world.registry.get(world.cached.local_player)) {
            int32_t at[3];
            body_position(*this, *player, at);
            const int32_t dx = io::bam_sub(at[0], e.pos[0]);
            const int32_t dy = io::bam_sub(at[1], e.pos[1]);
            const int32_t dz = io::bam_sub(at[2], e.pos[2]);
            const int32_t heading =
                    ftol32(std::atan2(double(dy), double(dx)) * io::kBamPerRadian);
            const int32_t planar = ftol32(std::sqrt(double(dx) * dx + double(dy) * dy));
            inf.aim_heading = heading;
            inf.aim_pitch = ftol32(std::atan2(double(dz), double(planar)) * io::kBamPerRadian);
            inf.aim_valid = false;
            inf.aim_override = true;
            if (io::bam_abs(io::bam_sub(heading, inf.target_heading)) > 0x1FFFFFE0)
                inf.target_heading = heading;
            inf.move_mode = 0;
            inf.target_dist = 0;
            inf.path_state = 0;
        }
    }

    // The reload override on every path, after the aim blocks: a def clipsize,
    // the reload clip and a signed magazine word at or below zero select 65 with
    // no movement; whenever the CURRENT state is 65 the word refills from the
    // clipsize (its low word), clipsize or not.
    // [orig: @0x4BD132..0x4BD17D — the current state re-read @0x4BCFF5]
    if (e.profile.clip_size != 0 && avail(anim_state::kReload) && inf.magazine <= 0) {
        selected_state = anim_state::kReload;
        inf.move_mode = 0;
        inf.target_dist = 0;
    }
    if (inf.anim_state == anim_state::kReload)
        inf.magazine = static_cast<int16_t>(static_cast<uint16_t>(e.profile.clip_size));

    // One Flags read feeds the ladder and guard legs [orig: `mov ecx,[esi+24h]`
    // @0x4BD184]. On a ladder the move is dropped [orig: Flags 0x100000
    // @0x4BD187..0x4BD194].
    const uint32_t tail_flags = self_entity != nullptr
            ? (self_entity->flags | self_entity->engine_flags) : 0u;
    if ((tail_flags & kEntityFlagLadderContact) != 0) {
        inf.move_mode = 0;
        inf.target_dist = 0;
    }
    // The guard family [orig: @0x4BD196..0x4BD231]. A Flags 0x40 body holds in
    // place in guard (140); without the clip the flag drops [orig:
    // @0x4BD19B..0x4BD1BB]. A hit since the last think (the wasHit byte the
    // think captured at entry @0x4BA9A2 / @0x4BA9C1) takes guard_cover (143)
    // [orig: @0x4BD1BE..0x4BD1D5]; a chosen reaction takes guard_attack (142)
    // with moveMode 7 [orig: @0x4BD1D9..0x4BD1F8]. Off guard, a current
    // 140..143 leaves through guard_leave (144) with no move [orig:
    // @0x4BD1FA..0x4BD231].
    if ((tail_flags & kEntityFlagMounted) != 0) {
        inf.move_mode = 0;
        inf.target_dist = 0;
        if (avail(anim_state::kGuard)) {
            selected_state = anim_state::kGuard;
        } else if (self_entity != nullptr) {
            self_entity->flags &= ~kEntityFlagMounted;
            self_entity->engine_flags &= ~kEntityFlagMounted;
        }
        if (inf.was_hit && avail(anim_state::kGuardCover)) selected_state = anim_state::kGuardCover;
        if (inf.combat_reaction && avail(anim_state::kGuardAttack)) {
            selected_state = anim_state::kGuardAttack;
            inf.move_mode = 7;
        }
    } else if (avail(anim_state::kGuardLeave) &&
               (inf.anim_state == anim_state::kGuardCover || inf.anim_state == anim_state::kGuard ||
                inf.anim_state == anim_state::kGuardLook ||
                inf.anim_state == anim_state::kGuardAttack)) {
        inf.move_mode = 0;
        inf.target_dist = 0;
        selected_state = anim_state::kGuardLeave;
    }
    // The WAC holdSSN hold (+0x2C bit 0x2000) parks the body in moveMode 12
    // [orig: @0x4BD235..0x4BD240; set/clear by WacCmd_HoldSsn @0x4F785D /
    // WacCmd_UnholdSsn @0x4F78BD].
    if (self_entity != nullptr && (self_entity->cause_flags & 0x2000u) != 0) {
        inf.move_mode = 12;
        inf.target_dist = 0;
    }
    // A chosen reaction holds the body [orig: the hasReaction test ->
    // moveMode 7, distance 0 @0x4BD245..0x4BD251].
    if (inf.combat_reaction) {
        inf.move_mode = 7;
        inf.target_dist = 0;
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
        // WeaponSlot_FireAndSpawnEffects owns this session gate; the pass
        // itself runs on the authority only (tick_infantry's gate), so the
        // marks and the magazine decrement are authority work too.
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
    // Every mounted-live body makes the request, whatever its seat: only the
    // EquippedSlot test below tells a UseGun rider from a passenger.
    // [orig: Entity_UpdateInfantryAI mounted-live test @0x4BF4B3, the parent
    //  test @0x4BF4C1..0x4BF4C9]
    Entity *occ = world.registry.get(e.handle);
    if (occ == nullptr || !occ->mounted) return;
    Entity *mount = world.registry.get(occ->mount_target);
    if (mount == nullptr) return;
    // The parent's weapon slot and AdmDef byte exist from its own init in
    // retail; the port seeds them lazily. [orig: WeaponSlot_InitFromEntityDef
    //  @0x5466C0]
    world.vehicles.prepare_weapon_slot(*mount);
    if (!inf.combat_target.valid()) return;

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

    // Past the cadence and the stagger the rider takes the parent's AdmDef byte
    // (retail's parent +0x2B0 is WeaponSlot_InitFromEntityDef's byte, kept here
    // as primary_weapon_slot_adm). A parent whose def names no weapon never
    // stores one and still holds its spawn clear's zero, not the port's none
    // sentinel. [orig: Entity_UpdateInfantryAI @0x4BF4F4..0x4BF4FA;
    //  WeaponSlot_InitFromEntityDef @0x546742, skipped by the name test
    //  @0x5466E1 or the def test @0x546704; the clear
    //  Entity_SpawnFromBMSRecord @0x40EA1F]
    occ->equipped_adm_index = mount->primary_weapon_slot_adm != kAdmSlotNone
            ? mount->primary_weapon_slot_adm : 0;

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

    // The EquippedSlot the attach swapped to the parent's MountSlot must be
    // there. [orig: `mov edi,[esi+118h]; test edi,edi` @0x4BF564..0x4BF56C]
    if (!occ->use_gun_slot_swapped) return;
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
