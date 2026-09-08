#include <runtime/world/ai.h>
#include <base/io/fixed.h>

// Split out of ai.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// P2 ground combat: target acquisition and the perception gates, engagement
// relations, line of sight, ally alerting, AI fire, and command handling.

#include <runtime/terrain_query/height_field.h>
#include <runtime/world/angle.h>
#include <algorithm>
#include <cmath>

#include "ai_detail.h"

#include <runtime/world/world.h>

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

// ----------------------------------------------------------------------------
// P2: GROUND combat + targeting.
// ----------------------------------------------------------------------------

int AiSystem::index_of(const AiEntity &e) const {
    return static_cast<int>(&e - entities_.data());
}

// [orig: inline LCG on dword_31BFBB8]. PRNG_Next16's separate but shared
// dword_31BFBB0 stream is owned by World; the low 16 bits feed both jitters.
int32_t AiSystem::prng_step_a() { return static_cast<int32_t>(prng_step(prng_a)); }

// [orig: the shared death-velocity event @0x467730/0x457d70/0x467400] queue a crash(3)/still(4)
// AIEvent by horizontal speed. Channel 0, entity index, timer 0 (the orig stores fldz to var_C).
void AiSystem::queue_death_event(AiEntity &e) {
    double sp = std::sqrt(static_cast<double>(e.vel_x) * e.vel_x +
                          static_cast<double>(e.vel_z) * e.vel_z);
    if (sp > kDeathSpeedClamp) sp = kDeathSpeedClamp; // flt_7C19E0 min-clamp
    int32_t isp = static_cast<int32_t>(sp);
    AiEventEntry ev{};
    ev.f[0] = (isp >= 1057) ? 3 : 4;     // crash/ragdoll vs still death
    ev.f[1] = (index_of(e) << 16);       // channel 0 | entity index
    ev.set_timer(0.0f);
    events.queue(ev);
}

// The candidate FEED (D-AI-1, witnessed world-wac-ai-re §16.2): the class-driven pool
// walk into the scratch list, then the byte-exact scoring core with the lazy LOS probe.
// [orig: AI_FindBestTargetB @0x466f60 — the outer do/while over the four profile
// weapon-slot classes (+40+4*slot), each gated by its class-priority word (+80+4*class),
// each selecting pools 0/1/2 with the helo-brain / Player-flag sub-filters.]
bool AiSystem::acquire_target(World &world, AiEntity &e, AiTarget &out) {
    scan_candidates_.clear();
    // Entry gates [orig: 0x466f60 head]. The round-end latch nulls acquisition outright
    // [orig: g_spawn_success_gate @0x24C1928 nonzero -> return null @0x466fba]; the
    // teamless gate is mirrored in the scoring core (it is part of @0x466f60) — here it
    // only skips the wasted pool walk.
    if (world.match.outcome().ended) return false;
    if (e.team == 0 && !e.see_all) return acquire_target_from(e, scan_candidates_, out);

    const int32_t prio_packed = e.brain.f[AiBrain::kPriorityTarget];

    // One pool leg of the walk: the common perception pre-gates + the class sub-filter,
    // eligible candidates pushed onto the scratch list [orig: the inner while loop
    // @0x467070-0x4673a7]. The remaining per-candidate gates (flags 2/0x8000000, the
    // +530 refcount saturation, FOV/range caps) live in the scoring core.
    const auto scan_pool = [&](int pool, bool target_vehicles, bool check_player_flag) {
        const size_t cap = world.registry.pool_capacity(pool);
        for (size_t s = 0; s < cap; ++s) {
            const EntityHandle h = EntityHandle::make(pool, static_cast<int>(s));
            const Entity *c = world.registry.get(h);
            if (c == nullptr) continue;                    // [orig: +28 in-use]
            if (h == e.handle) continue;                   // not self
            if (c->health <= 0) continue;                  // [orig: +286 > 0]
            // Team gate [orig: teamless candidates need the attacker's 0x200; same-team
            // skipped unless 0x200].
            if ((c->team == 0 || c->team == e.team) && !e.see_all) continue;
            // The class sub-filter [orig: 0x46711f-0x46717e; "parent" there is the
            // candidate's BRAIN (+100), *(brain+4)+16 the profile type word].
            const AiEntity *cand_ai = for_handle(h);
            const bool helo_brained = cand_ai != nullptr && cand_ai->profile.type == 1;
            if (target_vehicles) {
                if (check_player_flag) {
                    // The class-0 pool-0 leg: Player-flagged only; the LOCAL player is
                    // excluded under the MP rules bit [orig: flags & 0x100 @0x46714b;
                    // candidate == g_local_player_entity && dword_24C1930 & 0x800
                    // @0x467155].
                    if ((c->engine_flags & kEntityFlagPlayer) == 0) continue;
                    if (h == world.cached.local_player && world.rules.ai_rules_skip_local_player)
                        continue;
                } else {
                    // The class-0 pool-1 leg (and an inherited case-3 walk): unbrained
                    // or helo-typed [orig: !parent || profile+16 == 1 @0x46715d].
                    if (cand_ai != nullptr && cand_ai->profile.type != 1) continue;
                }
            } else {
                // Classes 1/2: never Player-flagged, never helo-brained
                // [orig: the else-leg @0x467169-0x46717e].
                if ((c->engine_flags & kEntityFlagPlayer) != 0) continue;
                if (helo_brained) continue;
            }
            AiCandidate cand;
            cand.handle = h;
            cand.pos[0] = static_cast<int32_t>(c->position.x * io::kFp16One);
            cand.pos[1] = static_cast<int32_t>(c->position.y * io::kFp16One);
            cand.pos[2] = static_cast<int32_t>(c->position.z * io::kFp16One);
            cand.team = c->team;
            cand.flags = static_cast<int32_t>(c->engine_flags); // &2/&0x8000000 skip, &0x4000 prio x6
            cand.health = static_cast<int16_t>(std::min<int32_t>(c->health, INT16_MAX));
            cand.visibility = c->ai_target_refcount;       // [orig: +530 refcount saturation]
            // Per-candidate engage-range caps: the candidate's items.def signatures,
            // stamped at the item-traits sweep [orig: uint16 words +422/+420 read
            // @0x46723e/@0x467277; written entity+422 = def+376 radarSig, entity+420 =
            // def+378 heatSig by Entity_InitFromModel @0x40e136-0x40e15d].
            cand.range_primary = c->radar_sig;
            cand.range_secondary = c->heat_sig;
            cand.relmat_id = c->group_id;                  // group key (+0x11C commandGroup)
            cand.net_id = c->net_id;                       // single key (+0x7C SSN)
            cand.has_controller = (c->owner_connection_id != 0);
            // The scanner's brain+148 priority target [orig: read @0x467350; the same
            // packed+1 null rebase as kTargetSlot].
            cand.is_priority =
                    prio_packed != 0 && prio_packed == static_cast<int32_t>(h.packed) + 1;
            scan_candidates_.push_back(cand);
        }
    };

    // The class walk [orig: the switch @0x466fe9 + the two-leg continuation @0x4673ac].
    // target_vehicles deliberately persists across slots — case 3 never assigns it and
    // inherits the previous slot's value (function-entry 0), the witnessed quirk.
    bool target_vehicles = false;
    for (int slot = 0; slot < 4; ++slot) {
        int pool;
        int pools_remaining;
        bool check_player_flag;
        switch (e.profile.slot_class[slot]) {
        case 0: // air: pool 1 helo-brained, then pool 0 players [orig: case 0 @0x466ff3]
            if (e.profile.class_priority[0] == 0) continue;
            pool = 1; target_vehicles = true; check_player_flag = false; pools_remaining = 2;
            break;
        case 1: // ground: pool 1 non-helo [orig: case 1 @0x467012]
            if (e.profile.class_priority[1] == 0) continue;
            pool = 1; check_player_flag = false; target_vehicles = false; pools_remaining = 1;
            break;
        case 2: // organics: pool 0 non-player [orig: case 2 @0x46702d]
            if (e.profile.class_priority[2] == 0) continue;
            pool = 0; check_player_flag = false; target_vehicles = false; pools_remaining = 1;
            break;
        case 3: // decorations: pool 2 (buildings) [orig: case 3 @0x467048]
            if (e.profile.class_priority[3] == 0) continue;
            pool = 2; check_player_flag = false; pools_remaining = 1;
            break;
        default: // [orig: default -> next slot]
            continue;
        }
        scan_pool(pool, target_vehicles, check_player_flag);
        // The case-0 second leg [orig: 0x4673ac-0x4673be: pool 0, building/player
        // filter armed, target_vehicles re-set].
        while (--pools_remaining > 0) {
            target_vehicles = true;
            scan_pool(/*pool*/ 0, /*target_vehicles*/ true, /*check_player_flag*/ true);
        }
    }
    // The live feed's lazy LOS probe [orig: Entity_CheckMutualLineOfSight @0x539be0 —
    // fire-origin -> fire-origin, called only at the priority bypass / would-be-best
    // sites]. Endpoints ride the muzzle seam (weapon_fire_origin); an injected
    // candidate with no live entity keeps the list's raw pos.
    struct LosCtx {
        AiSystem *sys;
        World *world;
        const AiEntity *scanner;
    } los_ctx{this, &world, &e};
    const LosBlockedFn probe = [](void *ctx, const AiCandidate &c) -> bool {
        LosCtx *lc = static_cast<LosCtx *>(ctx);
        int32_t sa[3];
        lc->sys->weapon_aim_origin(*lc->world, *lc->scanner, sa);
        int32_t sb[3];
        if (const Entity *te = lc->world->registry.get(c.handle)) {
            lc->sys->weapon_aim_origin(*lc->world, *te, sb);
        } else {
            sb[0] = c.pos[0];
            sb[1] = c.pos[1];
            sb[2] = c.pos[2];
        }
        return !lc->sys->line_of_sight_clear(*lc->world, sa, sb,
                                             lc->scanner->handle, c.handle);
    };
    return acquire_target_from(e, scan_candidates_, out, probe, &los_ctx);
}

// [orig: AI_FindBestTargetB @0x466f60 scoring walk] over an explicit candidate list.
// Per-candidate perception gates (team, flags, health, self, refcount saturation) then
// FOV/range/stealth scoring + LOS; the priority target bypasses scoring on clear LOS.
bool AiSystem::acquire_target_from(AiEntity &e, const std::vector<AiCandidate> &candidates,
                                   AiTarget &out, LosBlockedFn los_fn, void *los_ctx) {
    ++find_target_calls;
    // LOS verdict per candidate: the lazy probe when supplied (the live feed), else the
    // preset flag (injected lists). Evaluated ONLY at the priority-bypass / would-be-best
    // sites below [orig: Entity_CheckMutualLineOfSight called at @0x467363/@0x46738b].
    const auto los_blocked = [&](const AiCandidate &c) {
        return los_fn != nullptr ? los_fn(los_ctx, c) : c.los_blocked;
    };
    // Entry gate [orig: 0x466f60 head]: teamless scanners need the 0x200 see-all flag.
    if (e.team == 0 && !e.see_all) return false;
    const int primary_fov = e.profile.fov_primary | 1;     // [orig: (def+75)|1]
    const int secondary_fov = e.profile.fov_secondary | 1; // [orig: (def+67)|1]
    const AiCandidate *best = nullptr;
    int best_score = 0;
    for (const AiCandidate &c : candidates) {
        if (c.handle == e.handle) continue;                       // candidate != self
        if ((c.flags & 2) != 0) continue;                         // [orig: flags & 2 -> skip]
        if (c.health <= 0) continue;                              // [orig: *(cand+286) > 0]
        // [orig: (cand+530 <= 16 || def+100 & 8)] stealth pre-gate
        if (!(c.visibility <= 16 || (e.profile.flags100 & 8) != 0)) continue;
        if ((c.flags & 0x8000000) != 0) continue;                 // [orig: flags & 0x8000000 -> skip]
        // [orig: team check with self aiSlot+4 & 0x200 see-all]
        bool team_ok = (c.team != 0 || e.see_all) && (c.team != e.team || e.see_all);
        if (!team_ok) continue;

        int32_t bearing = bearing_bam(c.pos[1] - e.pos[1], c.pos[0] - e.pos[0]);
        int angle_diff = static_cast<int>(static_cast<uint32_t>(bearing - e.heading) >> 24);
        if (angle_diff >= 0x80) angle_diff = 256 - angle_diff;    // fold to [0,128]
        int dist = dist3d_units(c.pos, e.pos);

        // [orig: 0x46722e..0x467291] FOV/range gate FIRST. A candidate outside both gates jumps to
        // LABEL_17 (next candidate) and never reaches the priority bypass (0x467350) or the best-of
        // compare. ai_score_target returns -1 on gate-fail.
        int score = ai_score_target(angle_diff, dist, primary_fov, secondary_fov,
                                    e.profile.range_primary, e.profile.range_secondary,
                                    c.range_primary, c.range_secondary, c.visibility, c.flags);
        if (score < 0) continue;

        if (c.is_priority) { // [orig: 0x467350 — reached only past the gate] priority -> LOS-only bypass
            if (!los_blocked(c)) { // Entity_CheckMutualLineOfSight @0x539be0
                out = AiTarget{c.relmat_id, c.net_id, c.has_controller, c.handle};
                return true;
            }
            continue; // priority but LOS blocked -> skip (no best-of, matches the orig else-if)
        }
        if (score > best_score && !los_blocked(c)) { // [orig: else if (score > best_score) + LOS]
            best_score = score;
            best = &c;
        }
    }
    if (best) {
        out = AiTarget{best->relmat_id, best->net_id, best->has_controller, best->handle};
        return true;
    }
    return false;
}

// The acquisition/fire relation quads, APPLIED (D-AI-3 closed 2026-07-16). Keys:
// group = commandGroup (entity+0x11C -> Entity::group_id), single = the authored SSN
// (entity+0x7C DcbId -> Entity::net_id). Witnessed order + arg orientation from the raw
// call block: sees {GG[selfG][tgtG], SG[selfSsn][tgtG], GS[tgtSsn][selfG] (transposed),
// SS[selfSsn][tgtSsn]} then targeted the same; setter->matrix identity pinned via the
// g_SeesMatrix*/g_TargetedMatrix* bases. [orig: @0x4677b3..0x4678b2 / @0x4b0a6f..0x4b0ae2;
// world-wac-ai-re §16.4/§17.2; matrix map docs/mission/bms-event-runtime-re.md §3a]
void AiSystem::apply_engage_relations(World &world, const Entity &self, const Entity &target) {
    TriggerRelations &rel = world.script.relations;
    const int sg = self.group_id, ss = self.net_id;
    const int tg = target.group_id, ts = target.net_id;
    rel.set_group_group(TriggerRelations::kSees, sg, tg);       // [orig: @0x452a40 GG]
    rel.set_single_group(TriggerRelations::kSees, ss, tg);      // [orig: @0x452ad0 SG]
    rel.set_group_single(TriggerRelations::kSees, sg, ts);      // [orig: @0x452b60 GS]
    rel.set_single_single(TriggerRelations::kSees, ss, ts);     // [orig: @0x452bf0 SS]
    rel.set_group_group(TriggerRelations::kTargeted, sg, tg);   // [orig: @0x452aa0 GG]
    rel.set_single_group(TriggerRelations::kTargeted, ss, tg);  // [orig: @0x452b30 SG]
    rel.set_group_single(TriggerRelations::kTargeted, sg, ts);  // [orig: @0x452bc0 GS]
    rel.set_single_single(TriggerRelations::kTargeted, ss, ts); // [orig: @0x452c70 SS]
}

// [orig: Entity_SetAITarget @0x45d760] target -> brain[kTargetSlot] + AiSlot[3]; the OLD
// target's Entity::ai_target_refcount decrements (clamp >= 0), the NEW one's increments —
// the +530 anti-pile-on gate the scorer consumes (world-wac-ai-re §16.3).
void AiSystem::ai_set_target(World &world, AiEntity &e, EntityHandle target) {
    AiBrain &b = e.brain;
    const int32_t old_packed = b.f[AiBrain::kTargetSlot];
	if (old_packed == (target.valid() ? int32_t(target.packed) + 1 : 0))
		return;
	if (old_packed != 0) {
		const EntityHandle old{static_cast<uint16_t>(old_packed - 1)};
        if (Entity *oe = world.registry.get(old)) {
			oe->ai_target_refcount = int16_t(uint16_t(oe->ai_target_refcount) - 1u);
			if (oe->ai_target_refcount < 0)
				oe->ai_target_refcount = 0;
		}
	}
	if (target.valid()) {
        if (Entity *ne = world.registry.get(target)) {
            ++ne->ai_target_refcount;
            ne->ai_target = -1; // scripts read the victim side via relations; id mirror below
        }
        b.f[AiBrain::kTargetSlot] = static_cast<int32_t>(target.packed) + 1;
        e.slot.f[3] = static_cast<int32_t>(target.packed) + 1; // AiSlot[3] mirror
        if (Entity *se = world.registry.get(e.handle)) {
            if (const Entity *ne = world.registry.get(target)) se->ai_target = ne->net_id;
        }
    } else {
        b.f[AiBrain::kTargetSlot] = 0;
        e.slot.f[3] = 0;
        if (Entity *se = world.registry.get(e.handle)) se->ai_target = -1;
    }
}

// [orig: Entity_ClearAllReferences @0x465670]
void AiSystem::clear_vehicle_target_references(World &world, AiEntity &vehicle) {
	const int32_t packed = int32_t(vehicle.handle.packed) + 1;
	for (int pool = 0; pool < 2; ++pool)
		world.registry.for_each_in_pool(pool, [&](const Entity &entity) {
			AiEntity *other = for_handle(entity.handle);
			if (other == nullptr)
				return;
			if (other->inf.active) {
				if (other->slot.f[3] == packed)
					other->slot.f[3] = 0;
				return;
			}
			// Both assembly sites pass the destroyed entity to SetAITarget,
			// preserving this retail quirk rather than clearing the other brain.
			if (other->brain.f[AiBrain::kTargetSlot] == packed)
				ai_set_target(world, vehicle, EntityHandle{});
			if (other->brain.f[AiBrain::kPriorityTarget] == packed)
				other->brain.f[AiBrain::kPriorityTarget] = 0;
			if (other->brain.f[AiBrain::kDamageInfo] == packed)
				other->brain.f[AiBrain::kDamageInfo] = 0;
		});
}

// The fire/aim/LOS origin (the header carries the witness): the launch
// userpoint on the entity's posed skeleton, resolved now by the world's
// muzzle-pose provider; without a model, a userpoint table, or a resolvable
// point retail copies the raw entity origin — no lift of any kind.
// [orig: Entity_GetAttachmentWorldPosition @0x4b2670: the raw copy
//  @0x4b2767..0x4b278e when index 0 / no graphicModel / no table]
void AiSystem::weapon_fire_origin(const AiEntity &e, int32_t out[3]) {
    out[0] = e.pos[0];
    out[1] = e.pos[1];
    out[2] = e.pos[2];
}

void AiSystem::weapon_fire_origin(const Entity &e, int32_t out[3]) {
    out[0] = static_cast<int32_t>(e.position.x * io::kFp16One);
    out[1] = static_cast<int32_t>(e.position.y * io::kFp16One);
    out[2] = static_cast<int32_t>(e.position.z * io::kFp16One);
}

// The live-pose seam shared by both World& forms: the world's native muzzle
// provider resolves against the current simulation pose first.
// Weapon aim keeps the defer-LOS flag, rotated offset and full-frame angle extraction.
// [orig: @0x456BD5, @0x456C59, @0x456CE5, @0x456CFA]
static bool provider_muzzle_origin(World &world, EntityHandle h, int32_t out[3]) {
    return world.pose_provider != nullptr &&
           world.pose_provider->resolve_muzzle_pose(world, h, out);
}

void AiSystem::weapon_fire_origin(World &world, const AiEntity &e, int32_t out[3]) const {
    if (provider_muzzle_origin(world, e.handle, out)) return;
    weapon_fire_origin(e, out);
}

void AiSystem::weapon_aim_origin(World &world, const Entity &e, int32_t out[3]) const {
    // [orig: Entity_ComputeWeaponFireOrigin @0x43b4b0 — def+92 == 3 selects
    //  the person leg; the +0x6C vector's writer is unwalked, so the muzzle
    //  seam stands in for it (D-AI-6)]
    const bool person = e.has_item_def ? e.item_type == 3 : e.kind == EntityKind::Organic;
    if (person) {
        weapon_fire_origin(world, e, out);
        return;
    }
    // No graphic model: the raw position [orig: @0x43b54f..0x43b55b].
    const bool has_model = world.collision != nullptr &&
                           world.collision->entity_model_id(e.handle) >= 0;
    if (!has_model) {
        weapon_fire_origin(e, out);
        return;
    }
    // def+1350: the TARGET userpoint through the placement matrix
    // [orig: @0x43b5d4..0x43b5f6].
    if (e.target_userpoint_byte != 0 && world.pose_provider != nullptr &&
        world.pose_provider->resolve_userpoint_rigid(
                world, e.handle, e.target_userpoint_byte, out))
        return;
    // Else the model collision-bbox center entity+0x1FC through the same
    // matrix [orig: @0x43b619].
    const int32_t center[3] = {
        static_cast<int32_t>(e.bbox_center.x * io::kFp16One),
        static_cast<int32_t>(e.bbox_center.y * io::kFp16One),
        static_cast<int32_t>(e.bbox_center.z * io::kFp16One)};
    entity_placement_matrix(e).transform_point(center, out);
}

void AiSystem::weapon_aim_origin(World &world, const AiEntity &e, int32_t out[3]) const {
    if (const Entity *ent = world.registry.get(e.handle)) {
        weapon_aim_origin(world, *ent, out);
        return;
    }
    weapon_fire_origin(world, e, out);
}

void AiSystem::weapon_fire_origin(World &world, const Entity &e, int32_t out[3]) const {
    if (provider_muzzle_origin(world, e.handle, out)) return;
    weapon_fire_origin(e, out);
}

// LOS between two EXACT 16.16 endpoints — true = clear. Callers supply the fire
// origins via weapon_fire_origin (or their own witnessed endpoints: the corpse
// watch and USE scan keep their explicit lifts at the call sites). The terrain
// leg is the ported heightmap segment raycast; the sector leg clips the ray
// against pool-2/pool-1 collision models (the D-AI-7 leg).
// [orig: Entity_CheckMutualLineOfSight @0x539be0 = two Entity_ComputeWeaponFireOrigin
// @0x43b4b0 results into Physics_RaycastTerrainAndSectors @0x539910, radius 0,
// 1 = clear.] No terrain wired -> clear, the headless-test default; no collision
// world wired (terrain-only unit tests) -> terrain leg alone.
// The endpoint fold shared by both LOS forms: a mounted body excludes its
// carrier, an emplaced weapon its parent (retail resolves entity[154] /
// entity[91] before the model walk), so a rider's own vehicle never occludes
// its sight. Persons carry no collision instance, so substituting the carrier
// keeps the effective exclusion identical. One link level; the two-level
// gunner-on-emplaced-child chain is a tracked follow-up.
// [orig: raycast_find_collision_entity @0x539a70 endpoint resolve
//  @0x539aba..0x539b10; raycast_against_entity_pool @0x538720 skips
//  entity_a/entity_b/parent_a/parent_b]
static EntityHandle los_exclude_handle(const World &world, EntityHandle h) {
    const Entity *ent = world.registry.get(h);
    if (ent != nullptr) {
        if (ent->mounted && ent->mount_target.valid()) return ent->mount_target;
        if (ent->emplacement_parent.valid()) return ent->emplacement_parent;
    }
    return h;
}

bool AiSystem::line_of_sight_clear(World &world, const int32_t a[3], const int32_t b[3],
                                   EntityHandle from, EntityHandle to) const {
    return line_of_sight_clear_impl(world, a, b, from, to, /*cached=*/false);
}

// The stable-phase replication form of line_of_sight_clear: identical
// semantics (the same endpoint carrier/emplacement fold, then the cached
// exact clip), so a mounted recipient or target still sees through its own
// hull [orig: the same raycast_find_collision_entity endpoint resolve].
bool AiSystem::line_of_sight_clear_cached(World &world, const int32_t a[3],
                                          const int32_t b[3], EntityHandle from,
                                          EntityHandle to) const {
    return line_of_sight_clear_impl(world, a, b, from, to, /*cached=*/true);
}

// One body for both LOS forms; `cached` picks the raycast_clear /
// raycast_clear_cached sector leg.
bool AiSystem::line_of_sight_clear_impl(World &world, const int32_t a[3],
                                        const int32_t b[3], EntityHandle from,
                                        EntityHandle to, bool cached) const {
    if (terrain == nullptr || !terrain->valid()) return true;
    if (collision != nullptr) {
        // Each endpoint folds through los_exclude_handle (the +0x268 link,
        // then the +0x16C carrier overriding) before the model walk.
        const EntityHandle from_h = los_exclude_handle(world, from);
        const EntityHandle to_h = los_exclude_handle(world, to);
        const CollisionWorld::RayDebugScope ray_scope(
                collision, cached
                        ? CollisionWorld::RayDebugCategory::kReplicationLos
                        : CollisionWorld::RayDebugCategory::kAiLos);
        return cached ? collision->raycast_clear_cached(world, a, b, from_h, to_h)
                      : collision->raycast_clear(world, a, b, from_h, to_h);
    }
    return !los_terrain_blocked(*terrain, a, b);
}

// [orig: Entity_AlertNearbyAllies @0x4654b0] same-team, alive, non-building entities
// within `radius` (16.16): own AiSlot+136 = 2 and each ally's brain alert = 2. Container
// rebase: the original scans pool 1; our brains live on the AiEntity list, so the walk
// covers every brained entity (pool 0 organics included) — values written are identical.
void AiSystem::alert_nearby_allies(World &world, AiEntity &e, int32_t radius) {
    (void)world;
    e.slot.bytes()[AiSlot::kAlertByte] = 2;
    const int64_t r = radius;
    for (AiEntity &ally : entities_) {
        if (&ally == &e || ally.team != e.team || ally.health <= 0) continue;
        const int64_t ddx = static_cast<int64_t>(ally.pos[0]) - e.pos[0];
        const int64_t ddy = static_cast<int64_t>(ally.pos[1]) - e.pos[1];
        if (ddx * ddx + ddy * ddy > r * r) continue;
        ally.brain.f[AiBrain::kAlert] = 2;
    }
}

// AI fire -> the same authoritative round pair the C2S 0x06 player path enters: ring
// append (the S2C 0x0A tag-2 fan-out every remote client re-simulates the round from)
// + the RoundSim spawn. [orig: WeaponSlot_FireAndSpawnEffects @0x53f440 ->
// Entity_FireWeaponAndSendPacket @0x42bd80 authority leg -> Server_ClientFiredRound
// @0x50baa0 (RoundData_AddRound @0x4fdb40 + RoundData_SpawnRound @0x4ec0d0); §5.60.
// The ammo id rides the ring's adm byte as in the retail fire_cmd; fire sound/muzzle
// effect (g_ammoDefTable +64/+68) are host-presentation, deferred.]
bool AiSystem::fire_ai_round(World &world, AiEntity &e, const int32_t origin[3],
                             int32_t yaw_bam, int32_t pitch_bam, int32_t ammo_index) {
    if (world.tables.ammo.by_index(ammo_index) == nullptr) return false;
    ++fire_shot_seq; // [orig: word_B7C670 round-trips into the ring +28 word]

    RoundEvent ev;
    ev.shooter_handle = e.handle.packed;
    ev.origin_x = origin[0];
    ev.origin_y = origin[1];
    ev.origin_z = origin[2];
    ev.dir_yaw = yaw_bam;
    ev.dir_pitch = pitch_bam;
    ev.shot_seq = fire_shot_seq;
    const Entity *shooter = world.registry.get(e.handle);
    const uint8_t adm_index = shooter != nullptr &&
                                      world.tables.weapons.by_index(
                                              shooter->equipped_adm_index) != nullptr
            ? shooter->equipped_adm_index
            : static_cast<uint8_t>(ammo_index & 0xFF);
    ev.adm_index = adm_index;
    world.out.rounds.add(ev);

    RoundSpawnParams rp;
    rp.owner = e.handle;
    rp.shooter_handle = e.handle.packed;
    rp.origin.x = static_cast<float>(origin[0]) / io::kFp16One;
    rp.origin.y = static_cast<float>(origin[1]) / io::kFp16One;
    rp.origin.z = static_cast<float>(origin[2]) / io::kFp16One;
    rp.dir_yaw_bam = yaw_bam;
    rp.dir_pitch_bam = pitch_bam;
    rp.ammo_index = ammo_index;
    rp.adm_index = adm_index;
    rp.shot_seq = fire_shot_seq;
    world.round_sim.spawn(world, rp);

    // Firing marks the shooter a priority target until the next perception scan clears
    // it [orig: Flags |= 0x4000 after every fire @0x4bf370; the §16.2 x6 scoring flag].
    if (Entity *se = world.registry.get(e.handle)) se->engine_flags |= kEntityFlagPriorityTarget;
    return true;
}

// The SM/turret fire-transform solve — the D-AI-2 core, ported as a structural
// translation of Entity_ComputeWeaponFireTransform_0 @0x456980 (the record's old
// 0x455b30 address was a transcription slip). Deviations, each bounded and cited
// in place: our world model carries no SM muzzle bone lists (brain+224/+292 stay
// unfilled), so the muzzle always takes the witnessed EMPTY-LIST leg; the solve
// frame is yaw-only because AiEntity carries no pitch/roll (retail inverts the
// full entity matrix — level shooters are identical); the §17.2 ctx range legs
// stay with the acquire-time gates (D-AI-1's ctx model); and the per-type turret
// CTRL diagnostic globals (dword_83FE88/dword_83FEE0 pairs @0x456E33/0x456F9C)
// are unported — their consumer is the D-3DI-2 bus.
// The fire validator's full local-frame metrics, shared by the aircraft
// movement visibility check and its weapon solver. [orig: sub_53AFC0 @0x53AFC0;
// Entity_ValidateWeaponTarget @0x53A400; compute_relative_position_metrics @0x545710]
bool AiSystem::weapon_target_metrics(World &world, AiEntity &e, const Entity &target,
		const int32_t pose[6], int32_t aim_offset, bool skip_los, int32_t metrics[6]) {
	if (target.handle == e.handle ||
			(target.health <= 0 && int32_t(world.logic_tick - target.death_tick) > 16))
		return false;
	if (((target.flags | target.engine_flags) & kEntityFlagPlayer) != 0 &&
			world.match.outcome().ended)
		return false;
	int32_t aim[3];
	weapon_aim_origin(world, target, aim);
	if (aim_offset != 0) {
		const int32_t zero[3] = {};
		const CollisionMatrix yaw = collision_matrix_from_euler(e.heading, 0, 0, zero);
		const int32_t offset[3] = { aim_offset, 0, 0 };
		int32_t rotated[3];
		yaw.rotate_point(offset, rotated);
		for (int axis = 0; axis < 3; ++axis)
			aim[axis] = io::bam_add(aim[axis], rotated[axis]);
	}
	CollisionMatrix frame = collision_matrix_from_euler(pose[3], pose[4], pose[5], pose), inverse;
	frame.invert_into(inverse);
	const int32_t delta[3] = { io::bam_sub(aim[0], pose[0]), io::bam_sub(aim[1], pose[1]),
		io::bam_sub(aim[2], pose[2]) };
	int32_t local[3];
	inverse.rotate_point(delta, local);
	const auto sq = [](int32_t x) {
		const int64_t v = x >> 8;
		return int32_t((v * v + 0x8000) >> 16);
	};
	const int32_t xy = io::bam_add(sq(local[0]), sq(local[1]));
	const auto root = [](int32_t x) {
		if (x < 0)
			return int32_t(0); // x87 invalid conversion <<16 yields zero
		return int32_t(uint32_t(int32_t(std::sqrt(double(x)))) << 16);
	};
	std::fill_n(metrics, 6, 0);
	metrics[0] = root(xy);
	metrics[2] = root(io::bam_add(xy, sq(local[2])));
	metrics[3] = bearing_bam(local[1], local[0]);
	metrics[4] = bearing_bam(local[2], metrics[0]);
	const uint32_t yaw = uint32_t(metrics[3] < 0 ? io::bam_sub(0, metrics[3]) : metrics[3]);
	const uint32_t pitch = uint32_t(metrics[4] < 0 ? io::bam_sub(0, metrics[4]) : metrics[4]);
	const uint32_t arc = std::max(yaw, pitch) + ((5u * std::min(yaw, pitch)) >> 4);
	const auto &p = e.profile;
	// Context flags 111: heat and radar arms, with their separate FOV/range
	// and signature caps. The cone-only range arm requires bit16, absent here.
	const bool heat = arc <= uint32_t(p.view_fov_bam) && metrics[2] <= p.view_dist &&
			metrics[2] < int32_t(uint32_t(target.heat_sig) << 16);
	const bool radar = arc <= uint32_t(p.radar_fov_bam) && metrics[2] <= p.approach_cap &&
			(target.radar_sig == 0 || metrics[2] < int32_t(uint32_t(target.radar_sig) << 16));
	if (!heat && !radar)
		return false;
	if (skip_los)
		return true;
	int32_t start[3];
	const Entity *self = world.registry.get(e.handle);
	if (self == nullptr || self->look_userpoint_byte == 0 || world.pose_provider == nullptr ||
			!world.pose_provider->resolve_userpoint_rigid(
					world, e.handle, self->look_userpoint_byte, start))
		weapon_aim_origin(world, e, start);
	return line_of_sight_clear(world, start, aim, e.handle, target.handle);
}

bool AiSystem::solve_weapon_fire_transform(World &world, AiEntity &e, const Entity *target,
                                           const AiProfile::WeaponFire &wb, int32_t aim_offset,
                                           bool skip_los, int32_t out[6]) {
    AiBrain &b = e.brain;
    const uint32_t flags = wb.flags;

    // Head gate [orig: @0x4569B2 — (weaponDef+16 & 0x18) == 0 && no target -> 0].
    if ((flags & 0x18u) == 0 && target == nullptr) return false;
    b.bytes()[AiBrain::kBoneFlagByte] = 0; // [orig: @0x4569D5]

    // Muzzle origin + frame pre-seed. The empty-bone-list leg: entity position
    // with +2.0u Z [orig: @0x456B03 out[2] += 0x20000]; angles pre-seeded from
    // the entity (the call sites copy entity+4..+0x18 into the out block), then
    // the def yaw bias [orig: @0x456B1C out[3] += weaponDef+0x14].
    out[0] = e.pos[0];
    out[1] = e.pos[1];
	out[2] = io::bam_add(e.pos[2], 0x20000);
	out[3] = io::bam_add(e.heading, wb.facing_bam);
	out[4] = e.pitch;
	out[5] = e.roll;
	const bool secondary = &wb == &e.profile.fire_b;
	const int count_index = secondary ? AiBrain::kBoneCountB : AiBrain::kBoneCountA;
	const int count = std::clamp(b.f[count_index], 0, 16);
	int userpoint = 0;
	if (count > 0) {
		const int seed = b.f[secondary ? AiBrain::kAmmoB : AiBrain::kAmmoA];
		uint32_t selection;
		if (seed > 0)
			selection = uint32_t(seed) % uint32_t(count);
		else {
			if (b.f[AiBrain::kBoneRoundRobin] == 0)
				b.f[AiBrain::kBoneRoundRobin] = -1;
			selection = uint32_t(b.f[AiBrain::kBoneRoundRobin]) % uint32_t(count);
			b.f[AiBrain::kBoneRoundRobin] = io::bam_sub(b.f[AiBrain::kBoneRoundRobin], 1);
		}
		b.bytes()[AiBrain::kBoneFlagByte] = uint8_t(selection) | 0x80;
		userpoint = b.f[count_index + 1 + selection];
		if (world.pose_provider != nullptr) {
			if ((flags & 1) == 0 ||
					!world.pose_provider->resolve_userpoint_pivot(world, e.handle, userpoint, out))
				world.pose_provider->resolve_userpoint_rigid(world, e.handle, userpoint, out);
		}
	}
	const auto refine_muzzle = [&]() {
		if (userpoint != 0 && world.pose_provider != nullptr) {
			int32_t posed[6];
			if (world.pose_provider->resolve_userpoint_transform(world, e.handle, userpoint, posed))
				std::copy_n(posed, 3, out);
		}
	};

	// The WEAPON_PITCHLOCKED legs fire at a fixed/commanded elevation with no
	// target solve, no cone gate, and no slew [orig: @0x45705D].
	if ((flags & 0x10u) != 0 || (flags & 0x8u) != 0) {
        if ((flags & 0x10u) != 0)
            out[4] += static_cast<int32_t>(0xE0000020u); // -45 deg-ish [orig: @0x457061]
        else
            out[4] += b.f[AiBrain::kElevationBias]; // [orig: @0x45706A brain+0x314]
        if ((flags & 0x1u) != 0) {
            // WEAPON_TURRET staging: stage the pose, mirror the yaw, snap active
            // [orig: @0x45707D..0x457113; the current-pose muzzle refine behind
            // it needs the model handle our no-list leg never has — retail
            // returns 1 right there too].
            for (int i = 0; i < 6; ++i) b.f[AiBrain::kStagingBlock + i] = out[i];
            b.f[AiBrain::kStagingBlock + 3] = -1 - out[3] - wb.facing_bam; // [orig: @0x4570B3]
            for (int i = 0; i < 6; ++i)
                b.f[AiBrain::kActiveBlock + i] = b.f[AiBrain::kStagingBlock + i];
        }
		if ((flags & 1) != 0)
			refine_muzzle();
		return true;
	}

    if (target == nullptr) return false;
	int32_t metrics[6];
	if (!weapon_target_metrics(world, e, *target, out, aim_offset, skip_los, metrics))
		return false;
	const int32_t hdist = metrics[0], rel_yaw = metrics[3], rel_pitch = metrics[4];

	// Compose the solved relative yaw/pitch into the ORIGINAL vehicle frame,
	// before the authored facing bias, then extract the full Euler pose.
	const int32_t zero[3] = {};
	const CollisionMatrix base = collision_matrix_from_euler(e.heading, e.pitch, e.roll, zero);
	const CollisionMatrix relative = collision_matrix_from_euler(rel_yaw, rel_pitch, 0, zero);
	CollisionMatrix composed;
	for (int row = 0; row < 3; ++row)
		for (int col = 0; col < 3; ++col) {
			uint64_t dot = 0x200000;
			for (int k = 0; k < 3; ++k)
				dot += uint64_t(int64_t(base.m[row * 4 + k]) * relative.m[k * 4 + col]);
			composed.m[row * 4 + col] = int32_t(dot >> 22);
		}
	collision_matrix_to_euler(composed, out + 3);

	// The cone gate: |relative angle| in 1/256-turn units vs the def cone with a
	// floor of 1 [orig: @0x456D17..0x456D6B — HIBYTE fold, limit =
	// (weaponDef+8 | 0x2000000) >> 25].
	const int32_t limit = (wb.cone_bam | 0x2000000) >> 25;
    uint32_t yaw_mag = static_cast<uint32_t>(rel_yaw) >> 24;
    if (yaw_mag >= 0x80u) yaw_mag = 256u - yaw_mag;
    if (yaw_mag > static_cast<uint32_t>(limit)) return false;
    uint32_t pitch_mag = static_cast<uint32_t>(rel_pitch) >> 24;
    if (pitch_mag >= 0x80u) pitch_mag = 256u - pitch_mag;
    if (pitch_mag > static_cast<uint32_t>(limit)) return false;

    if ((flags & 0x1u) == 0) return true; // no turret tracking [orig: @0x456D71 -> ret 1]

    // WEAPON_TURRET staging [orig: @0x456D7B]: {hdist, ?, dist, yaw, pitch, ?},
    // staged yaw mirrored minus the def bias [orig: @0x456DB7 -1 - yaw - def+0x14].
	const int32_t dist = metrics[2];
	b.f[AiBrain::kStagingBlock + 0] = hdist;
	b.f[AiBrain::kStagingBlock + 1] = 0;
    b.f[AiBrain::kStagingBlock + 2] = dist;
    b.f[AiBrain::kStagingBlock + 3] = -1 - rel_yaw - wb.facing_bam;
    b.f[AiBrain::kStagingBlock + 4] = rel_pitch;
    b.f[AiBrain::kStagingBlock + 5] = 0;

    const auto snap_active = [&] {
        for (int i = 0; i < 6; ++i)
            b.f[AiBrain::kActiveBlock + i] = b.f[AiBrain::kStagingBlock + i];
    };

    if ((flags & 0x2u) == 0 && (flags & 0x4u) == 0) {
        snap_active(); // instant turret [orig: @0x456DDC]
		refine_muzzle();
		return true;
	}

    // WEAPON_SLOW / WEAPON_FAST slew toward the staged yaw. Aligned when the
    // remaining delta is under one step's threshold; otherwise advance the
    // ACTIVE yaw by the fixed slew step and hold fire [orig: SLOW @0x456E52 —
    // threshold step*0x18C6318, slew +-0x2108421; FAST @0x456EF9 — exactly
    // double, threshold step*0x318C631, slew +-0x4210842].
    const int32_t staged = b.f[AiBrain::kStagingBlock + 3];
    const int32_t active = b.f[AiBrain::kActiveYaw];
    const int32_t delta = staged - active;
    const bool fast = (flags & 0x4u) != 0 && (flags & 0x2u) == 0;
    const int32_t threshold =
            b.f[AiBrain::kStep] * (fast ? 0x318C631 : 0x18C6318);
    if (iabs32(delta) < threshold) {
        snap_active();
		refine_muzzle();
		return true;
	}
    const int32_t slew = fast ? 0x4210842 : 0x2108421;
    b.f[AiBrain::kActiveYaw] = active + (delta > 0 ? slew : -slew);
    return false;
}

// The engagement block [orig: @0x4677b3..0x4678b2]: the sees quad, Entity_SetAITarget,
// reset the combat timer, set the fire-delay with the exact PRNG jitter (the
// has_controller branch guards the jitter by base-delay and uses the inline LCG; the
// other branch always jitters via PRNG_Next16), pending = 17, then the targeted quad.
// The rel_ops trace keeps the recorded shape the tests pin; the APPLY is D-AI-3.
void AiSystem::engage_target(World &world, AiEntity &e, const AiTarget &t, bool aircraft) {
	AiBrain &b = e.brain;
	const int32_t self_rm = static_cast<int32_t>(static_cast<int16_t>(e.relmat_id)); // movsx entity+284
    const int32_t tgt_rm = static_cast<int32_t>(static_cast<int16_t>(t.relmat_id));  // movsx target+284

    // [orig: 0x4677b3..0x4677e2] the sees quad before the controller branch.
    rel_ops.push_back({kRelEventSpecial, self_rm, tgt_rm});
    rel_ops.push_back({kRelSharedMem, e.net_id, tgt_rm});
    rel_ops.push_back({kRelProximity, self_rm, t.net_id});
    rel_ops.push_back({kRelEnemy, e.net_id, t.net_id});
    {
        const Entity *se = world.registry.get(e.handle);
        const Entity *te = t.handle.valid() ? world.registry.get(t.handle) : nullptr;
        if (se != nullptr && te != nullptr) apply_engage_relations(world, *se, *te);
    }

    target_set_calls.push_back(t.net_id); // [orig: Entity_SetAITarget(entity, best_target) @0x45d760]
    ai_set_target(world, e, t.handle);
    b.f[AiBrain::kCombatTimer] = 0;        // [orig: ai_comp[40] = 0]
    b.f[AiBrain::kFireDelay] = e.profile.field104; // [orig: ai_comp[41] = *(profile+104)]
	// Both aircraft branches use the guarded A-stream jitter. [orig: @0x466460]
	if (aircraft || t.has_controller) {
		// [orig: branch A — jitter only if base_delay != 0; inline LCG on dword_31BFBB8]
		if (e.profile.field104 != 0)
            b.f[AiBrain::kFireDelay] += static_cast<int32_t>(static_cast<uint16_t>(prng_step_a()) % 62);
	} else {
		// [orig: branch B — always jitter; PRNG_Next16 on dword_31BFBB0]
		b.f[AiBrain::kFireDelay] += static_cast<int32_t>(world.next_prng16() % 62);
	}
	b.set_pend(aircraft ? kAiHeloCombat : kAiGroundCombat); // [orig: @0x466460 / @0x467730]

	// [orig: 0x467883..0x4678b2] the targeted quad after pending = 17.
	rel_ops.push_back({kRelAllied, self_rm, tgt_rm});
    rel_ops.push_back({kRel452B30, e.net_id, tgt_rm});
    rel_ops.push_back({kRelDamaged, self_rm, t.net_id});
    rel_ops.push_back({kRelSpotted, e.net_id, t.net_id});
}

// [orig: AI_HandleCommand @0x465770] Queued ChangeAI dispatcher. Alert, authored
// state, aim/drive skill, speed, weapons-free, and elevation commands are live;
// cases 0x0C..0x14 remain explicit deferred arms. Combat event types (1/3/4)
// are not commands, so the original returns 0 and the event switch proceeds;
// this faithfully returns false.
bool AiSystem::ai_handle_command(AiEntity &e, const AiEventEntry &ev) {
    int32_t t = ev.type();
    switch (t) {
    case 6: { // ChangeAI alert level [orig: AI_HandleCommand case 6
              //  @0x4657a6..0x465816 <- Entity_ApplyCommand queues {6, level}
              //  @0x43ac59/@0x43acc4/@0x43ad34 for subs 5/22/6]
        int32_t level = ev.f[3];
        if (level < 0) level = 0;
        if (level > 2) level = 2;
        if (e.brain.f[AiBrain::kPrevAlert] != level) {
            // On ANY change the stored level is FORCED to 2: mov eax, ebx
            // runs unconditionally in the changed branch [orig: @0x4657cd],
            // so a green/yellow command onto a differing brain still drives
            // both alert words red — a witnessed retail quirk, carried.
            level = 2;
            const int32_t cur = e.brain.f[AiBrain::kCurState];
            if ((e.profile.flags96 & 2) == 0) {
                // Raw def+0x10 type words with literal state ids: the kong
                // auto-comment and our vehicle enum disagree on the 1/2
                // naming, so neither name is trusted here.
                // [orig: type 1 -> pend 10 unless cur in {14, 6}
                //  @0x4657d6..0x4657e6; type 2 -> pend 18 unless cur == 22,
                //  both behind !(def+0x60 & 2) @0x4657d0/@0x4657f0]
                if (e.profile.type == 1 && cur != 14 && cur != 6)
                    e.brain.set_pend(10);
                else if (e.profile.type == 2 && cur != 22)
                    e.brain.set_pend(18);
            }
        }
        e.brain.f[AiBrain::kPrevAlert] = level; // [orig: @0x465803]
        e.brain.f[AiBrain::kAlert] = level;     // [orig: @0x465809]
        return true;
    }
    case 7: { // AISETSTATE [orig: AI_HandleCommand @0x465770 case 7]
        int32_t next = -1;
        const int32_t authored = ev.f[3];
        // [orig: AIState_SetByEntityType @0x457570] The authored 1..5
        // selector maps through a different state table for HELO and GROUND.
        if (e.profile.type == 1) {
            switch (authored) {
                case 1: next = kAiHeloFormation; break;
                case 2: next = kAiHeloReturnToBase; break;
                case 3: next = kAiHeloPretty; break;
                case 4: next = kAiHeloLand; break;
                case 5: next = kAiHeloFollowWp; break;
                default: break;
            }
        } else if (e.profile.type == 2) {
            switch (authored) {
                case 1: next = kAiGroundFormation; break;
                case 2: next = kAiGroundReturnToBase; break;
                case 3: next = kAiGroundPretty; break;
                case 5: next = kAiGroundFollowWp; break;
                default: break; // selector 4 has no GROUND state arm
            }
        }
        if (next >= 0) {
            e.brain.f[AiBrain::kFallback] = next;
            e.brain.set_pend(next);
        }
        return true;
    }
    case 8: // AIMSKILL [orig: AI_HandleCommand @0x465770 case 8]
        e.brain.f[AiBrain::kAccuracy] = std::clamp(ev.f[3], 0, 4);
        return true;
    case 9: // DRIVESKILL [orig: AI_HandleCommand @0x465770 case 9]
        // Declared residual: brain+176 has no reader here yet (the vehicle
        // driver skill consumer is unported).
        e.brain.f[AiBrain::kDriveSkill] = std::clamp(ev.f[3], 0, 4);
        return true;
    case 10:
    case 11: { // COMBATSPEED / PATROLSPEED [orig: AI_HandleCommand @0x465770
               //  cases 0xA @0x46589a / 0xB @0x4658d3]
        // Retail treats a negative signed dword as its unsigned value before
        // converting authored km/h to 16.16 world-units/tick: fild, then
        // fadd 2^32 when negative [orig: flt_7C3288 @0x4658a4/@0x4658da],
        // fmul 1000.0 [orig: flt_7C6EC0], fmul the stored float32 of
        // 1/225000 [orig: flt_7C6EBC = 4.444444584805751e-06], fmul 65536
        // [orig: flt_7C32BC], ftol2_sse @0x76bc00 (out-of-range converts to
        // the x87 indefinite 0x80000000 — the branch below).
        double value = static_cast<double>(ev.f[3]);
        if (value < 0.0) value += 4294967296.0;
        const double scaled = value * 1000.0 * 4.444444584805751e-06 * io::kFp16OneD;
        const int32_t fixed =
                (scaled >= 2147483648.0 || scaled < -2147483648.0)
                        ? static_cast<int32_t>(0x80000000u)
                        : static_cast<int32_t>(scaled);
        e.brain.f[t == 10 ? AiBrain::kSpeedA : AiBrain::kSpeedB] = fixed;
        return true;
    }
    case 0x15: // stationary weapons-free [orig: @0x4659A7 — arg 0 clears byte
               // +785; nonzero pushes the type's combat state (GROUND -> 17,
               // HELO -> 8) into pending when different, then sets it 1]
        if (ev.f[3] != 0) {
            int32_t combat_state = -1;
            if (e.profile.type == 1) combat_state = kAiHeloCombat;
            else if (e.profile.type == 2) combat_state = kAiGroundCombat;
            if (combat_state >= 0 &&
                e.brain.f[AiBrain::kCurState] != combat_state)
                e.brain.set_pend(combat_state);
            e.brain.bytes()[AiBrain::kGuardFireByte] = 1;
        } else {
            e.brain.bytes()[AiBrain::kGuardFireByte] = 0;
        }
        return true;
    case 0x16: // commanded elevation, degrees -> BAM32 [orig: @0x4659EF —
               // brain+0x314 = arg * 11930464; the solver's WEAPON_PITCHLOCKED leg]
        e.brain.f[AiBrain::kElevationBias] =
                static_cast<int32_t>(ev.f[3] * kBamPerDegreeInt);
        return true;
    default:
        if (t >= 0x0C && t <= 0x14) ++unported_calls; // deferred command arms
        return false;
    }
}

} // namespace opennova::world
