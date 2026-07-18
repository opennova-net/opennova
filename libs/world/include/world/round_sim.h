// The authoritative round simulation — server-side flight, impact, damage, and death
// detection for every fired round. [orig: the 512-slot round array @ 0xB7E1A8 (128 groups
// x 4 sub-slots x 780 B, active-flag bytes @ 0xB7DFA0), ticked by
// Weapon_UpdateAllProjectiles @ 0x4EC020 -> Projectile_UpdatePhysics @ 0x4E9D70 from
// Entity_UpdateAllEntities; spawned SYNCHRONOUSLY at fire time by RoundData_SpawnRound
// @ 0x4EC0D0 (called inline from RoundData_AddRound @ 0x4FDB40 — the ring is only the
// tag-2 fan-out log); damage chain Projectile_HandleEntityImpact @ 0x4E9390 ->
// Projectile_ProcessDamageOnTarget @ 0x4E7FB0 -> Weapon_CalcImpactDamage @ 0x4EC920,
// AUTHORITY-ONLY. docs/net/novaworld-net-re.md §5.60.]
//
// Reimpl altitude — tracked deferrals (§5.60 port follow-ups):
//  * ORGANIC hit test = per-tick segment vs a fixed organic cylinder (the witnessed
//    person bone-section collision is unported). Hit ZONE resolution is therefore
//    body-only (zone multiplier 1.0; head 1.25 / limbs 0.5 / 13-14 3.0 wait on bone
//    hits). ITEM hits (pools 1/2) run the witnessed bound-sphere broad phase + the
//    collision-model FACE narrow phase with the material-tagged impact
//    [orig: Projectile_RaycastProximitySlots @ 0x4E5340 ->
//    Physics_RaycastAgainstBoneCollision @ 0x4E4CB0]; models with no face mesh keep
//    the bound-sphere stand-in (D-ITEM-1's bounded fallback).
//  * no drag/gravity yet (Projectile_ApplyDragDeceleration internals unwitnessed) — the
//    round flies straight at muzzle speed, so kinetic damage does not yet fall off.
//  * no weapon spread on the sim round (the 0x06 carries the claimed pre-spread pose;
//    Weapon_CalcRandomSpreadOffset unported), no water plane, no bounce/shell physics.
//  * kztype Knife(1)/Medic(3) raycast leaves and item-placing ammo (`hasitem`) do not
//    spawn a sim round; explosive kill zones (kz_* radius damage) apply direct-hit
//    kinetic damage only.
#ifndef OPENNOVA_WORLD_ROUND_SIM_H
#define OPENNOVA_WORLD_ROUND_SIM_H

#include <array>
#include <cstdint>
#include <vector>

#include "world/entity.h"
#include "world/geom.h"
#include "world/tracer_trails.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class CollisionWorld;

class World;

struct RoundSpawnParams {
    EntityHandle owner;               // the shooter entity (skipped in the hit test)
    uint16_t shooter_handle = 0xFFFF; // pool<<12|slot, for death credit
    Vec3 origin;                      // fire origin, mission units
    int32_t dir_yaw_bam = 0;          // wire fire direction (engine-frame BAM32, §5.16)
    int32_t dir_pitch_bam = 0;
    int32_t ammo_index = -1;          // resolved adm round_type -> AmmoTable index
    uint8_t adm_index = 0;
    uint16_t shot_seq = 0;
};

// One in-flight round. [orig: 780-B record; the fields we simulate: pos, velocity
// (+152/156/160), ammo index (+620), remaining age (+684, seeded from +676), owner
// (+368), shot-seq word (+120).]
struct LiveRound {
    bool active = false;
    EntityHandle owner;
    uint16_t shooter_handle = 0xFFFF;
    int32_t ammo_index = -1;
    uint8_t adm_index = 0;
    uint16_t shot_seq = 0;
    Vec3 pos;            // mission units
    Vec3 vel;            // mission units per TICK [orig: velocity = ammo speed / 62]
    int32_t age_ticks = 0;
    int32_t max_age_ticks = 0;
    // Tracer presentation state [orig: RoundData_SpawnRound @0x4ec184-0x4ec1e5 decision;
    // team = round+0x162, 0xFF when there is no shooter]: the host present pass draws
    // the trail channels in flight.
    bool tracer = false;
    uint8_t team = 0xFF;
    // The round's trail channel [orig: round+0x2B4 <- CEffectEmitterPool_AllocSlot
    // @ 0x4ec774]; -1 = no visual (non-tracer, NoTracers rules, or pool full).
    int32_t trail_slot = -1;
};

// A death the damage pass detected this tick — drained by the host session, which owns
// the wire (S2C 0x13 / 0x1E / 0x26 staging) and the respawn queue [orig: the death
// handlers run inline in the entity update; our libs/world stays transport-free].
struct RoundDeath {
    EntityHandle victim;
    EntityHandle killer;
    uint16_t victim_handle = 0xFFFF;
    uint16_t killer_handle = 0xFFFF;
    uint8_t adm_index = 0;
};

// A round impact the flight pass resolved this tick — the IMPACT-EFFECT seam. The host
// drains these and spawns the ammo effects_table row for the tag (effect + sound), with
// the emitter forward = the flight direction. Retail ballistic rounds select and spawn
// this row from the physical impact handlers at collision time
// [orig: Projectile_UpdatePhysics @ 0x4E9D70 -> Projectile_SpawnImpactEffect
// @ 0x4E9B80]. Weapon_RaycastAndSpawnImpact @ 0x4E8460 is a separate Knife-only
// instant-kill-zone leaf (flags&0x400, kztype==1) whose ray extent is
// AmmoDef.kz_maxradius; it is not the bullet path. Terrain surface-map sampling,
// entity material, and the water plane remain D-WPN-15 (net-re §5.60).
struct RoundImpact {
    Vec3 position;
    Vec3 direction;         // normalized flight direction (the witnessed descriptor dir)
    int32_t ammo_index = -1;
    int32_t effect_tag = 0; // canonical effect-tag index [orig: g_AmmoEffectTagTable
                            //  @ 0x813420; world/ammo_table.h kImpactEffectTagNames]
    uint32_t tick = 0;      // authoritative presentation tick for catch-up aging
    uint64_t source_order = 0; // stable order across impacts resolved on the same tick
};

// A processed (non-zero) damage hit — drained by AiSystem::tick to stamp the victim's
// AI reaction state (wasHit / lastAttacker / the SM damage event). [orig: the damage
// chain writes the victim entity + queues the AI event inline
// (Projectile_ProcessDamageOnTarget @ 0x4E7FB0); our sim/AI split records instead.]
struct RoundHit {
    EntityHandle victim;
    EntityHandle shooter;
    int32_t damage = 0;
};

// One presented fire — the origin/direction/ammo of a spawned round, drained by the
// HOST present layer for the fire sound + muzzle effect (+ the MF-light deferral).
// [orig: WeaponSlot_FireAndSpawnEffects @ 0x53F440 presents inline at fire time on
// the firing host (ammo-def 'ai_launch' sound +64 via Sound_PlayWithDistanceAttenuation
// @ 0x528E40, 'ai_launcheffect' muzzle +68 via CEffectWorld_SpawnEmitterAtPosition
// @ 0x5F6DF0); our libs stay render-free, so the sim records at the same moment and
// the host drains — the remote-client analog re-fires the ring records (§5.60).]
struct FireEvent {
    EntityHandle shooter;
    uint16_t shooter_handle = 0xFFFF;
    int32_t ammo_index = -1;
    Vec3 origin;          // mission units
    int32_t yaw_bam = 0;  // fire direction (engine-frame BAM32, §5.16)
    int32_t pitch_bam = 0;
};

// One resolved hit-test outcome, kept in a persistent ring for the F3 Rounds
// debug view (developer tooling over our port — not retail-mimicked UI). The
// ring is never drained: hosts snapshot it read-only, so a headless server
// pays only the ring writes.
struct RoundDebugEvent {
    enum Kind : uint8_t {
        kOrganic = 0,      // pool-0 body stand-in hit
        kItemFace = 1,     // pool-1/2 CFAC face hit (section/face/material valid)
        kItemSphere = 2,   // pool-1/2 bound-sphere stand-in (no face mesh)
        kTerrain = 3,      // terrain column stop
        kExpired = 4,      // max-age expiry / timed fuze
        kFaceMiss = 5,     // bound-sphere graze whose face walk missed — round flew on
    };
    uint32_t tick = 0;
    uint8_t kind = kExpired;
    uint8_t material = 0;    // CFAC face material byte (kItemFace)
    int16_t section = -1;    // COBJ section index (kItemFace / kFaceMiss)
    int32_t face = -1;       // face index within the section (kItemFace)
    int32_t effect_tag = -1; // impact tag handed to the present pass
    uint16_t entity = 0xFFFF;   // packed EntityHandle of the struck entity
    uint16_t shooter = 0xFFFF;  // packed EntityHandle of the round's owner
    int32_t ammo_index = -1;
    bool husk = false;       // target was in the husk-swapped (destroyed) state
    float t = 0.0f;          // hit parameter along the tick segment
    Vec3 p0, p1;             // the tick's flight segment (mission units)
    Vec3 hit;                // resolved stop / graze point (mission units)
};

class RoundSim {
public:
    static constexpr int kCapacity = 512; // [orig: 128 groups x 4 sub-slots @0xB7E1A8]

    std::array<LiveRound, kCapacity> rounds{};
    int active_count = 0;

    // Deaths detected by the damage pass, in tick order. The host session drains this
    // every tick (npruntime server tick) and stages the death broadcasts.
    std::vector<RoundDeath> deaths;

    // Impacts resolved this tick, in tick order — drained by the presenting host
    // every frame (the sim stays render-free). Bounded: a headless server never
    // drains, so emission stops at the cap instead of growing without bound.
    static constexpr size_t kMaxPendingImpacts = 256;
    std::vector<RoundImpact> impacts;
    uint64_t next_impact_order = 1;

    // Processed hits (damage > 0), in tick order — drained by AiSystem::tick before the
    // per-entity updates (wasHit / lastAttacker / SM damage events).
    std::vector<RoundHit> hits;

    // Fires spawned since the last presentation drain (every spawn records one, the
    // local player's included — the present pass self-filters). Drained by the host
    // present layer each tick; see FireEvent for the witness map.
    std::vector<FireEvent> fired;

    // The F3 Rounds debug ring: the last kDebugTrailCap resolved outcomes
    // (hits, terrain stops, expiries, AND face-miss fly-ons), newest replacing
    // oldest. Read-only snapshots; reset() clears it.
    static constexpr int kDebugTrailCap = 48;
    std::array<RoundDebugEvent, kDebugTrailCap> debug_trail{};
    int debug_trail_next = 0;  // ring cursor (next write slot)
    int debug_trail_count = 0; // valid entries, saturates at the cap

    // The tracer trail channels — appended per round tick, drained per pool tick,
    // styled and drawn by the host present pass (world/tracer_trails.h witness map).
    TracerTrailPool trails;

    // The presenting client's identity, for the friendly/enemy style select AT SPAWN
    // [orig: RoundData_SpawnRound @ 0x4ec740 compares the round team byte to
    // g_local_player_entity->Team, shooter == local player counts friendly]. The host
    // stamps these before ticking (SP listen-server: the local avatar); remote-client
    // presentation re-runs its own select when it re-fires ring records (net-re §5.60).
    EntityHandle local_player;
    uint8_t local_team = 0;
    // The MP NoTracers rules bit [orig: dword_24D1E34 & 1] — kills the tracer visual
    // unless FORCETRACER. SP hosts leave it false; the net seam wires it later.
    bool no_tracers_rule = false;

    // Spawn one round at fire time [orig: RoundData_SpawnRound @ 0x4EC0D0 default path].
    // Returns the round slot, or -1 (pool full / non-ballistic ammo / null ammo).
    int spawn(World &world, const RoundSpawnParams &params);

    // One 62 Hz step for every live round [orig: Weapon_UpdateAllProjectiles @ 0x4EC020
    // -> Projectile_UpdatePhysics @ 0x4E9D70]: advance along velocity, terrain stop,
    // organic hit test, the item bound-sphere broad phase + the collision-model
    // face narrow phase (through `collision`, husk-aware; null = sphere-only),
    // authority damage, death detection.
    void tick(World &world, const terrain::TerrainHeightField *terrain,
              CollisionWorld *collision);

    // Mission restart discards all transient projectile/presentation state.
    void reset() noexcept;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ROUND_SIM_H
