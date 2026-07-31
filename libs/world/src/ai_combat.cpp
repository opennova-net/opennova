#include "world/ai.h"

// Split out of ai.cpp (quality campaign W3-3). Motion only — every body is
// unchanged, and each original-code citation moved with the code it annotates.
//
// P2 ground combat: target acquisition and the perception gates, engagement
// relations, line of sight, ally alerting, AI fire, and command handling.

#include "terrain/height_field.h"
#include "world/angle.h"
#include <algorithm>
#include <cmath>

#include "ai_detail.h"

#include "world/world.h"

namespace opennova::world {

using namespace detail; // the shared AI helpers, unqualified as before

// ----------------------------------------------------------------------------
// P2: GROUND combat + targeting.
// ----------------------------------------------------------------------------

int AiSystem::index_of(const AiEntity &e) const {
    return static_cast<int>(&e - entities_.data());
}

// [orig: inline LCG on dword_31BFBB8] / [orig: PRNG_Next16 @0x6130a0 on dword_31BFBB0]. Both are
// the same rotate-LCG over different global state; the low 16 bits feed the %62 fire-delay jitter.
int32_t AiSystem::prng_step_a() { return static_cast<int32_t>(prng_step(prng_a)); }
int32_t AiSystem::prng_step16() { return static_cast<int32_t>(prng_step(prng16)); }

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

// The candidate FEED (D-AI-1, witnessed world-wac-ai-re §16.2): build the eligible list
// from the registry pools with the witnessed gates, then run the byte-exact scoring core.
// [orig: AI_FindBestTargetB @0x466f60 iterates g_pool_list in-function per profile
// weapon-slot class (+40+4i -> pools 0/1/2 with the building sub-filter).] Tracked
// deviations (ledger D-AI-1): the class table comes from the unparsed ai.def profile, so
// pools 0 and 1 scan unconditionally; the priority target (profile+148) and the building
// class remain unmodeled; LOS = line_of_sight_clear (terrain leg, D-AI-7).
bool AiSystem::acquire_target(World &world, AiEntity &e, AiTarget &out) {
    scan_candidates_.clear();
    // The teamless entry gate lives in the scoring core (it is part of @0x466f60);
    // mirrored here only to skip the wasted pool walk.
    if (e.team == 0 && !e.see_all) return acquire_target_from(e, scan_candidates_, out);
    for (int pool = 0; pool <= 1; ++pool) {
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
            AiCandidate cand;
            cand.handle = h;
            cand.pos[0] = static_cast<int32_t>(c->position.x * 65536.0f);
            cand.pos[1] = static_cast<int32_t>(c->position.y * 65536.0f);
            cand.pos[2] = static_cast<int32_t>(c->position.z * 65536.0f);
            cand.team = c->team;
            cand.flags = static_cast<int32_t>(c->engine_flags); // &2/&0x8000000 skip, &0x4000 prio x6
            cand.health = static_cast<int16_t>(std::min<int32_t>(c->health, INT16_MAX));
            cand.visibility = c->ai_target_refcount;       // [orig: +530 refcount saturation]
            // Per-candidate engage-range caps [orig: words +422/+420] — def fields, not yet
            // modeled on Entity: uncapped (the profile's own range still gates).
            cand.range_primary = INT32_MAX / 2;
            cand.range_secondary = INT32_MAX / 2;
            cand.relmat_id = c->group_id;                  // group key (+0x11C commandGroup)
            cand.net_id = c->net_id;                       // single key (+0x7C SSN)
            cand.has_controller = (c->owner_connection_id != 0);
            cand.is_priority = false;                      // [orig: profile+148 — unmodeled]
            cand.los_blocked = !line_of_sight_clear(world, e.pos, cand.pos, e.handle, h);
            scan_candidates_.push_back(cand);
        }
    }
    return acquire_target_from(e, scan_candidates_, out);
}

// [orig: AI_FindBestTargetB @0x466f60 scoring walk] over an explicit candidate list.
// Per-candidate perception gates (team, flags, health, self, refcount saturation) then
// FOV/range/stealth scoring + LOS; the priority target bypasses scoring on clear LOS.
bool AiSystem::acquire_target_from(AiEntity &e, const std::vector<AiCandidate> &candidates,
                                   AiTarget &out) {
    ++find_target_calls;
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
            if (!c.los_blocked) { // Entity_CheckMutualLineOfSight @0x539be0
                out = AiTarget{c.relmat_id, c.net_id, c.has_controller, c.handle};
                return true;
            }
            continue; // priority but LOS blocked -> skip (no best-of, matches the orig else-if)
        }
        if (score > best_score && !c.los_blocked) { // [orig: else if (score > best_score) + LOS]
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
    TriggerRelations &rel = world.relations;
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
    if (old_packed != 0) {
        const EntityHandle old{static_cast<uint16_t>(old_packed - 1)};
        if (Entity *oe = world.registry.get(old)) {
            if (oe->ai_target_refcount > 0) --oe->ai_target_refcount; // dec, clamp >= 0
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

// LOS between two 16.16 fire-origin points — true = clear. The terrain leg is the
// ported heightmap segment raycast; the sector leg clips the ray against pool-2 /
// pool-1 collision models (the D-AI-7 leg). [orig: Entity_CheckMutualLineOfSight
// @0x539be0 -> Physics_RaycastTerrainAndSectors @0x539910, ray radius 0, 1 = clear.]
// No terrain wired -> clear, the headless-test default; no collision world wired
// (terrain-only unit tests) -> terrain leg alone.
bool AiSystem::line_of_sight_clear(World &world, const int32_t a[3], const int32_t b[3],
                                   EntityHandle from, EntityHandle to) const {
    if (terrain == nullptr || !terrain->valid()) return true;
    // The original tests the segment between the two FIRE ORIGINS — Entity_CheckMutual-
    // LineOfSight @0x539be0 feeds two Entity_ComputeWeaponFireOrigin @0x43b4b0 results
    // into the ray — never the ground-level entity origins (feet-to-feet sampling
    // false-blocks on the very ground both stand on). Until the bone seam lands, lift
    // both endpoints by the same 0.9 u chest stand-in the fire pass uses (D-AI-6/-7).
    constexpr int32_t kChestLift = 0xE666; // 0.9 u [orig: the def+1350 muzzle bone stand-in]
    const int32_t la[3] = {a[0], a[1], a[2] + kChestLift};
    const int32_t lb[3] = {b[0], b[1], b[2] + kChestLift};
    if (collision != nullptr) return collision->raycast_clear(world, la, lb, from, to);
    return !los_terrain_blocked(*terrain, la, lb);
}

// [orig: Entity_AlertNearbyAllies @0x4654b0] same-team, alive, non-building entities
// within `radius` (16.16): own AiSlot+136 = 2 and each ally's brain alert = 2. Container
// rebase: the original scans pool 1; our brains live on the AiEntity list, so the walk
// covers every brained entity (pool 0 organics included) — values written are identical.
void AiSystem::alert_nearby_allies(World &world, AiEntity &e, int32_t radius) {
    (void)world;
    e.slot.bytes()[AiSlot::kMoveFlagByte] = 2;
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
    if (world.ammo.by_index(ammo_index) == nullptr) return false;
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
                                      world.weapons.by_index(
                                              shooter->equipped_adm_index) != nullptr
            ? shooter->equipped_adm_index
            : static_cast<uint8_t>(ammo_index & 0xFF);
    ev.adm_index = adm_index;
    world.rounds.add(ev);

    RoundSpawnParams rp;
    rp.owner = e.handle;
    rp.shooter_handle = e.handle.packed;
    rp.origin.x = static_cast<float>(origin[0]) / 65536.0f;
    rp.origin.y = static_cast<float>(origin[1]) / 65536.0f;
    rp.origin.z = static_cast<float>(origin[2]) / 65536.0f;
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

// The engagement block [orig: @0x4677b3..0x4678b2]: the sees quad, Entity_SetAITarget,
// reset the combat timer, set the fire-delay with the exact PRNG jitter (the
// has_controller branch guards the jitter by base-delay and uses the inline LCG; the
// other branch always jitters via PRNG_Next16), pending = 17, then the targeted quad.
// The rel_ops trace keeps the recorded shape the tests pin; the APPLY is D-AI-3.
void AiSystem::engage_target(World &world, AiEntity &e, const AiTarget &t) {
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
    if (t.has_controller) {
        // [orig: branch A — jitter only if base_delay != 0; inline LCG on dword_31BFBB8]
        if (e.profile.field104 != 0)
            b.f[AiBrain::kFireDelay] += static_cast<int32_t>(static_cast<uint16_t>(prng_step_a()) % 62);
    } else {
        // [orig: branch B — always jitter; PRNG_Next16 on dword_31BFBB0]
        b.f[AiBrain::kFireDelay] += static_cast<int32_t>(static_cast<uint16_t>(prng_step16()) % 62);
    }
    b.set_pend(kAiGroundCombat); // [orig: ai_comp[5] = 17]

    // [orig: 0x467883..0x4678b2] the targeted quad after pending = 17.
    rel_ops.push_back({kRelAllied, self_rm, tgt_rm});
    rel_ops.push_back({kRel452B30, e.net_id, tgt_rm});
    rel_ops.push_back({kRelDamaged, self_rm, t.net_id});
    rel_ops.push_back({kRelSpotted, e.net_id, t.net_id});
}

// [orig: AI_HandleCommand @0x465770] command dispatcher (cases 6..0x16). Deferred to the
// AI-command phase. The combat event types (1/3/4) are not commands, so the original returns 0
// for them and the event switch proceeds; this faithfully returns false.
bool AiSystem::ai_handle_command(AiEntity &, const AiEventEntry &ev) {
    int32_t t = ev.type();
    if (t >= 6 && t <= 0x16) ++unported_calls; // a real command would be handled by the AI-command phase
    return false;
}

} // namespace opennova::world
