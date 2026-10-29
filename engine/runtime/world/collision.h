// World-object collision: the runtime queries the original engine runs against a
// model's collision block (the .3di CDTA volumes/planes), plus the per-tick
// proximity tables that feed them. Witness record: docs/world/world-wac-ai-re.md
// §15 (collision + blink boxes), grilled 2026-07-09 against Jointops.exe.
//
// Runtime data (the query-side layout, witnessed in the three query functions):
//   COBJ section records — volume list + local AABB + bound sphere
//   BVOL volume records  — collidable type + local AABB + plane run + flags
//   BPLN plane records   — int16 Q14 normal + 16.16 distance
// The embedder builds these PODs from the parsed .3di collision IR (the same
// leaf-consumer seam as terrain_query's TerrainHeightField, ADR 0020): engine/runtime/world
// never touches the format stack.
//
// Collidable types (runtime dispatch, witnessed in
// Entity_ComputeBoneCollisionForce @ 0x4ae150):
//   1  generic collision box ("CB") -> ordinary solid contact and generic rays
//   4  ladder volume ("CL")  -> contact flag 0x1 + ladder alignment frame
//   6  armory volume ("CA")  -> contact flag 0x4 -> entity Flags |= 0x400000
//      (gates the in-game armory screen: input action 218 opens weapon.mnu WEAPON
//       only while this flag is set [orig: Input_HandleActionBinding @ 0x49b848])
//   7  vehicle collision ("VC") -> solid on the vehicle-contact mask 0x8 path
//   8  blink box ("BB")      -> contact flag 0x10 + blink accumulation (buildings)
//   9  CD door touch         -> contact flag 0x20 + door-section bit on target
//   10 CT change-team box    -> contact flag 0x200
//   11 vehicle-loadout volume -> contact flag 0x400 -> entity Flags |= 0x800
//      (gates vehicle.mnu VEHICLE on the same key [orig: @ 0x49b858])
//   12 masked volume (mask 0x10 path)
//   13 CF flag volume        -> contact flag 0x800 (source stands on target;
//      the authoring manual describes the family as special-function/FARP activation)
//   16 DH damage high        -> contact flag 0x100 (-50 HP)
//   17 DM damage medium      -> contact flag 0x80  (-6 HP)
//   18 DL damage low         -> contact flag 0x40  (-1 HP)
//   19 CP player collision   -> solid only on the player mask 0x2 (not AI)
//   1 (and other unlisted types) solid -> SAT push-out force
//   5  contact-no-force marker
// Raycasts test ONLY type-1 volumes [orig: Entity_RaycastCollisionModel @ 0x413060];
// the blink point query tests ONLY type-8 volumes of building-kind entities
// [orig: Entity_TestCollisionSections @ 0x4aef90].
#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <runtime/devtools/tick_profile.h>
#include <runtime/world/collision_model.h> // the model records, the matrix, the terrain helpers
#include <runtime/world/collision_query.h> // the query records, the wire proxies, the projectile trace
#include <runtime/world/entity.h>

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;
class EntityRegistry;

// Static proximity coordinates are signed whole mission units stored in u16
// table words. Decode explicitly instead of relying on signed left-shift wrap.
inline int32_t static_slot_coord_units(uint16_t word) {
    const int32_t raw = static_cast<int32_t>(word);
    return raw >= 0x8000 ? raw - 0x10000 : raw;
}

inline int32_t static_slot_coord_q16(uint16_t word) {
    return static_slot_coord_units(word) * 0x10000;
}

// ----------------------------------------------------------------------------
// CollisionWorld: the proximity tables + per-entity instances, and the
// world-level blink state. [orig: the g_StaticProx* table is built at mission
// start and on the teleport paths by Entity_BuildAllProximityLists @ 0x4c20f0
// -> Entity_BuildProximityLists_Pool2 @ 0x4b9430; the g_DynProx*/g_PersonProx*
// tables are rebuilt every tick by Entity_BuildProximityLists_Pool01
// @ 0x4b9340 (Entity_UpdateAllEntities @ 0x4c240a); the g_ProxCandidateArena
// slices every 17th tick by Entity_BuildProximityListsFromPools @ 0x4b8eb0
// (gate @ 0x4c2416).]
// ----------------------------------------------------------------------------
class CollisionWorld {
public:
    // Debug-card tap: the last resolve_entity's strongest-contact source (the
    // candidate whose model produced push-out force) — the frozen-clump
    // instrument. Reset at each resolve; single-threaded like the resolver.
    EntityHandle dbg_last_contact{};
    int32_t dbg_last_contact_item = 0;
    // Opt-in, per-tick projectile-trace profile: the probe/F3 attribution
    // surface for sustained-fire cost. Times are microseconds; *_survivors
    // count geometric broad-phase passes and *_faces count CFAC face-set sizes
    // admitted to the static/dynamic narrow phases.
    struct TraceProfile {
        int64_t calls = 0;
        int64_t terrain_us = 0;
        int64_t static_us = 0;
        int64_t dynamic_us = 0;
        int64_t person_us = 0;
        int64_t static_survivors = 0;
        int64_t dynamic_survivors = 0;
        int64_t person_survivors = 0;
        int64_t static_faces = 0;
        int64_t dynamic_faces = 0;
    };
    // Opt-in ray-debug capture: every segment query records one event into a
    // per-category ring while enabled (dev tooling — the F3 ray view/window
    // feed, not a ported surface). Per-category rings keep recurring per-frame
    // categories (replication LOS, sun visibility) from evicting one-shot rays
    // (a bullet, a pick) inside the view's fade window; `total` counts every
    // recorded event so ring overwrite never hides true throughput.
    enum class RayDebugCategory : uint8_t {
        kUncategorized = 0,
        kProjectile,
        kKnife,
        kThrowable,
        kAiLos,
        kReplicationLos,
        kScriptLos,
        kExplosionLos,
        kGroundProbe,
        kCameraIris,
        kRenderOcclusion,
        kSunVisibility,
        kSoundOcclusion,
        kPrecipitation,
        kPick,
        kCount,
    };
    static const char *ray_debug_category_name(RayDebugCategory category);
    // result: 0 = clear/miss (ray ran its full length), 1 = hit with a resolved
    // point in `hit`, 2 = blocked (boolean query, no hit point resolved).
    enum : uint8_t {
        kRayDebugClear = 0,
        kRayDebugHit = 1,
        kRayDebugBlocked = 2,
    };
    struct RayDebugEvent {
        FixedVec3 start;
        FixedVec3 end; // the requested endpoint
        FixedVec3 hit; // the resolved stop/clip point (== end when none)
        uint32_t tick = 0;
        uint8_t category = 0; // RayDebugCategory
        uint8_t result = kRayDebugClear;
        uint16_t reserved = 0;
    };
    struct RayDebugRing {
        std::vector<RayDebugEvent> events; // cap once enabled, empty when off
        int32_t next = 0;
        int32_t count = 0;   // saturates at the cap
        uint64_t total = 0;  // lifetime recorded (the drop-honesty counter)
    };
    static constexpr int32_t kRayDebugCapPerCategory = 256;
    // The presentation filter the ray view/window share: bit i of the mask
    // draws category i; TTL is the fade window in 62 Hz ticks. Recording is
    // never filtered — rings capture everything, the mask gates drawing only.
    static constexpr uint32_t kRayDebugMaskAll = 0x7FFF;
    uint32_t ray_debug_mask() const { return ray_debug_mask_; }
    void set_ray_debug_mask(uint32_t mask) { ray_debug_mask_ = mask & kRayDebugMaskAll; }
    int32_t ray_debug_ttl_ticks() const { return ray_debug_ttl_ticks_; }
    void set_ray_debug_ttl_ticks(int32_t ticks) {
        ray_debug_ttl_ticks_ = ticks < 1 ? 1 : (ticks > 620 ? 620 : ticks);
    }
    bool ray_debug_enabled() const { return ray_debug_enabled_; }
    // Disabled by default; each enable/disable edge clears the rings (disable
    // also frees them — hosts/tests stack-allocate Worlds, keep pools off that
    // footprint) so a new consumer never inherits another capture's events.
    void set_ray_debug_enabled(bool enabled);
    const std::array<RayDebugRing,
                     static_cast<size_t>(RayDebugCategory::kCount)> &
    ray_debug_rings() const { return ray_debug_rings_; }
    // Hot-path recorder: a single cold branch when disabled. The category is
    // the active RayDebugScope override when set, else `fallback`.
    void ray_debug_record(RayDebugCategory fallback, uint32_t tick,
                          const int32_t a[3], const int32_t b[3],
                          const int32_t *hit_or_null, uint8_t result) const;
    // Outermost-wins RAII category tag: a scope takes effect only while no
    // outer scope is active, so a script-LOS caller keeps its tag through the
    // AI LOS leg it routes through. Cheap either way (one member write).
    class RayDebugScope {
    public:
        RayDebugScope(const CollisionWorld *cw, RayDebugCategory category)
                : cw_(cw) {
            if (cw_ != nullptr &&
                cw_->ray_debug_scope_ == RayDebugCategory::kUncategorized &&
                category != RayDebugCategory::kUncategorized) {
                cw_->ray_debug_scope_ = category;
                owns_ = true;
            }
        }
        RayDebugScope(const CollisionWorld &cw, RayDebugCategory category)
                : RayDebugScope(&cw, category) {}
        ~RayDebugScope() {
            if (owns_) cw_->ray_debug_scope_ = RayDebugCategory::kUncategorized;
        }
        RayDebugScope(const RayDebugScope &) = delete;
        RayDebugScope &operator=(const RayDebugScope &) = delete;

    private:
        const CollisionWorld *cw_;
        bool owns_ = false;
    };
    // Opt-in contact/hit capture (dev tooling — the F3 collision view's hit
    // flashes and the Physics window's counts, not a ported surface): resolved
    // per-body outcomes keyed by the touched entity, so the overlay can light
    // the box it already draws. One ring, not per-category — hits are sparse
    // next to rays — with per-kind lifetime totals for drop-honesty. The same
    // cold-branch/enable-edge contract as the ray capture.
    enum class ContactDebugKind : uint8_t {
        kProjectileHit = 0, // trace_projectile resolved on an entity/person
        kKnifeHit,          // trace_knife_impact resolved
        kMoveContact,       // resolve_entity solid push-out (pass 0)
        kVehicleHull,       // resolve_vehicle_probes wall-like push
        kTerrainHit,        // a trace resolved on terrain (marker only)
        kWaterHit,
        kCount,
    };
    static const char *contact_debug_kind_name(ContactDebugKind kind);
    struct ContactDebugEvent {
        FixedVec3 pos;            // world 16.16 hit/contact point
        uint32_t tick = 0;
        uint16_t target = 0xFFFF; // EntityHandle.packed of the flashed body (0xFFFF none)
        uint8_t kind = 0;         // ContactDebugKind
        uint8_t hit_class = 0xFF; // ProjectileHitClass for the trace kinds
    };
    struct ContactDebugRing {
        std::vector<ContactDebugEvent> events; // cap once enabled, empty when off
        int32_t next = 0;
        int32_t count = 0;   // saturates at the cap
        uint64_t total = 0;  // lifetime recorded (the drop-honesty counter)
        uint64_t kind_totals[static_cast<size_t>(ContactDebugKind::kCount)] = {};
    };
    static constexpr int32_t kContactDebugCap = 256;
    // The presentation filter (bit i of the mask flashes kind i) and the flash
    // window; recording is never filtered — the ring captures everything.
    static constexpr uint32_t kContactDebugMaskAll = 0x3F;
    static constexpr int32_t kContactDebugTtlTicks = 62; // ~1 s flash window
    uint32_t contact_debug_mask() const { return contact_debug_mask_; }
    void set_contact_debug_mask(uint32_t mask) {
        contact_debug_mask_ = mask & kContactDebugMaskAll;
    }
    bool contact_debug_enabled() const { return contact_debug_enabled_; }
    // Disabled by default; each enable/disable edge clears the ring (disable
    // also frees it — the ray-capture footprint rationale).
    void set_contact_debug_enabled(bool enabled);
    const ContactDebugRing &contact_debug_ring() const { return contact_debug_ring_; }
    // Hot-path recorder: a single cold branch when disabled.
    void contact_debug_record(ContactDebugKind kind, uint32_t tick, EntityHandle target,
                              const int32_t pos[3], uint8_t hit_class) const;
    const TraceProfile &trace_profile() const { return trace_profile_; }
    // The profile's consumer clears what it read (the board's feed at each logic tick's sink),
    // so every trace counts in one reading: those ahead of a tick's table build (the projectile
    // pass), a render frame's between ticks, and a tick that builds no tables (a host alone,
    // whose entity update does not run) read their own traces, never a running sum.
    void clear_trace_profile() const { trace_profile_ = TraceProfile{}; }
    bool trace_profile_enabled() const { return trace_profile_enabled_; }
    // Profiling is disabled by default. Each enable/disable edge clears the
    // snapshot so a new consumer never inherits another capture's counters.
    void set_trace_profile_enabled(bool enabled);
    // --- model registry (embedder-fed, keyed by an opaque graphic id) ---
    int32_t add_model(CollisionModel model); // returns model id
    const CollisionModel *model(int32_t id) const;
    // Attach a model instance to a live entity (net_id keyed like the traits sweep).
    void assign_entity(EntityHandle h, int32_t model_id,
                       uint64_t registry_spawn_id = 0);
    // Drop both intact and husk bindings for one packed slot. Hosts use this
    // when the registry serial proves the slot now belongs to another entity.
    void remove_entity_instance(EntityHandle h);
    // Attach the husk-stage collision model (swapped in while Flags & 4).
    void assign_entity_husk(EntityHandle h, int32_t husk_model_id);
    // Attach the final husk's model (entity+0x38), which serves as the husk model
    // once the swap stage promotes it (Entity::husk_promoted).
    void assign_entity_final_husk(EntityHandle h, int32_t final_husk_model_id);
    // Install the embedder's pose seam that supplies final per-section
    // matrices. Null restores the static shared-entity-matrix fallback.
    void set_pose_provider(IPoseProvider *provider);
    // Resolve an embedder-owned late-spawn instance on demand. Existing instances
    // never call the provider, so repeated round/F3 queries are idempotent.
    bool ensure_entity_instance(World &world, EntityHandle h);
    // Publish the pose owner's current world-space Q22 section matrices. Retail
    // collision consumes the model callback's live matrices rather than deriving
    // animation itself. Count must exactly match the assigned model's COBJ count.
    bool publish_entity_section_matrices(EntityHandle h,
                                         std::vector<CollisionMatrix> matrices);
    void clear_entity_section_matrices(EntityHandle h);
    bool has_instance(const World &world, EntityHandle h) const;
    // The entity's current world-space section matrix (the same slot array the
    // traces consume: published pose, else the host callback, else the placement
    // matrix). False when the entity has no collision instance or the section
    // is out of range. The scar writer transposes it to store a bone-local slot
    // [orig: Scar_AddEntry @0x5ccc99..0x5ccca5].
    bool entity_section_matrix(const World &world, EntityHandle h, int section,
                               CollisionMatrix &out) const;
    bool has_instance(EntityHandle h) const;
    // The attached collision model id for a live entity (-1 = no instance);
    // the userpoint leg of the muzzle-pose provider keys its parsed model by it.
    int32_t entity_model_id(EntityHandle h) const;
    // The husk-stage model id the entity holds (-1 = none or no instance): its
    // final husk's once the swap stage promoted it.
    int32_t entity_husk_model_id(const Entity &e) const;
    size_t instance_count() const { return instances_.size(); }

    // --- the per-tick snapshot ---
    // [orig: Entity_BuildProximityLists_Pool01 @ 0x4b9340] persons (pool 0) into the
    // full-precision repulsion table; dynamics (pool 1) into the dyn table, every
    // tick. The statics table (pool 2, [orig: Entity_BuildProximityLists_Pool2
    // @ 0x4b9430]: buildings first [0..static_building_count), all statics
    // [0..static_count), positions quantized (p + 0x8000) >> 16 as u16, radius
    // padded +111876, cap 1200) is built at the first build and then only at retail's
    // Pool2 calls: the load and the mission start (Entity_BuildAllProximityLists
    // @ 0x4c20f0: build_initial_tables, build_all_tables) and the teleports
    // (rebuild_statics), never per tick.
    // [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0] per-entity candidate
    // slices (dyn radius+4.0u / statics; pool-1 radius+6.0u) into a 3000-entry arena.
    void build_tick_tables(World &world);
    // Mission-init pool snapshot and candidate slices for pre-logic consumers
    // such as portal setup and first-tick grounding. The 17-tick cadence only
    // governs steady-state rebuilds.
    void build_initial_tables(World &world);
    // A spawn or registry rewind immediately republishes the pool-0 / pool-1 tables and
    // every candidate slice; the statics stand (a Pool2 call is build_all_tables').
    void refresh_after_registry_change(World &world);
    // Every table: the statics, the pool-0 / pool-1 tables and every slice (or, before the
    // first slices, the pre-logic snapshot). The mission start's local-player block and a
    // restart's reload. [orig: Entity_BuildAllProximityLists @0x4C20F0, called by
    //  Entity_InitAllFromModels @0x40E5A1 and Game_StartMission @0x525C90]
    void build_all_tables(World &world);
    // Direct candidate rebuild plus this entity's blink refresh. Pool position
    // snapshots stay at their published epoch; unrelated contacts are retained.
    // [orig: WacCmd_Tele @0x4F22D0 -> 0x4B8EB0, 0x4B3DC0]
    void refresh_entity_proximity(World &world, Entity &entity);
    // The candidate-slice build alone, outside the 17-tick cadence (which it
    // restarts): every source's slice over the published pool tables.
    // [orig: Entity_BuildProximityListsFromPools @0x4B8EB0, called by
    //  Entity_RespawnVehicle @0x460133]
    void rebuild_candidate_slices(World &world);
    // The statics table alone (buildings first, then the rest), outside the instance and
    // registry edges that otherwise rebuild it: the teleport actions' Pool2 calls.
    // [orig: Entity_BuildProximityLists_Pool2 @0x4B9430, called by
    //  Entity_TeleportTeamToSpawn @0x43D4F8, EventAction_TeleportEntityToSpawn @0x43E1F2,
    //  WacCmd_TeleSsn @0x4F7F3E]
    void rebuild_statics(World &world);
    // The pool-0 / pool-1 tables alone, no slice build (the statics too only where an
    // instance or registry edge left them dirty): the teleport actions' Pool01 calls.
    // [orig: Entity_BuildProximityLists_Pool01 @0x4B9340, called by
    //  Entity_TeleportTeamToSpawn @0x43D4FD, WacCmd_TeleSsn @0x4F7F43]
    void rebuild_pool01(World &world);
    // The mission start's authority rebuild after the WAC initial run: the pool-0/1
    // tables, then every source's slice (which restarts the 17-tick cadence); the
    // statics table stands as the @0x525C90 rebuild left it.
    // [orig: Game_StartMission @0x524360 - Entity_BuildProximityLists_Pool01 @0x525E63,
    //  Entity_BuildProximityListsFromPools @0x525E68]
    void rebuild_pool01_and_slices(World &world);

    // Atomically replace the persistent decoded collision projection. Sorting
    // by wire handle gives both domains deterministic order; the wire-keyed
    // candidate arena is separate from candidates_, so a numerically equal
    // packed wire and registry handle can never alias. There is deliberately
    // no projectile-only compatibility mutator: projectile traces and replica
    // sun lighting consume this same snapshot.
    // `skipped` names the decoded rows the tables pass over (WireSliceRow): a wire slice
    // stands while a proxy or a skipped row names its handle with the lifetime it was built
    // for (D-COL-20).
    void replace_wire_collision_proxies(
            std::vector<WirePersonCollisionProxy> persons,
            std::vector<WireDynamicCollisionProxy> dynamics,
            uint16_t local_player_wire_handle = 0xFFFF,
            std::vector<WireSliceRow> skipped = {});
    // A decoded person's update ran after the table was built: its proxy takes the
    // person's live Position and dead bit (a handle no proxy names is ignored), which
    // the next repulsion's dead skip and push and the next trace's bone walk re-read
    // through the table's entity pointer; the table's copy, which the coarse gates
    // read, stays as built (D-NET-196).
    // [orig: Entity_MovementCollisionResolver @0x4B2BD0 (the reach gate on the copy
    //  @0x4B3AF6..0x4B3B74, the live dead skip @0x4B3B89..0x4B3B8D, the live reads
    //  @0x4B3B93..0x4B3B99)]
    void set_wire_person_proxy_live(uint16_t wire_handle, const FixedVec3 &position,
                                    bool dead, uint32_t live_flags);
    // A decoded pool-1 mover moved after the table was built: its proxy takes the
    // mover's live Position, heading and pitch / roll (a handle no proxy names is
    // ignored), which the next trace's narrow phase reads through the table's entity
    // pointer; the table's copy, which the slot gate reads, stays as built (D-NET-196).
    // [orig: Projectile_RaycastProximitySlots @0x4E5340 (the slot gate on the copy
    //  @0x4E55E6..0x4E575A, g_DynProxEntity @0x4E5760); Physics_RaycastAgainstBoneCollision
    //  @0x4E4CB0]
    void set_wire_dynamic_proxy_live(uint16_t wire_handle, const FixedVec3 &position,
                                     int32_t heading_bam, int32_t pitch_bam,
                                     int32_t roll_bam, uint32_t live_flags);
    // The decoded person's current proxy (null when none names the handle).
    const WirePersonCollisionProxy *wire_person_proxy(uint16_t wire_handle) const;
    // The decoded pool-1 mover's current proxy (null when none names the
    // handle): the pose and the verified registry twin every trace reads.
    const WireDynamicCollisionProxy *wire_dynamic_proxy(uint16_t wire_handle) const;

    // Segment arbitration shared by authoritative and visual-only projectile
    // loops. The query is read-only: callers must publish/build collision
    // snapshots at the normal tick seam before tracing.
    // `out_ground` takes the entity whose geometry clamped the height (the
    // query's own out word; a client's wire-proxied mover names its registry
    // twin), kInvalid when none did, even where the terrain floor then wins.
    // [orig: Entity_ComputeClampedDisplacement @0x4ad6a0, the out store
    // @0x4ad80e..0x4ad810]
    int32_t minefield_ground(const World &world, EntityHandle source,
                             FixedVec3 position, bool indoors,
                             EntityHandle *out_ground = nullptr) const;

    ProjectileHit trace_projectile(const World &world,
                                   const ProjectileTrace &trace) const;

    // The instant Knife presenter walks terrain, water, then the PERSON prox
    // table (replacing on a distance tie), then buildings and items (strict),
    // and every entity pool uses authored CFAC geometry. In particular, pool-0
    // persons do not use the ballistic bone-sphere path and unresolved models
    // do not gain a sphere substitute. The hit's `surface_type` is the CFAC
    // material; the caller maps it to the effect row (person material 1 =
    // flesh). [orig: Weapon_RaycastAndSpawnImpact @0x4e8460 ->
    // Projectile_RaycastProximitySlots @0x4e5340 slot types 0/2/1]
    // [orig: Physics_RaycastEntityPoolsAndUpdate @0x539580]
    ProjectileHit trace_aim(const World &world, const ProjectileTrace &trace) const;
    // Squib uses the general TYPE-1 solid / whole-person sphere query.
    // [orig: Entity_ProcessProjectileTravel @0x448D50 ->
    //  Physics_RaycastAgainstPoolByIndex @0x539530]
    ProjectileHit trace_squib(World &world, EntityHandle source,
            const FixedVec3 &start, const FixedVec3 &end);

    // The sun glare's fine rays and the water glint's eye ray ask the same walker for a
    // boolean over pools 2, 1 and 0, the local player excluded: an IntContext ray of radius 0
    // (a segment under 16 raw is clear), each pool walked with the walker's include arg 1 (a
    // NoTarget row blocks), the local player and what stands on it skipped, a def-type-3 row a
    // bound-sphere hit unless it is on the local player's team within 3 u of `start`, any other
    // row its TYPE-1 solid. On a joiner the pools hold every decoded row, so the walk takes the
    // wire proxies too (env #49): each person proxy as a type-3 row and each mover proxy with no
    // registry twin its posed authored solid, as the projectile trace merges them.
    // True when nothing blocks; the terrain is the caller's.
    // [orig: Render_SkyboxSunGlow @0x5ace65..0x5acee8; Environment_UpdateSunGlare
    //  @0x5ad282..0x5ad305 -> Physics_InitRayContext @0x5385E0 (radius 0),
    //  Physics_RaycastAgainstPoolByIndex @0x539530 (2, 1, 0; include 1), ctx[17] =
    //  g_LocalPlayerEntity @0x5acea7 / @0x5ad2c4]
    bool sun_ray_clear_of_entities(const World &world, const FixedVec3 &start,
            const FixedVec3 &end) const;
    // The nearest entity the segment meets: pool 2's nearest face hit, then pool 1's when it is
    // strictly nearer; `exclude` skipped. Each pool walks its rows in slot order: a row with a
    // type ordinal (+0x1C) and a bound word whose bound box holds the segment's box and whose
    // centre lies within the bound of the line, its faces through
    // collision_raycast_entity_faces. A segment under 16 meets nothing, as does a hit at 0.
    // [orig: Physics_RaycastNearestEntity @0x5394B0 (pool 2 @0x5394ED, the `jge` @0x5394FF, pool
    //  1 @0x53950E, the `jge` @0x53951E); Physics_InitRayContext @0x5385E0;
    //  Physics_RaycastEntityPool @0x4143E0 (the +0x1C and bound gates @0x41445C..0x41446E, the
    //  box @0x414470..0x4144A6, the excludes @0x4144A8..0x4144BA, the line distance
    //  @0x4144BC..0x4145C5, the bone data @0x4145E4, the return @0x414968)]
    EntityHandle raycast_nearest_entity(const World &world, EntityHandle exclude,
            const FixedVec3 &start, const FixedVec3 &end) const;

    ProjectileHit trace_knife_impact(const World &world,
                                     const ProjectileTrace &trace) const;

    // Per-entity blink refresh: test the entity position (one point, radius 0.5u)
    // against nearby buildings' blink volumes; stamp Entity.blink_hits + the
    // indoors flag; accumulate the local player's flags word.
    // [orig: Entity_BuildProximityList @ 0x4b3dc0]
    void refresh_blink(World &world, Entity &ent);
    // The mission-start blink stamp over the static table just built: every
    // pool-1 row, then every pool-2 row that is not a building, takes one
    // refresh_blink before the portal init.
    // [orig: Entity_BuildProximityListsForPools12 @ 0x5240a0 — the pool-1 loop
    //  @ 0x5240b8..0x5240cd, the pool-2 loop with its def-type-5 skip
    //  @ 0x5240e4..0x524102; called from Game_StartMission @ 0x525898, after
    //  Entity_InitAllFromModels built the static table]
    void refresh_mission_start_blink(World &world);

    // Point blink query at an arbitrary position (the camera-side analog of
    // refresh_blink): walk the building prefix with per-axis + euclid broad
    // phase at radius + 0x8000, run the point query (one point, radius 0x8000),
    // return the packed hit set in `accum`. Clears `accum` first.
    // [orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]
    void query_blink_boxes_at_point(World &world, const int32_t pos[3], BlinkAccum &accum);

    // Iris/exposure point query: clear `accum`, then test only building-kind
    // entries in `source`'s fixed proximity-candidate slice. This is a
    // different retail function from query_blink_boxes_at_point: the latter
    // walks the global building prefix for camera/occlusion callers, while
    // Terrain_SectorComputeLighting reuses the local player's
    // entity+0x1BC/+0x1C0 slice for all three marched samples.
    // [orig: Terrain_SectorComputeLighting @ 0x5c7550, caller passes
    // g_LocalPlayerEntity from Environment_ApplyFogAndAmbient @ 0x57e51d]
    void query_candidate_blink_boxes_at_point(World &world, EntityHandle source,
                                              const int32_t pos[3], BlinkAccum &accum);

    // The wire-identity twin of refresh_blink for a decoded source without a
    // registry entity: the retail client runs Entity_BuildProximityList on its
    // own copy of the entity. A def type 1/3 source (`candidate_walk`) tests
    // the building-kind entries of its candidate slice — the source's
    // wire-keyed slice here, the one wire_sun_visibility_blocked_rays walks;
    // no slice, no hit [orig: @ 0x4b3e53..0x4b3e5f -> @ 0x4b3f3e..0x4b3f93].
    // Every other type walks the static building prefix [orig: @ 0x4b3e65..
    // 0x4b3f39]. Clears `accum` first.
    void query_wire_blink_boxes_at_point(World &world, uint16_t wire_handle,
                                         const int32_t pos[3], bool candidate_walk,
                                         BlinkAccum &accum);

    // Projectile face raycast against ONE entity's collision instance (the
    // husk-aware target view). kNoFaceMesh = no instance or the model carries
    // no face mesh, which no walk hits; kMiss = a face mesh exists and the segment misses it
    // (the round flies on); kHit fills `out`. [orig: each pool-walk candidate
    // runs Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]
    enum class FaceRaycast { kNoFaceMesh, kMiss, kHit };
    FaceRaycast raycast_entity_faces(World &world, EntityHandle h, const int32_t start[3],
                                     const int32_t end[3], uint32_t ammo_flags,
                                     RayFaceHit &out);

    // Retail organic/person narrow phase over the entity's posed COBJ spheres.
    // A set section-mask bit removes that bone from collision.
    bool raycast_person_sections(World &world, EntityHandle h, const int32_t start[3],
                                 const int32_t end[3], int32_t extra_radius,
                                 PersonSectionHit &out);

    // Entity-only radiused segment test over `source`'s candidate slice:
    // TRUE = an eligible pool-1 dynamic or pool-2 static type-1 solid clips
    // the segment at `radius` (negative radius reads the planes thinner). The
    // iris/entity-sun callers use allowAllTypes=1, so there is no building-kind
    // gate. [orig: Physics_RaycastFindCollisionEntity @ 0x539a70 ->
    // Physics_RaycastAgainstEntityPool @ 0x538720]
    bool candidate_segment_hits_solid(World &world, EntityHandle source,
                                      const int32_t a[3], const int32_t b[3],
                                      int32_t radius);

    // The three sun-occlusion clip radii, most permissive first — the same
    // segment recast with progressively thinner plane reads; each blocked cast
    // steps the light quality down one. Shared by the camera iris march and the
    // per-entity sun-visibility factor. [orig: the -0x2000/-0x5000/-0x8000
    // pushes @ 0x5c687c/0x5c68a7/0x5c68c8 (Entity_ComputeSunVisibility) and
    // @ 0x5c7767/0x5c7792/0x5c77ac (the iris march).]
    static constexpr int32_t kSunOcclusionClipRadii[3] = {-0x2000, -0x5000,
                                                          -0x8000};

    // Per-entity sun-visibility blocked-ray count for the drawn-entity lighting
    // factor: one segment from the entity position + raw collision-bbox
    // midpoint, 200 u along the active light direction, recast at the three
    // clip radii — against every eligible entry in the ENTITY'S OWN candidate
    // slice (the +0x1BC arena block the ray walker iterates; only solids whose
    // inflated sphere overlaps the entity's bubble can block its sun). An
    // entity with no slice — statics, ineligible pool-1 rows, or the 16
    // sliceless mission-start ticks — returns 0, matching retail's +0x1C0 == 0 skip
    // (quality stays 4, factor 1.0). The caller maps the count through
    // renderer::sun_visibility_factor ((4 - blocked) * 0.25).
    // [orig: Entity_ComputeSunVisibility @ 0x5c6800 — the +0x1C0 gate
    // @ 0x5c6808, origin = position + (entity+0x1FC..+0x204)
    // @ 0x5c681f..0x5c6847, end = origin + 200*lightdir @ 0x5c6850..0x5c6876,
    // one decrement per blocked cast @ 0x5c689e..0x5c68ea;
    // Physics_RaycastFindCollisionEntity @ 0x539a70 walks entity_a's
    // +0x1BC/+0x1C0 slice]
    int sun_visibility_blocked_rays(World &world, const Entity &e,
                                    const int32_t sun_step_q16[3]);

    // Wire-identity twin of the entity query above. Membership comes from the
    // decoded source's own separately keyed 17-tick slice; blockers remain the
    // locally hosted pool-1/pool-2 collision instances. Position and bbox are
    // live decoded Q16 values, while membership is intentionally stale for up
    // to 16 ticks like retail's entity+0x1BC/+0x1C0 pointer/count.
    int wire_sun_visibility_blocked_rays(
            World &world, uint16_t wire_handle, const FixedVec3 &position_q16,
            const FixedVec3 &bbox_center_q16,
            const int32_t sun_step_q16[3]);

    // Clip an arbitrary segment in place to the nearest terrain or eligible
    // solid in `source`'s candidate slice. The returned handle identifies the
    // nearest entity hit; invalid means terrain-only or no hit. Terrain is
    // skipped for an indoors source. This is the single hosted form of
    // Entity_RaycastCollision used by both the ground-column wrappers and the
    // iris camera ray. [orig: Entity_RaycastCollision @ 0x413760]
    EntityHandle clip_segment_to_nearest_collision(World &world, EntityHandle source,
                                                   const int32_t start[3],
                                                   int32_t inout_end[3]);

    // The nearest solid a segment crosses over the published proximity tables
    // (the statics, then the dynamics), not a candidate slice: the terrain clips
    // the end first (an indoors source skips it), each table row passes the ray
    // box and the line distance at its table position and radius, a row with
    // Flags 0x2000001 or standing on the source (three levels) is skipped, and
    // the collision model clips the ray; the last clip is the nearest. Returns
    // the hit entity, invalid on a miss. [orig: Entity_FindNearestByRay
    // @0x413AF0 (the terrain clip @0x413B12..0x413B1E, the static walk
    // @0x413C1F..0x413E0D, the dynamic walk @0x413E2C..0x41403A, the end's
    // restore @0x413E40..0x413E4B)]
    EntityHandle find_nearest_by_ray(World &world, EntityHandle source, const int32_t start[3],
                                     const int32_t end[3]);

    // Ground-column probe through terrain + the entity's candidate models.
    // Builds the ray {x+dx, y+dy, z+z_up} down z_drop, clamps to the terrain
    // column, clips against candidate solids; returns the resolved ground Z and
    // (optionally) the hit entity. [orig: Entity_RaycastGroundHeight @ 0x4142c0 /
    // Entity_RaycastGroundHeightAndObject @ 0x414320 -> Entity_RaycastCollision
    // @ 0x413760]
    int32_t raycast_ground(World &world, EntityHandle source, const int32_t pos[3],
                           int32_t dx, int32_t dy, int32_t z_up, int32_t z_drop,
                           EntityHandle *out_hit_entity);

    // Segment LOS query, TRUE = CLEAR of terrain + sector solids — the AI mutual-LOS
    // seam. [orig: Physics_RaycastTerrainAndSectors @ 0x539910: terrain leg via
    // Terrain_RaycastHeightmapHiRes @ 0x60c760 (skipped when BOTH endpoint entities
    // carry Flags & 0x800000 INDOORS — the heightmap has no interiors), then the
    // sector walk (Physics_RaycastAgainstEntityPool @ 0x538720) over pool 2 statics, then
    // pool 1 dynamics; LOS callers pass ray radius 0.] The walk skips the endpoint
    // entities A/B, their parent slots (los_walker_parent; the AI LOS passes both,
    // other callers may leave them null) and any candidate standing on A or B
    // (+0x28) [orig: @0x53882F..0x538877]. The itemDef type-3 person sphere-block
    // (same-team within 3.0 u exempt) has no resident: organics are pool 0,
    // unwalked, like retail.
    bool raycast_clear(World &world, const int32_t a[3], const int32_t b[3],
                       EntityHandle exclude_a, EntityHandle exclude_b,
                       EntityHandle parent_a = EntityHandle{},
                       EntityHandle parent_b = EntityHandle{});
    // Terrain plus the querying entity's building-candidate slice. A null
    // second entity permits a buried endpoint; height_offset raises/lowers the
    // terrain ray and shrinks/inflates the solid clip. Audio and blasts share
    // this distinct retail query [orig: Entity_CheckLineOfSightTerrainAndEntities
    // @ 0x53B130; Physics_CheckTerrainLineOfSight @ 0x53B080].
	// `query_parent_cleared` models a caller that NULLS the query entity's
	// parentEntity (+0x16C) around the call, so the walker's parent slot falls
	// to mountedChild and the occupant's own mount hull can block the ray
	// [orig: Projectile_ProcessExplosionQueue pool-0 leg @0x4eb142..0x4eb16c].
	bool entity_los_clear(World &world, EntityHandle query, EntityHandle endpoint,
			const int32_t start[3], const int32_t end[3], int32_t height_offset,
			bool all_types = false, bool query_parent_cleared = false);
	// A decoded remote person as the endpoint ENTITY, for a client that keeps
	// no registry entity for it: the two endpoint fields the query reads, its
	// Flags indoors bit and its parent slot. Retail's endpoint is the client's
	// own pool-0 row, so the no-entity leg's buried-endpoint pass never runs.
	// [orig: Physics_CheckTerrainLineOfSight `entityB+24h & 800000h` @0x53B0A0;
	//  Physics_RaycastFindCollisionEntity entity_b[154] / [91] @0x539AE8..0x539B10]
	struct LosWireEndpoint {
		bool indoors = false;
		EntityHandle parent;
	};
	bool wire_person_los_clear(World &world, EntityHandle query, const LosWireEndpoint &endpoint,
			const int32_t start[3], const int32_t end[3], int32_t height_offset,
			bool all_types = false);
	// raycast_clear with a decoded remote person as entity B through the same two fields: its
	// Flags indoors bit (the terrain leg's pair) and its parent slot (excluded from the walk).
	// [orig: Physics_RaycastTerrainAndSectors @0x539910 - entityB's Flags @0x539949, its
	//  +0x16C @0x539A08..0x539A12]
	bool raycast_clear_to_person(World &world, const int32_t a[3], const int32_t b[3],
			EntityHandle exclude_a, EntityHandle parent_a, const LosWireEndpoint &person);
	// Same exact query with per-target section matrices retained for a caller-
    // declared stable world phase. The server resets the cache after gameplay
    // movement and again before snapshot fan-out; every recipient LOS ray can
    // then reuse retail's entity-resident matrix equivalent without observing
    // a pre-movement pose.
    bool raycast_clear_cached(World &world, const int32_t a[3], const int32_t b[3],
                              EntityHandle exclude_a, EntityHandle exclude_b,
                              EntityHandle parent_a = EntityHandle{},
                              EntityHandle parent_b = EntityHandle{});
    // Sector candidates the most recent raycast_clear / raycast_clear_cached
    // visited (the broad-phase's exactness, pinned by the collision ctest).
    uint64_t last_los_sector_candidates() const { return last_los_sector_candidates_; }
    // Publish an exact broad-phase over the final live target bounds for a
    // caller-declared stable world phase. The replication fan prepares once
    // after movement/destruction, then every recipient ray queries only cells
    // intersecting its segment while retaining the original exact solid clip.
    // Its phases lap onto the SIM_REPLICATION_QUERY_* rows of world.profile.
    void prepare_cached_raycast_queries(World &world);
    void reset_query_view_cache();

    // Sound-occlusion distance inflation [orig: Sound_ApplyOcclusionDistance
    // @ 0x529970]: base = min(d/8, 10u); two LOS rays listener -> source
    // (source z lifted +0x2000 for both; ray 2's segment raised 0.5u); a
    // blocked ray 1 compounds base = 2*base + 5u; the single final add is
    // base (ray 2 clear) or 2*base + 5u (ray 2 blocked). LOS per ray =
    // terrain leg [orig: Physics_CheckTerrainLineOfSight @ 0x53b080 —
    // skipped as clear when both entities are indoors; either-entity-null
    // paths precheck both endpoints above the bilinear surface, an
    // under-surface endpoint reading as clear] then entity leg [orig:
    // Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130 ->
    // Physics_RaycastFindCollisionEntity @ 0x539a70 with allowAllTypes = 0 —
    // only building-kind candidates from the LISTENER's slice block, via the
    // type-1 solid clip]. `source` may be invalid when the host has no emitter
    // identity; identified ambient/fire sources preserve exclusion and the
    // both-indoors terrain bypass. listener_pos is the AUDIO listener [orig:
    // listener_pos @ 0x24D6630], not the entity position.
    // Returns the inflated effective distance (16.16). Ray 2's -0x8000
    // height offset doubles as the entity-leg clip radius (the witnessed
    // arg-slot reuse): its planes read 0.5u thinner, which is what lets the
    // second ray clear walls the first grazes — ported; the per-plane
    // flag-byte branch (flagged planes clamp the radius at 0) is the D-SND-9
    // residue (docs/audio/lwf-dbf-sound-re.md).
    int32_t sound_occlusion_inflate(World &world, EntityHandle listener,
                                    EntityHandle source,
                                    const int32_t listener_pos[3],
                                    const int32_t source_pos[3],
                                    int32_t distance_q16);

    // The movement resolver: candidate contact forces + damage/flag dispatch +
    // repulsion + the ground-settle tail. Returns the foot clearance (feet Z -
    // resolved ground Z): <= 0 grounded (caller lifts by the return), > 0xF000
    // airborne. [orig: movement collision resolver @ 0x4b2bd0]
    // capsule_bottom/top are the anim frame's 16.16 capsule extents (out[3]/out[4]).
    struct GameplayContact {
        EntityHandle source;
        EntityHandle target;
    };

    // Drain the exact type-10 Change Team Box touches produced by this tick's
    // authority movement resolves. The collision module owns shape/cadence;
    // gameplay owns the capture request transaction.
    // [orig: contact flag 0x200 @0x4AEB7B; resolver dispatch/callback
    // @0x4B31DD..0x4B3238]
    std::vector<GameplayContact> take_change_team_contacts();

    // Drain successful first-pass MoveCB/non-Powerup contacts. Retail routes
    // these through Entity_ProcessWaypointInteraction instead of treating the
    // target as solid geometry; gameplay interprets the contacted item ID.
    // [orig: resolver attrib branches/call @0x4B2F90..0x4B2FF5]
    std::vector<GameplayContact> take_movement_callback_contacts();

    // Drain successful first-pass Powerup contacts of a player-class source.
    // Retail invokes the target's pickup callback inline, on every peer whose
    // resolver ran the body, and treats the target as no solid geometry;
    // world/powerup.h runs the callback in this order.
    // [orig: the attrib&2 branch @0x4B2FB8, the source Flags&0x100 gate
    //  @0x4B2FBD, Entity_InvokeCollisionCallback @0x4B2FCC]
    std::vector<GameplayContact> take_powerup_contacts();

    // The same Powerup contacts a wire-replica resolve (resolve_replica) made:
    // the source is the decoded row's wire handle, which names no registry
    // entity, so the embedder runs the pickup for its transient copy of the
    // body. [orig: the same branch; a client's resolver moves its remote
    // bodies through the org2 tail call @0x4B7CF4]
    struct ReplicaPowerupContact {
        uint16_t wire_handle = 0xFFFF;
        EntityHandle target;
    };
    std::vector<ReplicaPowerupContact> take_replica_powerup_contacts();

    struct ResolveState {
        // The savedLivePose stand-in for a source no mover stamped (updated per
        // resolve): a stamped registry source reads its own Entity::saved_live_pos,
        // a decoded row the stamp resolve_replica stages (resolve_entity).
        int32_t prev_pos[3] = {};
        bool prev_valid = false;
        uint8_t skip_counter = 0;   // [orig: entity +0x373 collideIdleSkipCount, the idle throttle]
    };
    // is_player_class = the entity's wire Player class bit (Flags & 0x100):
    // retail keys EVERY physics leg on it — the skip-path gravity undo
    // (@ 0x4b2cd9), the candidate mask (@ 0x4b2f7c / @ 0x4b35af), the ladder
    // entry/chase gates (@ 0x4b3271 / @ 0x4b32a5 / @ 0x4b33aa) and the exit
    // push — for local and remote bodies alike; only the local side-writes
    // (g_LocalPlayerLookYaw @ 0x4b33ca, the blink mirror @ 0x4b34ce, the pitch
    // restore @ 0x4b3cdc / @ 0x4b3cfe) add `entity == g_LocalPlayerEntity`,
    // and those ride LadderResolveIO::is_local_player.
    // anim_state_flags = the state's g_AnimStateFlagsTable word (bit 0 forces a
    // full update; the id itself picks the repulsion-exempt states).
    // out_ground (optional) receives the ground probe's hit entity — the same
    // value the resolver stores into a registered source's groundEntity.
    // ladder_io (optional) wires the D-COL-5 climb legs: the CL entry gate +
    // anchor snap, the per-tick alignment chase, and the exit push / pitch
    // restore. Without it only the raw latch/bookkeeping runs (replica rows,
    // harness callers).
    // eye_offset (optional, xyz 16.16) is the entity's CameraOffset: the second
    // capsule test point is pos + CameraOffset [orig: @0x4b2ee0-0x4b2ef8]. When
    // absent the resolver derives the org1 producer's value from the capsule,
    // max(top - bottom, 0x9000) with lean at rest [orig: Entity_UpdateInfantryAI
    // @0x4b9910, kong 155492-155521] -- harness rows and the replica seam.
    int32_t resolve_entity(World &world, EntityHandle source, ResolveState &state,
                           int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                           int32_t capsule_bottom, int32_t capsule_top,
                           int32_t heading, int32_t body_pitch, bool is_player_class,
                           bool is_authority, uint32_t tick, int32_t anim_state_id,
                           uint32_t anim_state_flags, int16_t &health,
                           EntityHandle *out_ground = nullptr,
                           const LadderResolveIO *ladder_io = nullptr,
                           const int32_t *eye_offset = nullptr,
                           devtools::TickProfile *profile = nullptr);

    // The REPLICA seam (net-re §5.38e, D-NET-196): the same resolver for a
    // decoded remote row that has NO world entity — retail runs remote
    // organics through the ordinary org movers whose shared tail calls the
    // resolver ungated [orig: Entity_UpdateInfantryPlayerBody call @0x4B7CF4;
    // Entity_UpdateInfantryAI @0x4BF7FA]. The candidate slice is built AD HOC
    // at the query position with the pool-0 rule (the exact caller-supplied
    // authored bound radius + 4.0 u pad), person repulsion runs the
    // world persons_ PLUS the caller-passed replica peer spheres through the
    // SAME witnessed loop (a ClientState peer's "live re-read" is its staged
    // snapshot — the row has no registry entity), and the Entity-side writes
    // (flag latches, blink, groundEntity) become the out_ground return.
    // is_authority is pinned false: a replica resolve never runs damage legs.
    struct ReplicaPeer {
        int32_t x = 0, y = 0, z = 0;
        int32_t radius = 0;
        uint16_t handle = 0xFFFF; // wire handle, for self-exclusion only
    };
    // entity_flags (optional) is the row's persistent retail-Flags mirror — the
    // caller-owned stand-in for the entity Flags word the resolver's latch
    // sites read and write on a registry row. When provided, the resolve runs
    // the SAME flag channel retail runs on a remote entity: the InAir bit
    // feeds the idle-skip full-update discriminant [orig: @ 0x4b2ca6], the
    // resolve-start clear drops Indoors/LadderContact/zone bits
    // [orig: @ 0x4b2d54 block], the CL touch latches LadderContact
    // [orig: @ 0x4b3291], the blink accum latches Indoors [orig: @ 0x4b34c2],
    // the dead/hidden and parachute bits gate/widen repulsion
    // [orig: @ 0x4b3aac/0x4b3aba], and the ground probe's terrain clamp is
    // skipped while Indoors [orig: the Flags & 0x800000 gate @ 0x413785].
    // saved_live_pos (optional) is the row's savedLivePose, its mover's entry stamp,
    // which the resolver's skip test, plane gating and victim delta read where they
    // read a registry row's own words; without it the last resolve's end pose stands
    // in (an unstamped harness row). eye_offset (optional) is the row's CameraOffset
    // as its mover stored it; without it the resolver derives the org1 eye.
    // mounted_latch is the row's mounted latch, which a decoded row's caller answers
    // (a row attached to a seat carries the attach's Flags 0x40): it suppresses every
    // model push force, as a registry row's Flags 0x40 or live modeled parent does.
    // [orig: Entity_UpdateInfantryPlayerBody @0x4B4190..0x4B419C, the call @0x4B7CF4;
    //  Entity_UpdateInfantryAI @0x4B9A53..0x4B9A6E; Entity_MovementCollisionResolver
    //  @0x4B2BD0 - the eye @0x4B2EE2..0x4B2EFB, the latch @0x4B2BE0..0x4B2C02 /
    //  @0x4B2D10..0x4B2D3F]
    int32_t resolve_replica(World &world, ResolveState &state, int32_t pos[3],
                            int32_t vel_xy[2], int32_t &vel_z,
                            int32_t capsule_bottom, int32_t capsule_top,
                            int32_t source_bound_radius_q16,
                            bool is_player_class, uint32_t tick, int32_t anim_state_id,
                            uint32_t anim_state_flags, const ReplicaPeer *peers,
                            int32_t peer_count, uint16_t exclude_handle,
                            uint32_t *entity_flags, EntityHandle *out_ground,
                            const int32_t *saved_live_pos = nullptr,
                            const int32_t *eye_offset = nullptr, int32_t item_id = 0,
                            bool mounted_latch = false);
    // A wire replica row's ground column (raycast_ground) through the same
    // candidate slice its resolve reads (resolve_replica's staging), the row's
    // Flags mirror gating the terrain clamp. The player body's slope conform
    // probes a remote body this way [orig: Entity_UpdateInfantryPlayerBody
    // @0x4B40E0 - the probes @0x4B6E41 / @0x4B6E5D / @0x4B6E98 / @0x4B6EB3 ->
    // Entity_RaycastGroundHeight @0x4142C0, over the body's proximity slice].
    int32_t raycast_ground_replica(World &world, const int32_t pos[3],
                                   int32_t source_bound_radius_q16, uint16_t row_handle,
                                   uint32_t entity_flags, int32_t dx, int32_t dy,
                                   int32_t z_up, int32_t z_drop);

	// Fold model contacts into the terrain forces, one query per authored
	// wheel/hull probe. Returns severity 0..3 and the last severe hit entity.
	// The hull position is the already-integrated pose; savedLivePose supplies
	// the previous-plane gate. Mounted children are excluded through three
	// carrier links. [orig: Entity_CheckCollisionState @0x462A30, entity half
	// @0x462DFB..0x4632CC; Entity_ComputeCollisionForces @0x462150 boat twin]
	int32_t resolve_vehicle_probes(World &world, Entity &source, const int32_t hull_pos[3],
			const int32_t (*probes)[3], const int32_t *radii, int count, int32_t soft, int32_t hard,
			VehicleProbeForce *forces, EntityHandle &hit_entity);

	// World-level blink state for the local player.
	// [orig: g_LocalPlayerBlinkFlags @ 0x24C1934]
	uint32_t local_player_blink_flags = 0;
    EntityHandle local_player;

    // The last CL latch's alignment frame. Retail keeps these as globals that
    // persist across resolves — every consumer (the motors' dismount pushes,
    // the bottom-exit compare, org1's 33/35 select) is Flags-0x100000-gated, so
    // one world-level frame is the faithful carrier.
    // [orig: g_LadderContact{Pitch,Yaw,X,Y,Z} @ 0xB5AB70..80]
    LadderContact last_ladder_frame;
    // Whether the last resolve APPLIED a nonzero net push — org1's on-ladder
    // facing press runs only while this is clear. (NOT the pass-2 contact
    // flag: retail re-zeroes that stack slot after both passes and re-sets it
    // only when the accumulated total force is nonzero, so the global latches
    // "the resolver moved the entity this resolve".)
    // [orig: dword_B57C8C — slot re-zero @ 0x4b3734, set on nonzero total
    //  @ 0x4b3767, stored @ 0x4b3a5c-0x4b3a62; read @ 0x4bfa3e-0x4bfa45]
    bool resolver_applied_push = false;

    // The witnessed on-ladder person probe: 1.25u ahead of the climber, a live
    // pool-0 person within 1.125u on both axes whose Z band overlaps holds the
    // climb (org1 stamps climb_idle 32 and skips the press/select).
    // [orig: the g_PoolList[0] scan @ 0x4bf9b7-0x4bfa2b]
    bool ladder_person_ahead(World &world, EntityHandle self, int32_t probe_x,
                             int32_t probe_y, int32_t self_z, int32_t self_bound);

    // Read-only capture of the local player's last FULL resolve (skip-throttled
    // ticks keep the previous capture): the capsule test points/radii the
    // resolver actually queried, the anim-frame capsule extents, and the
    // returned foot clearance. Debug-view seam only — never consumed by the
    // resolver itself. All values 16.16 mission space.
    struct LocalResolveDebug {
        bool valid = false;
        int32_t pos[3] = {};        // resolved position (post push-out)
        int32_t points[3][3] = {};  // the 3 capsule test points (pre pass-shift)
        int32_t radii[3] = {};
        int32_t capsule_bottom = 0; // anim-frame extents (resolve_entity inputs)
        int32_t capsule_top = 0;
        int32_t foot_clearance = 0; // resolve_entity's return (feet Z - ground Z)
    };
    LocalResolveDebug local_resolve_debug;

    // Embedder-wired terrain (shared with the AI system's field).
    const terrain::TerrainHeightField *terrain = nullptr;

    // --- introspection for tests/embedders ---
    int32_t static_count() const { return static_count_; }
    int32_t static_building_count() const { return static_building_count_; }
    int32_t candidate_count(EntityHandle h) const;
    // A decoded row's wire slice size (0 for none).
    int32_t wire_candidate_count(uint16_t wire_handle) const {
        int32_t count = 0;
        (void)wire_candidate_slice(wire_handle, count);
        return count;
    }
    // The entity's proximity slice [orig: entity+444/448 — the g_ProxCandidateArena
    // window Entity_BuildProximityListsFromPools fills @ 0x4b8eb0]. Pointer into
    // the arena, valid until the next 17th-tick rebuild; null when the entity has
    // no slice (retail's BSS-zero start: the first 16 ticks scan nothing; a row
    // spawned into the slot since the slice was built, D-COL-19).
    const EntityHandle *candidate_slice(EntityHandle h, int32_t &count_out) const;
    // A clone's slice words: the clone copies its template's record, the slice's
    // start and count with it, so it walks the template's slice until a build gives
    // it its own (none when the template has none).
    // [orig: Entity_CloneFromTemplateByType @0x4398A0 - the 0x2B4-byte copies
    //  @0x4398DC / @0x43997D; Entity_InitArcMovement - the 0x310-byte copy @0x449BCC]
    void inherit_candidate_slice(EntityHandle source, const Entity &clone);
    // Every registry slice dropped: a restart's reload re-spawns every row through
    // the spawn's memset, so a row the next build skips has none.
    // [orig: Mission_LoadBMSFile -> Entity_SpawnFromBMSRecord - the 0x2B4-byte memset
    //  @0x40EA27]
    void forget_candidate_slices() { candidates_.clear(); }
    // Whether the pool-1 dynamic table holds a row for `h`, and that row's radius
    // [orig: g_DynProxRadius, the copy @0x4B9389].
    bool dynamic_table_radius_q16(EntityHandle h, int32_t &radius_out) const;
    // True once the per-tick pool tables have been built — the discriminator
    // between "no candidates near" and "this world never ran the table build"
    // (headless callers), which keep whole-registry fallbacks.
    bool tick_tables_ready() const { return tick_tables_built_; }
    // Attach scans switch from their compatibility registry walk only after a
    // real candidate-slice epoch exists. Initial table snapshots deliberately
    // precede the retail 17-tick slice cadence, so treating those as an empty
    // authoritative slice would make nearby seats temporarily disappear.
    bool attach_candidate_slices_authoritative() const {
        return candidate_slices_built_;
    }

    // Static prox-table slot view (quantized u16 coords/radius like the retail
    // tables) — the render-occlusion engine walks the building prefix through
    // this. [orig: g_StaticProx{X,Y,Z,Radius,Entity} @ 0xB55558/0xB54BF8/
    // 0xB54298/0xB55EB8/0xB52FD8]
    struct StaticSlotView {
        uint16_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    StaticSlotView static_slot(int32_t i) const;
    // The collision model attached to this exact live registry identity
    // (nullptr for an absent or recycled packed slot).
    const CollisionModel *model_for(const World &world, EntityHandle h) const;
    // The husk-stage collision model attached to that identity (nullptr when
    // the def authors no husk or the slot is absent/recycled). Retail keeps
    // the husk model pointer at entity+0x34 beside the graphic at +0x30 and
    // lets each consumer pick by Flags & 4 [orig: Entity_CalcSlopeForces
    // @ 0x4b0c10..0x4b0c1b] — this is the husk half of that pair.
    const CollisionModel *husk_model_for(const World &world, EntityHandle h) const;

    // Retail's per-section collision-triangle debris sampler. The transform
    // callback is resolved through the same target view as projectile traces;
    // an absent/invalid model produces no synthetic fallback. The launch origin
    // is the entity's savedLivePose words (D-NET-507), all zero for the radial
    // fallback. [orig: Entity_SpawnSectionDebris @0x43f580 - the +0x80 / +0x84 /
    // +0x88 reads and their zero test @0x43F6BD..0x43F6D8]
    std::vector<SectionDebrisSample> sample_section_debris(
            const World &world, EntityHandle h,
            const int32_t blast_center[3]) const;

    // Read-only world-space geometry snapshot for an embedder collision debug view.
    // Each instance's volumes are transformed through the SAME target_view /
    // collision_matrix_from_placement path every query uses, so what the host
    // draws is exactly what the resolver tests. Corner order: index bit 0 = max
    // x, bit 1 = max y, bit 2 = max z (mission space, 16.16). `range` > 0 keeps
    // only instances whose entity position is within it (per-axis box) of
    // `anchor`; `max_instances` caps the sweep either way.
    struct DebugVolume {
        int32_t type = 0;
        uint32_t flags = 0;
        int32_t min[3] = {}, max[3] = {}; // section-local AABB
        int32_t corners[8][3] = {};       // world-space transformed corners
    };
    struct DebugInstance {
        EntityHandle handle;
        int32_t pos[3] = {};
        int32_t heading_bam = 0; // the exact heading the world matrix was built from
        std::vector<DebugVolume> volumes;
    };
    std::vector<DebugInstance> debug_instances(World &world, const int32_t anchor[3],
                                               int32_t range, int32_t max_instances) const;

    // The round hit-detection reality for a host hitbox view: per entity, the
    // CFAC bullet-mesh triangles in WORLD space, transformed through the SAME
    // husk-aware target_view + full-euler placement path the projectile
    // raycast walks (what is drawn IS what rounds test), each face carrying
    // its material byte + flags; plus the broad-phase bound sphere and
    // whether the entity has a face mesh at all (none = the bound-sphere
    // stand-in decides hits, D-ITEM-1). `max_faces` is a total triangle
    // budget; face_total still reports each entity's authored count so a
    // truncated draw is visible as such.
    struct DebugHitboxFace {
        int32_t v[3][3] = {}; // world-space triangle corners (mission 16.16)
        uint8_t material = 0;
        uint32_t flags = 0;
    };
    struct DebugHitboxEntity {
        EntityHandle handle;
        int32_t pos[3] = {};
        int32_t bound_radius = 0; // 16.16 (the round broad-phase sphere)
        bool husk = false;        // the shell is the husk-swapped model
        bool has_faces = false;   // false = sphere stand-in resolves hits
        int32_t face_total = 0;   // authored faces (before the budget cap)
        std::vector<DebugHitboxFace> faces;
    };
    std::vector<DebugHitboxEntity> debug_hitboxes(World &world, const int32_t anchor[3],
                                                  int32_t range, int32_t max_entities,
                                                  int32_t max_faces) const;

    // The organic/person narrow-phase reality for F3: every authored COBJ
    // sphere after the SAME posed target_view matrix used by
    // raycast_person_sections. Radius is the retail effective projectile-zero
    // radius (+0xCCC padding and per-bone scale/cap), not the authored radius.
    struct DebugPersonSection {
        EntityHandle handle;
        int32_t section = -1;
        int32_t center[3] = {};
        int32_t radius = 0;
        int32_t authored_radius = 0;
        bool masked = false;
    };
    std::vector<DebugPersonSection> debug_person_sections(
            World &world, const int32_t anchor[3], int32_t range,
            int32_t max_entities);

private:
    void build_tables(World &world, bool advance_candidate_slices);
    // Drops the bindings (and candidate slices) whose registry lifetime ended.
    void prune_stale_bindings(World &world);
    // The statics table over the pool-2 rows [orig: Entity_BuildProximityLists_Pool2
    //  @0x4B9430].
    void build_statics(World &world);
    void build_candidate_slices(World &world);
    void purge_stale_candidate_slices(const World &world);
    // entity_los_clear over an endpoint described by its read fields.
    bool entity_los_clear_impl(World &world, EntityHandle listener, EntityHandle source,
            bool endpoint_present, bool endpoint_indoors, EntityHandle parent_b,
            const int32_t start_in[3], const int32_t end_in[3], int32_t height_offset,
            bool all_types, bool query_parent_cleared);
    // Contact-flag side effects shared by both resolver passes (DH/DM/DL damage +
    // the CA/CM entity flags); contact_legs is the authority's admission of the damage
    // legs (no Flags 0x4000000, the rotor gate). [orig: the dispatch @ 0x4b30b7-0x4b351e]
    void apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health, bool contact_legs);
    // The struck body's rotor-strike sound (vehicle_collision_damage.h).
    void play_rotor_strike_sound(World &world, EntityHandle source, const Entity *ent,
                                 const int32_t pos[3]);
    void record_change_team_contact(EntityHandle source, EntityHandle trigger);
    void record_movement_callback_contact(EntityHandle source, EntityHandle target);
    void record_powerup_contact(EntityHandle source, EntityHandle target);

    std::vector<GameplayContact> change_team_contacts_;
    std::vector<GameplayContact> movement_callback_contacts_;
    std::vector<GameplayContact> powerup_contacts_;
    std::vector<ReplicaPowerupContact> replica_powerup_contacts_;

    struct Instance {
        int32_t model_id = -1;
        // The husk-stage collision model — substituted for every query once the
        // entity carries the destroyed flag (Flags & 4): rays, contacts, and
        // ground probes collide with the wreck, not the intact model. -1 = the
        // def authors no husk (the intact model keeps serving, the witnessed
        // fallback). [orig: the +52 huskModel substitution in the pool walk
        // Physics_RaycastAgainstEntityPool @ 0x538720 and the ray/contact picks
        // @ 0x413086 / @ 0x4ae233; D-AI-7 residual closed §24]
        int32_t husk_model_id = -1;
        // The final husk's model (entity+0x38), which the swap stage's promotion
        // makes the husk model; -1 = none. [orig: Render_SectorEntity @0x5C4241 /
        //  @0x5C42C7]
        int32_t final_husk_model_id = -1;
        // The husk model the entity holds (+0x34): the final husk once promoted.
        // A respawn re-arms the def's pair, so the entity's flag picks, never a
        // store here. [orig: Entity_RespawnVehicle @0x45FF40 (@0x460048..0x460059)]
        int32_t held_husk(const Entity &e) const {
            return e.husk_promoted ? final_husk_model_id : husk_model_id;
        }
        // Entity::registry_spawn_id at assignment. Zero preserves the legacy
        // handle-only behavior for portable callers/tests that do not stamp it.
        uint64_t registry_spawn_id = 0;
        std::vector<CollisionMatrix> section_matrices;
    };
    const Instance *live_instance(const World &world, EntityHandle h) const;
    struct StaticSlot { // [orig: g_StaticProx* u16 tables + entity ptr array]
        uint16_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct DynSlot {    // [orig: g_DynProx* dword tables]
        int32_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
        // The row's +0x114 noage bit (Entity::ammo_word_noage), which every
        // slice build reads through the row's entity pointer; the word has no
        // writer after the device's clone, so the table's copy is that word.
        // [orig: Entity_BuildProximityListsFromPools @0x4B8EB0 - @0x4B8FCA /
        //  @0x4B920A]
        bool noage = false;
    };
    struct PersonSlot { // [orig: g_PersonProx* dword tables]
        int32_t x = 0, y = 0, z = 0, radius = 0;
        // The row's raw boundRadius word, the table's radius the repulsion
        // reads; `radius` is the projectile slot's, widened for a posed body.
        // [orig: g_PersonProxRadius = entity+0 @ 0x4b93eb]
        int32_t bound = 0;
        EntityHandle h;
    };
    struct CandidateSlice {
        int32_t start = 0;
        int32_t count = 0;
        // The source position the slice was built at (wire slices only): a
        // row that has moved past its pad since then needs a fresh build.
        int32_t built_pos[3] = {0, 0, 0};
        // The lifetime the slice was built for (a registry slot's spawn id, a
        // decoded row's spawn revision): the slot's words outlive a build the
        // gates skip, never a respawn into the slot.
        uint64_t lifetime = 0;
    };
    // The slot's slice as its current lifetime reads it (null for none). The
    // replica resolve's staged slice carries no lifetime and reads as staged.
    const CandidateSlice *find_slice(EntityHandle h) const;
    struct StableLosCandidate {
        EntityHandle h;
        int32_t pos[3] = {};
        int32_t radius = 0;
    };
    struct StableLosCell {
        std::vector<uint32_t> candidates;
        uint32_t epoch = 0;
    };
    struct StableLosCellSpan {
        int32_t min_x = 0;
        int32_t max_x = 0;
        int32_t min_y = 0;
        int32_t max_y = 0;
        uint32_t candidate = 0;
    };

    // husk_swap false keeps the intact model on a husked row: the ground drop's pool walk reads
    // entity+0x30 whatever Flags 4 holds [orig: Physics_RaycastEntityPool @0x4143E0 - the
    // `mov esi, [ecx+30h]` @0x4145CF, its +0xA8 callback @0x4145D2..0x4145E2].
    const CollisionTargetView *target_view(const World &world, EntityHandle h,
                                           CollisionTargetView &scratch,
                                           std::vector<CollisionMatrix> &mat_scratch,
                                           bool husk_swap = true) const;
    // One candidate entity vs one radiused segment: the cheap-metadata broad
    // phase, then live section matrices for survivors (shared by sound and the
    // slice-scoped iris/entity sun casts).
    bool entity_blocks_segment(World &world, EntityHandle candidate,
                               const CollisionRay &ray, int32_t radius,
                               int32_t broad_r);
    // One pool row against the walker's ray [orig: Physics_RaycastAgainstEntityPool @0x538720]:
    // a row with no def or bound radius, `excluded` itself, a flags-1 row and a row standing on
    // `excluded` pass; then the bound sphere against the ray's box and line (the walker's ray
    // radius is 0); then a def-type-3 row is a hit unless it is on `excluded_row`'s team within
    // 3 u of the ray's start (@0x5389B1..0x5389FB), any other row its TYPE-1 solid's clip.
    bool pool_row_hits_ray(const World &world, EntityHandle handle, const Entity &e,
                           const CollisionRay &ray, EntityHandle excluded,
                           const Entity *excluded_row) const;
    bool candidate_slice_segment_hits_solid(
            World &world, const EntityHandle *slice, int32_t slice_count,
            EntityHandle source_registry_twin, const int32_t a[3],
            const int32_t b[3], int32_t radius);
    int candidate_slice_sun_blocked_rays(
            World &world, const EntityHandle *slice, int32_t slice_count,
            EntityHandle source_registry_twin, const int32_t a[3],
            const int32_t b[3]);
    const EntityHandle *wire_candidate_slice(
            uint16_t wire_handle, int32_t &count_out) const;
    void invalidate_trace_view(EntityHandle h);
    void invalidate_trace_views();
    void invalidate_stable_los_index();
    const std::vector<uint32_t> &stable_los_candidates_for_ray(
            const CollisionRay &ray);
    // Per-logic-tick cache of projectile target views. Sustained automatic
    // fire re-traced the same structures per ROUND per tick, and every
    // broad-phase survivor rebuilt its per-section matrix vector (heap
    // allocation included) — ~15 ms/tick with ~90 rounds in flight at a
    // firing range. Retail keeps section matrices RESIDENT on the entity
    // [orig: the entity matrix array consumed by
    // Physics_RaycastAgainstBoneCollision @ 0x4e4cb0]; this cache is the
    // host-side equivalent scoped to one tick. A cache hit revalidates the
    // husk bit because retail's model pick runs per query [orig: Flags & 4
    // pick @ 0x413086] and a round earlier in the SAME tick can husk the
    // target.
    struct TraceViewCacheEntry {
        CollisionTargetView view;
        std::vector<CollisionMatrix> matrices;
        bool valid = false;
        bool husk_bit = false;
        uint32_t piece_mask = 0;
        uint64_t registry_spawn_id = 0;
        int32_t effective_model_id = -1;
    };
    // Cleared by every table epoch and collision model/instance/pose mutation,
    // so entries never outlive either their tick or their source identity.
    mutable std::unordered_map<uint16_t, TraceViewCacheEntry> trace_view_cache_;
    // Exact stable-phase LOS broad phase. Candidates retain pool-2 then pool-1
    // order; a bounded dense grid handles ordinary mission extents and the
    // sparse hash grid handles pathological extents. Both bucket forms hold
    // candidate indices, and each query sorts its small deduplicated result
    // back into pool order before the exact clip.
    std::vector<StableLosCandidate> stable_los_candidates_;
    std::unordered_map<uint64_t, StableLosCell> stable_los_cells_;
    std::vector<StableLosCell> stable_los_dense_cells_;
    std::vector<StableLosCellSpan> stable_los_cell_spans_;
    std::vector<uint32_t> stable_los_large_candidates_;
    std::vector<uint32_t> stable_los_query_candidates_;
    std::vector<uint32_t> stable_los_query_marks_;
    uint32_t stable_los_query_generation_ = 0;
    uint32_t stable_los_index_epoch_ = 0;
    int32_t stable_los_dense_min_x_ = 0;
    int32_t stable_los_dense_max_x_ = -1;
    int32_t stable_los_dense_min_y_ = 0;
    int32_t stable_los_dense_max_y_ = -1;
    int32_t stable_los_dense_height_ = 0;
    bool stable_los_dense_enabled_ = false;
    bool stable_los_index_ready_ = false;
    const CollisionTargetView *trace_target_view(const World &world, EntityHandle h) const;
    // `wire_b`: entity B is a decoded remote person (raycast_clear_to_person), whose Flags
    // indoors bit stands in for a registry B's.
    bool raycast_clear_impl(World &world, const int32_t a[3], const int32_t b[3],
                            EntityHandle exclude_a, EntityHandle exclude_b,
                            EntityHandle parent_a, EntityHandle parent_b,
                            bool cache_target_views, const LosWireEndpoint *wire_b = nullptr);
    uint64_t last_los_sector_candidates_ = 0;
    ProjectileHit trace_projectile_impl(const World &world,
                                        const ProjectileTrace &trace,
                                        bool person_faces_only, bool aim = false) const;
    void record_trace_debug(const World &world, const ProjectileTrace &trace,
                            const ProjectileHit &hit, RayDebugCategory category,
                            bool knife) const;

    bool trace_profile_enabled_ = false;
    mutable TraceProfile trace_profile_;
    bool ray_debug_enabled_ = false;
    uint32_t ray_debug_mask_ = kRayDebugMaskAll;
    int32_t ray_debug_ttl_ticks_ = 93; // ~1.5 s at 62 Hz
    mutable RayDebugCategory ray_debug_scope_ = RayDebugCategory::kUncategorized;
    mutable std::array<RayDebugRing,
                       static_cast<size_t>(RayDebugCategory::kCount)>
            ray_debug_rings_;
    bool contact_debug_enabled_ = false;
    uint32_t contact_debug_mask_ = kContactDebugMaskAll;
    mutable ContactDebugRing contact_debug_ring_;
    // target_view's model selection without the section-matrix build: fills the
    // entity position and bound radius for the witnessed gate-before-view order.
    // False exactly when target_view would return null; solid_only also
    // rejects models without a solid volume.
    bool target_bound(const World &world, EntityHandle h, int32_t pos_out[3],
                      int32_t &radius_out, bool solid_only) const;

    std::vector<CollisionModel> models_;
    std::unordered_map<uint16_t, Instance> instances_; // key: EntityHandle.packed
    IPoseProvider *pose_provider_ = nullptr; // non-owning embedder seam

    std::vector<StaticSlot> statics_;   // cap 1199 counted [orig: g_StaticProx*]
    int32_t static_count_ = 0;
    int32_t static_building_count_ = 0;
    // Build state is independent of table contents: an empty-but-built table is
    // still authoritative and must not fall back to whole-registry scans.
    bool tick_tables_built_ = false;
    bool candidate_slices_built_ = false;
    // A pool-2 instance/registry edge since the statics table was last built.
    bool statics_dirty_ = true;
    // Candidate slices rebuild only every 17th tick — the counter increments per
    // tick and the rebuild fires (and resets it) once it reaches 16; pool tables
    // rebuild every tick. BSS-zero start: retail's first slice build lands on
    // tick 17 (mission starts run 16 sliceless ticks). [orig: dword_B57C84 vs
    // 0x10 @ 0x4c240f, zeroed by Entity_BuildProximityListsFromPools @ 0x4b8ed0]
    uint32_t slice_refresh_counter_ = 0;

    std::vector<DynSlot> dynamics_;     // [orig: g_DynProx*]
    std::vector<PersonSlot> persons_;   // [orig: g_PersonProx*]
    std::vector<WirePersonCollisionProxy> wire_person_proxies_;
    std::vector<WireDynamicCollisionProxy> wire_dynamic_proxies_;
    // Server H used solely as local L's ordering key during proxy-enabled
    // person walks. Geometry and ignore logic continue to use L.
    uint16_t wire_local_player_handle_ = 0xFFFF;
    // The arena's 3000 entries and its fill count. A build refills it from 0
    // and leaves the entries past the fill as the last build that reached
    // them wrote them; a slice a later build skipped keeps its start and
    // count, so it reads whatever the arena holds there now.
    // [orig: g_ProxCandidateArena, g_ProxCandidateArenaUsed; the < 3000 tests
    //  @0x4b8fd4 / @0x4b90bc / @0x4b9214 / @0x4b92fc]
    static constexpr int32_t kCandidateArenaCap = 3000;
    std::vector<EntityHandle> arena_;
    int32_t arena_used_ = 0;
    std::unordered_map<uint16_t, CandidateSlice> candidates_;
    // The registry the slices were last built over: a slice belongs to the slot's
    // lifetime it was built for (find_slice).
    const EntityRegistry *slice_registry_ = nullptr;
    // A distinct identity domain for decoded rows. Using a second arena/map is
    // structural: wire H=0 and a registry L=0 are both valid simultaneously. The
    // wire arena keeps the registry arena's form: 3000 entries refilled from 0 at
    // each build, a slice a build skips keeping its start and count (D-COL-20).
    std::vector<EntityHandle> wire_arena_;
    int32_t wire_arena_used_ = 0;
    std::unordered_map<uint16_t, CandidateSlice> wire_candidates_;
    // The decoded rows the tables pass over, by handle (replace_wire_collision_proxies).
    std::vector<WireSliceRow> wire_skipped_rows_;
    // Drop every wire slice no proxy or skipped row names with its lifetime.
    void purge_stale_wire_slices();
    // Staged only for the duration of one resolve_replica call: the replica
    // peer spheres the person-repulsion loop walks after persons_, and the
    // calling row's own wire handle (self-exclusion). Empty for every
    // ordinary resolve_entity call.
    const ReplicaPeer *replica_peers_ = nullptr;
    int32_t replica_peer_count_ = 0;
    uint16_t replica_exclude_handle_ = 0xFFFF;
    // The row's runtime type id (its def's sound profile binding: the rotor-strike sound).
    int32_t replica_item_id_ = 0;
    // A cell index over the staged peer table, rebuilt once per (table, pump
    // tick): the repulsion loop only ever moves a row for peers within 30% of
    // the summed radii, so each resolve gathers the peers of the cells that
    // reach can touch and walks them in table order — the same peers, the
    // same order, the same pushes as the full walk (see the exactness guard
    // beside the walk). Pure acceleration; no witnessed rule lives here.
    struct ReplicaPeerIndex {
        const ReplicaPeer *src = nullptr;
        int32_t count = 0;
        uint32_t tick = 0;
        int32_t max_radius = 0;
        std::unordered_map<uint64_t, std::vector<int32_t>> cells;
        std::vector<int32_t> gathered;
    };
    ReplicaPeerIndex replica_peer_index_;
    bool replica_peer_index_enabled_ = true;
    void stage_replica_peer_index(const ReplicaPeer *peers, int32_t count,
                                  uint32_t tick);

public:
    // Test seam: the full table walk stays reachable so the cell gather can
    // be proven equivalent against it.
    void set_replica_peer_index_enabled(bool enabled) {
        replica_peer_index_enabled_ = enabled;
    }

private:
    int32_t replica_source_bound_radius_q16_ = 0;
    // Staged like replica_peers_: the calling row's retail-Flags mirror. The
    // resolver's flag latch sites and the ground probe's indoors gate read and
    // write it exactly where they read and write ent->flags on a registry row.
    uint32_t *replica_flags_ = nullptr;
    // Staged like replica_flags_: the calling row's savedLivePose, which
    // resolve_entity reads where it reads a registry row's own words.
    const int32_t *replica_saved_live_ = nullptr;
    // Staged like replica_flags_: the calling row's mounted latch (resolve_replica).
    bool replica_mounted_latch_ = false;
    // resolve_replica's candidate staging under the reserved invalid key, which
    // raycast_ground_replica shares; unstage drops it and the scratch arena tail.
    size_t stage_replica_candidates(const int32_t pos[3], int32_t source_bound_radius_q16,
                                    uint16_t exclude_handle);
    void unstage_replica_candidates(size_t arena_mark);
};

} // namespace opennova::world
