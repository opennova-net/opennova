// The authoritative round simulation — server-side flight, impact, damage, and death
// detection for every fired round. [orig: the 512-slot round array @ 0xB7E1A8 (128 groups
// x 4 sub-slots x 780 B, active-flag bytes @ 0xB7DFA0), ticked by
// Weapon_UpdateAllProjectiles @ 0x4EC020 -> Projectile_UpdatePhysics @ 0x4E9D70 from
// Entity_UpdateAllEntities; spawned SYNCHRONOUSLY at fire time by RoundData_SpawnRound
// @ 0x4EC0D0 (called inline from RoundData_AddRound @ 0x4FDB40 — the ring is only the
// tag-2 fan-out log); damage chain Projectile_HandleEntityImpact @ 0x4E9390 ->
// Projectile_ProcessDamageOnTarget @ 0x4E7FB0 -> Weapon_CalcImpactDamage @ 0x4EC920,
// with authority-owned relation/health/death mutation and a shared peer callback/
// presentation tail. docs/net/novaworld-net-re.md §5.60.]
//
// Reimpl altitude — current retail-alignment boundary (§5.60):
//  * hit test = one CollisionWorld query in retail order: refined terrain, water,
//    static CFAC, dynamic CFAC, then persons. Static/dynamic faces use the exact
//    fixed-point CNRM/CFAC/CVRT routine; models without a face mesh retain the
//    bounded item-sphere fallback. Persons use posed COBJ bone spheres from either
//    the host matrix provider or explicitly published matrices, with a bounded
//    torso fallback. ray[31] selects reactions/death while ray[32] selects normal
//    infantry damage; recovered head/limb/special multipliers consume ray[32].
//  * stock ballistic flight sweeps with the pre-force Q16 velocity, then on a miss
//    applies the retail 167-Q16 gravity step and aerodynamic drag-table force. The
//    zero-speed, submerged-slow, Ignore, NoGravity, UseOwnMove, stability damping,
//    overshoot, and underwater x25 gates follow the recovered branch order.
//  * damage includes the ItemDef-presence gate, exact signed-32 overflow arithmetic,
//    MP authority/OneShotKill, one carrier hop, impact-armor/dead/indestructible/NoDie
//    gates, person and seat zones (including the critical 0x800 entity flag), per-ammo
//    shooter class, and vehicle occupant-count reduction. Static/vehicle targets are
//    no longer merely blockers when promoted item traits make them damageable.
//  * a pre-arm entity contact resolves notarmmedammo and keeps an active logical
//    dud child at the contact point. The modeled prefix preserves owner, kinematics,
//    and elapsed age; the dud supplies its max age, while the trail/emitter slot at
//    retail byte +692 is deliberately not inherited. Pool-slot identity and other
//    bytes outside LiveRound remain unrepresentable rather than guessed; this in-slot
//    projection first advances on the following RoundSim tick.
//  * remaining flight gaps are the randomized tumble PRNG/local frame, the guided
//    (designator/missile) callbacks, and float LiveRound position/velocity carriers
//    around the per-tick fixed-point core. Unresolved/unposed persons continue to
//    use the characterized torso fallback.
//  * useownmove rounds run the throwable class motors (world/throwables.cpp,
//    world-wac-ai-re §27): grenade arc/bounce/fuse, satchel/claymore stick +
//    placed-device conversion; the spawn dispatches instantkillzone (0x400),
//    Detonatesatchels (0x20), DesignateTarget (0x02000000), and the claymore
//    pellet fan (0x20000) before the ballistic default, and the PowerThrow charge
//    byte scales launch speed.
//  * spawn-time Weapon_CalcRandomSpreadOffset and recoil follow the recovered
//    player/rules/stance gates. Impact-energy armor-density loss and shell-eject
//    physics remain outside this model.
//  * kztype Knife(1)/Medic(3) raycast leaves and item-placing ammo (`hasitem`) do not
//    spawn a sim round.
#ifndef OPENNOVA_WORLD_ROUND_SIM_H
#define OPENNOVA_WORLD_ROUND_SIM_H

#include <array>
#include <cstdint>
#include <unordered_map>
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

struct AmmoTableEntry; // world/ammo_table.h

enum class ThrowClass : uint8_t; // world/throwables.h

// A client re-runs S2C tag-2 descriptors for presentation, but none of that
// flight may produce authoritative gameplay consequences. Presentation state
// (trails, cosmetic cadence, impacts, debug) still advances normally. Carry the
// mode on each round so mixed callers cannot infer safety from zero damage alone.
enum class RoundConsequenceMode : uint8_t {
    Authoritative,
    VisualOnly,
};

// The shooter fields RoundData_SpawnRound reads while applying weapon ERROR and
// ammo recoil. Ordinary authoritative shots resolve these from the World owner;
// a pure client has no World entity for a decoded peer, so its persistent wire
// row supplies this immediate, non-owning projection instead.
// [orig: RoundData_SpawnRound @0x4EC0D0]
struct RoundSourceState {
    bool person_with_item_def = false;
    bool player = false;
    uint8_t stance_category = 2; // 0 prone, 1 crouch/mounted, 2 standing/air/water
    bool scope_raised = false;
    bool underwater = false;
    int32_t *recoil_pitch = nullptr;              // entity+0x380
    const int32_t *weapon_weight_spread = nullptr; // entity+0x384
};

struct RandomSpreadOffset {
    int32_t yaw_bam = 0;
    int32_t pitch_bam = 0;
};

// Deterministic retail dispersion helper. Constants are loaded as binary32 but
// the x87 intermediates remain PC53 until the final truncations.
// [orig: Weapon_CalcRandomSpreadOffset @0x4E4120]
RandomSpreadOffset weapon_calc_random_spread_offset(
        int32_t spread_fp16, uint32_t seed, int32_t vertical_fp16,
        bool use_spread_two);

// The distinct shotgun pellet-fan distribution: one draw selects a radial
// distance through cos([0,pi/2]), the next selects its full-circle phase.
// [orig: Weapon_SpawnProjectileBurstWithSpread @0x4EBBB0]
RandomSpreadOffset weapon_calc_shotgun_spread_offset(
        int32_t pie_slice_bam, uint16_t radial_draw, uint16_t phase_draw);

struct RoundSpawnParams {
    EntityHandle owner;               // the shooter entity (skipped in the hit test)
    uint16_t shooter_handle = 0xFFFF; // pool<<12|slot, for death credit
    // The decoded shooter's carrier at fire time (visual-only rounds): keeps a
    // mounted shooter's re-simulated round from clipping the shooter's own
    // vehicle proxy — the wire-side ray[18] mount-exclusion analog.
    uint16_t shooter_carrier_handle = 0xFFFF;
    // The decoded wire shooter's team, for rounds spawned WITHOUT a local owner
    // entity (the joiner's remote rounds): retail resolves the wire shooter and
    // copies its team — the friend/enemy TrcrID item and tracer styling key on it;
    // 0xFF is only the truly-unresolvable-source arm.
    // [orig: the sourceEntity team copy @0x4ec705; the !sourceEntity arm @0x4ec721]
    uint8_t shooter_team = 0xFF;
    Vec3 origin;                      // fire origin, mission units
    int32_t dir_yaw_bam = 0;          // wire fire direction (engine-frame BAM32, §5.16)
    int32_t dir_pitch_bam = 0;
    int32_t ammo_index = -1;          // resolved adm round_type -> AmmoTable index
    uint8_t adm_index = 0;
    uint16_t shot_seq = 0;
    // Shooter fire-context composite (ring+31). Bit 7 selects the vertical
    // projectile ERROR row; the remaining bits are opaque here.
    uint8_t subtype = 0;
    // The PowerThrow charge byte [orig: fire descriptor +20 <- MountSlot+0x5C;
    // 1..254 scale the ammo velocity by charge/256 @ 0x4ec5bb, 0/255 = full].
    // Rides the wire as the round event's slot_byte (ring+32).
    uint8_t charge = 0;
    // The wire round-event flags byte, for rounds re-spawned from a received
    // descriptor. Zero for host/AI-originated fire. See FireEvent::wire_round_flags.
    // [orig: NetPacket_DeserializeRoundEvent @0x42f270 arm tests @0x42f521/@0x42f6ce]
    uint8_t wire_round_flags = 0;
    // Immediate-only source override for a decoded remote shooter that has no
    // Entity/AiEntity in this World. Never retained by RoundSim.
    RoundSourceState *source_state = nullptr;
};

// One in-flight round. [orig: 780-B record; the fields we simulate: pos, velocity
// (+152/156/160), ammo index (+620), remaining age (+684, seeded from +676), owner
// (+368), shot-seq word (+120).]
struct LiveRound {
    bool active = false;
    RoundConsequenceMode consequence_mode = RoundConsequenceMode::Authoritative;
    EntityHandle owner;
    uint16_t shooter_handle = 0xFFFF;
    uint16_t shooter_carrier_handle = 0xFFFF;
    int32_t ammo_index = -1;
    uint8_t adm_index = 0;
    uint16_t shot_seq = 0;
    // Host-local lifetime identity for presentation. Pool slots are reused,
    // potentially between two rendered frames during fixed-tick catch-up; a
    // slot index alone would teleport an old owned effect onto the new round.
    uint64_t presentation_generation = 0;
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

    // --- throwable state (zeroed on ballistic rounds; world-wac-ai-re §27) ---
    // Orientation + spin [orig: round +16/+20/+24 angles, +164/+168/+172 spin
    // rates; the class init callback seeds 1 deg/tick @ 0x4435A0].
    int32_t yaw_bam = 0;
    int32_t pitch_bam = 0;
    int32_t roll_bam = 0;
    int32_t spin_yaw = 0;
    int32_t spin_pitch = 0;
    int32_t spin_roll = 0;
    uint8_t bounce_count = 0;      // [orig: byte +341]
    // The TrcrID item the round renders as (items.def id - 100000) — picked
    // friendly/enemy by the spawning host's team [orig: +28 ItemTypeIndex
    // @ 0x4ec79b -> Entity_InitFromItemDef]. The class/motor binding survives
    // a non-tracer cadence shot; only its separate visible-model pointer clears.
    int32_t item_type_id = 0;
    // Class bindings resolved from the item's ai_function/move_function tags
    // [orig: itemDef updateCallback -> +452, deathCallback -> +456].
    ThrowClass motor{};
    ThrowClass think{};
    // Landed-on / stuck-to entity [orig: +40 groundEntity; the motors parent
    // and follow it].
    EntityHandle parent;
    uint64_t parent_spawn_id = 0;
    Vec3 parent_prev_pos;
    int32_t parent_prev_yaw_bam = 0;
    bool parent_tracking = false;
    // Armed at 2 ticks remaining above water; the expiry queues the kill zone
    // [orig: the runtime 0x1000 flag @ 0x444a29 consumed by the update head].
    bool det_at_expiry = false;
};

// Retail keeps the selected TrcrID item/class bound independently of tracer
// cadence (@0x4ec79b..0x4ec7b7), but clears the round+0x30 visible-model
// pointer when this particular shot is not a tracer (@0x4ec900).
inline int32_t round_visible_item_id(const LiveRound &round) {
    return round.tracer ? round.item_type_id : 0;
}

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
// drains these and presents the enabled legs of the ammo effects_table row for the tag,
// with the emitter forward = the flight direction. Retail ballistic rounds select and
// spawn both legs from the physical impact handlers at collision time
// [orig: Projectile_UpdatePhysics @ 0x4E9D70 -> Projectile_SpawnImpactEffect
// @ 0x4E9B80]. Custom motors can select the legs independently: grenade bounces call
// AmmoDef_ProcessImpactEffect directly with sound enabled and particles disabled
// [orig: Entity_UpdateGrenadePhysics @ 0x4447D3..0x444824]. Weapon_RaycastAndSpawnImpact
// @ 0x4E8460 is a separate Knife-only
// instant-kill-zone leaf (flags&0x400, kztype==1) whose ray extent is
// AmmoDef.kz_maxradius; it is not the bullet path. Terrain surface-map overrides
// and the building material-1 special tag remain D-WPN-15 (net-re §5.60).
struct RoundImpact {
    Vec3 position;
    Vec3 direction;         // normalized flight direction (the witnessed descriptor dir)
    int32_t ammo_index = -1;
    int32_t effect_tag = 0; // canonical effect-tag index [orig: g_AmmoEffectTagTable
                            //  @ 0x813420; world/ammo_table.h kImpactEffectTagNames]
    bool present_effect = true; // submit the row's particle-effect leg
    bool present_sound = true;  // play the row's impact-sound leg
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
    int16_t primary_section = -1;
    int16_t secondary_section = -1;
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
    // Which arm of retail's round-event RECEIVE path produced this, for rounds that
    // came off the wire. The two arms are mutually exclusive and pick different
    // presentation entirely: bit 0 set -> the ammo-def arm, which plays ammoDef+64 and
    // spawns ammoDef+68 at the wire position; bit 0 clear with bit 1 set -> the
    // adm-indexed arm, which spawns NO ammo-def effect and instead executes the
    // addressed ADM def's fire and recoil action rows at the weapon's own userpoint.
    // Zero means this fire did NOT come off the wire (our own host/AI presentation,
    // which retail runs inline at the shooter instead) and keeps the ammo-def legs.
    // [orig: NetPacket_DeserializeRoundEvent @0x42f270 — bit 0 @0x42f521, bit 1
    //  @0x42f6ce; ammo arm @0x42f527..0x42f6c7 (sound @0x42f5dc, effect @0x42f6c2);
    //  adm arm action pair @0x42f777/@0x42f785 and @0x42f98f/@0x42f9d0]
    uint8_t wire_round_flags = 0;
    // The ADM def the adm arm addresses. It is the WIRE-ADDRESSED def, not the
    // observed shooter's equipped weapon — conflating them is a port bug.
    // [orig: AdmDef_GetEntryByIndex @0x42f6d9]
    uint8_t adm_index = 0;
};

// One resolved hit-test outcome, kept in a persistent ring for the F3 Rounds
// debug view (developer tooling over our port — not retail-mimicked UI). The
// ring is never drained: hosts snapshot it read-only, so a headless server
// pays only the ring writes.
struct RoundDebugEvent {
    enum Kind : uint8_t {
        kOrganic = 0,      // pool-0 posed person-section hit (or unresolved fallback)
        kItemFace = 1,     // pool-1/2 CFAC face hit (section/face/material valid)
        kItemSphere = 2,   // pool-1/2 bound-sphere stand-in (no face mesh)
        kTerrain = 3,      // terrain column stop
        kExpired = 4,      // max-age expiry / timed fuze
        kFaceMiss = 5,     // bound-sphere graze whose face walk missed — round flew on
    };
    uint32_t tick = 0;
    uint8_t kind = kExpired;
    uint8_t material = 0;    // CFAC byte (kItemFace), fixed 19 for organic
    int16_t section = -1;    // primary COBJ section (item face / organic bone)
    int16_t secondary_section = -1; // organic ray[32], lowest overlapping bone
    bool organic_fallback = false; // synthetic torso sphere, not authored COBJ
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

// Compatibility fallback for an organic whose graphic/COBJ block could not be
// resolved. Normal organic collision uses posed per-section spheres.
inline constexpr float kOrganicStandInCenterZ = 0.9f;
inline constexpr float kOrganicStandInRadius = 0.6f;

class RoundSim {
public:
    static constexpr int kCapacity = 512; // [orig: 128 groups x 4 sub-slots @0xB7E1A8]

    // Heap-backed pool (fixed kCapacity size for the life of the sim): a World
    // embeds this sim, and hosts/tests stack-allocate Worlds — the 512-slot
    // record array stays off that footprint.
    std::vector<LiveRound> rounds = std::vector<LiveRound>(kCapacity);
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
    // Raw retail weapon-dispersion rules seam. Session rules bit 0x2000 and
    // offline rules bit 0x4000 select this same helper gate; recoil, shotgun
    // fans, and ammo-def fallback ERROR do not read it.
    // [orig: RoundData_SpawnRound @0x4EC0D0]
    bool weapon_spread_enabled = true;

    // Spawn one round at fire time [orig: RoundData_SpawnRound @ 0x4EC0D0 default path].
    // Returns the round slot, or -1 (pool full / non-ballistic ammo / null ammo).
    int spawn(World &world, const RoundSpawnParams &params,
              RoundConsequenceMode mode = RoundConsequenceMode::Authoritative);

    // The pellet fan for claymore-flag ammo [orig: Weapon_SpawnProjectileBurst
    // @ 0x4EB900]. Returns the first pellet slot or -1.
    int spawn_burst(World &world, const RoundSpawnParams &params,
                    const AmmoTableEntry &ammo, RoundConsequenceMode mode,
                    bool shotgun_spread = false);

    // One 62 Hz step for every live round [orig: Weapon_UpdateAllProjectiles @ 0x4EC020
    // -> Projectile_UpdatePhysics @ 0x4E9D70]: advance along velocity, terrain stop,
    // ordered terrain/water/item/person collision through `collision`, fixed-point
    // gravity/drag on a surviving miss, authority damage, death detection, and
    // presentation/debug consequences. A null override uses World::collision and
    // finally a headless fallback query world.
    void tick(World &world, const terrain::TerrainHeightField *terrain,
              CollisionWorld *collision = nullptr);

    // Mission restart discards all transient projectile/presentation state.
    void reset() noexcept;

private:
    uint64_t next_presentation_generation_ = 1;
    // Retail advances tracer cadence on each remote shooter's weapon slot. A
    // visual tag-2 round has wire H but no local Entity owner, so this is that
    // per-remote presentation field projected onto the decoded identity.
    std::unordered_map<uint16_t, uint32_t> remote_visual_tracer_counters_;
};

// Queue an explosive round's kill zone at its stop [orig: the kztype-gated
// WeaponEffect_PushExplosionQueueEntry push @ 0x4e83c0 the impact/expiry
// handlers run]. Shared with the throwable motors (world/throwables.cpp).
void detonate_round(World &world, const LiveRound &round, const Vec3 &at,
                    const AmmoTableEntry &ammo);

} // namespace opennova::world

#endif // OPENNOVA_WORLD_ROUND_SIM_H
