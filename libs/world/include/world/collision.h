// World-object collision: the runtime queries the original engine runs against a
// model's collision block (the .3di CDTA volumes/planes), plus the per-tick
// proximity tables that feed them. Witness record: docs/world/world-wac-ai-re.md
// §15 (collision + blink boxes), grilled 2026-07-09 against Jointops.exe.
//
// Runtime data (the query-side layout, witnessed in the three query functions):
//   COBJ section records — volume list + local AABB + bound sphere
//   BVOL volume records  — collidable type + local AABB + plane run + flags
//   BPLN plane records   — int16 Q14 normal + 16.16 distance
// The host builds these PODs from the parsed .3di collision IR (the same
// leaf-consumer seam as terrain_query's TerrainHeightField, ADR 0020): libs/world
// never touches the format stack.
//
// Collidable types (runtime dispatch, witnessed in
// Entity_ComputeBoneCollisionForce @ 0x4ae150):
//   4  platform/seat surface -> contact flag 0x1 + platform-carry anchor
//   6  armory volume ("CA")  -> contact flag 0x4 -> entity Flags |= 0x400000
//      (gates the in-game armory screen: input action 218 opens weapon.mnu WEAPON
//       only while this flag is set [orig: Input_HandleActionBinding @ 0x49b848])
//   7  damage-only volume (projectile mask 0x8 path)
//   8  blink box ("BB")      -> contact flag 0x10 + blink accumulation (buildings)
//   9  destructible-section touch -> contact flag 0x20 + section bit on target
//   10 capture-zone touch    -> contact flag 0x200
//   11 vehicle-loadout volume -> contact flag 0x400 -> entity Flags |= 0x800
//      (gates vehicle.mnu VEHICLE on the same key [orig: @ 0x49b858])
//   12 masked volume (mask 0x10 path)
//   13 grounded-only touch   -> contact flag 0x800 (source stands on target)
//   16/17/18 hurt volumes    -> contact flags 0x100/0x80/0x40 (-50/-6/-1 HP)
//   19 player-only solid (mask 0x2)
//   1 (and other unlisted types) solid -> SAT push-out force
//   5  contact-no-force marker
// Raycasts test ONLY type-1 volumes [orig: Entity_RaycastCollisionModel @ 0x413060];
// the blink point query tests ONLY type-8 volumes of building-kind entities
// [orig: Entity_TestCollisionSections @ 0x4aef90].
#ifndef OPENNOVA_WORLD_COLLISION_H
#define OPENNOVA_WORLD_COLLISION_H

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "world/entity.h"

namespace opennova::terrain {
struct TerrainHeightField;
}

namespace opennova::world {

class World;

// ----------------------------------------------------------------------------
// Runtime collision model (per graphic).
// ----------------------------------------------------------------------------

// [orig: runtime BPLN record, 12 B — int16 flags? + Q14 normal xyz @+2/+4/+6 +
// 16.16 dist @+8; dot scales witnessed as >>14 (ray clip) and >>9 vs 32*dist
// (contact force).]
struct CollisionPlane {
    int16_t nx = 0, ny = 0, nz = 0; // Q14 unit normal
    int32_t dist = 0;               // 16.16 plane distance
};

// [orig: runtime BVOL record, 40 B — type @+0, AABB minX,maxX,minY,maxY,minZ,maxZ
// @+4..+24, plane_count @+28, plane_ptr @+32, flags @+36.]
struct CollisionVolume {
    int32_t type = 0;
    int32_t min_x = 0, max_x = 0;   // section-local 16.16
    int32_t min_y = 0, max_y = 0;
    int32_t min_z = 0, max_z = 0;
    int32_t plane_start = 0;        // run into CollisionModel::planes
    int32_t plane_count = 0;
    uint32_t flags = 0;             // authored bvol flags (blink letters)
};

// [orig: runtime COBJ record, 108 B — volume count @+28, volume ptr @+36, damage
// start @+32, local AABB minX,maxX,minY,maxY,minZ,maxZ @+68..+88, bound-sphere
// center @+92..+100 + radius @+104.]
struct CollisionSection {
    int32_t volume_start = 0;       // run into CollisionModel::volumes
    int32_t volume_count = 0;
    int32_t damage_start = -1;      // first damage volume index (-1 = none) [orig: +32]
    int32_t min_x = 0, max_x = 0;   // section-local 16.16 AABB
    int32_t min_y = 0, max_y = 0;
    int32_t min_z = 0, max_z = 0;
    int32_t center[3] = {};         // bound-sphere center (section-local 16.16)
    int32_t radius = 0;             // bound-sphere radius (16.16)
    int32_t part_index = -1;        // source render part (animated-part transforms later)
};

struct CollisionModel {
    std::vector<CollisionSection> sections;
    std::vector<CollisionVolume> volumes;
    std::vector<CollisionPlane> planes;
    // Model-level AABB (union of the section AABBs, mission axes 16.16) — the
    // runtime collision-header bounds the render occlusion reads. [orig: the
    // collision block +24..+44 min/max fields, consumed by render_TOC's corner
    // refinement @ 0x5c4920, the water-straddle checks @ 0x5c8922, and
    // Entity_ComputeBoundingSphere @ 0x5c69a0]
    int32_t min[3] = {}, max[3] = {};
    bool valid() const { return !sections.empty(); }

    // Derive section AABB/bound-sphere from its volumes (the loader precomputes
    // these on the runtime records; we rebuild them at model build time).
    void finalize_sections();
};

// ----------------------------------------------------------------------------
// Section transform: 16-dword fixed matrix, Q22 rotation rows + 16.16 translation,
// m[15] low bit = section disabled. [orig: the per-section matrix array returned
// by the model's transform callback (model+168); consumed via
// Math_TransformPointWithTranslation22 @ 0x412f60 (rotate>>22 + translate),
// Math_TransformPointFixedPoint22 @ 0x412e90 / Math_FixedPointTransformPoint22
// @ 0x615810 (rotate only), inverted by Matrix_Transpose3x3WithNegateCol3
// @ 0x6136d0 (orthonormal transpose + negated rotated translation). Row/column
// convention is self-consistent here: we both build and invert the matrices.]
// ----------------------------------------------------------------------------
struct CollisionMatrix {
    int32_t m[16] = {};

    bool disabled() const { return (m[15] & 1) != 0; }

    // world = R * local >> 22 + t
    void transform_point(const int32_t in[3], int32_t out[3]) const;
    // world dir = R * local >> 22 (no translation)
    void rotate_point(const int32_t in[3], int32_t out[3]) const;
    // out = inverse(this) for orthonormal rotations: R' = transpose(R),
    // t' = -(R' * t) >> 22. [orig: Matrix_Transpose3x3WithNegateCol3 @ 0x6136d0]
    void invert_into(CollisionMatrix &out) const;
};

// Build a yaw-only world matrix (heading in BAM32, translation 16.16) using the
// engine's quantized direction table — the static-object placement transform.
// Per-part animated section transforms are a tracked follow-up (D-COL-1).
CollisionMatrix collision_matrix_from_heading(int32_t heading_bam, const int32_t pos[3]);

// The terrain leg of the LOS segment query, TRUE = the segment hits terrain
// (blocked). The ported heightmap raycast over the runtime height field; shared
// by CollisionWorld::raycast_clear, the sound-occlusion LOS, and the
// collision-less AiSystem fallback.
// [orig: Terrain_RaycastHeightmapHiRes @ 0x60c760, faithfully ported as
// terrain_raycast_los_clear (the occlusion slice closed the sibling-internals
// open item): endpoint prechecks + the 4u-texel point-sample march + the
// height-0 floor; nonzero = clear.]
bool los_terrain_blocked(const terrain::TerrainHeightField &field, const int32_t a[3],
                         const int32_t b[3]);

// ----------------------------------------------------------------------------
// Per-query blink accumulation. [orig: g_BlinkFlagsAccum @ 0xB57C70,
// g_BlinkHitSlot0..3 @ 0xB57C74, g_BlinkHitCount @ 0x82AE20 — cleared per query
// scope; hits pack ((section & 0x1F) | (pool_index << 8)) << 12; on hit the
// accumulator ORs volume.flags ^ 6, and bit 2 drives entity Flags 0x800000
// ("indoors").]
// ----------------------------------------------------------------------------
struct BlinkAccum {
    uint32_t flags = 0;
    int32_t hit_count = 0;
    uint32_t hits[4] = {};

    void reset() { flags = 0; hit_count = 0; hits[0] = hits[1] = hits[2] = hits[3] = 0; }
    void add_hit(int32_t section_index, int32_t pool_entity_index) {
        if (hit_count != -1 && hit_count < 4)
            hits[hit_count++] =
                static_cast<uint32_t>(((section_index & 0x1F) + (pool_entity_index << 8)) << 12);
    }
};

inline constexpr uint32_t kBlinkIndoorsBit = 0x2;        // accum bit -> Flags 0x800000
inline constexpr uint32_t kEntityFlagIndoors = 0x800000; // entity+36 bit
inline constexpr uint32_t kEntityFlagOnPlatform = 0x100000;
inline constexpr uint32_t kEntityFlagArmoryZone = 0x400000;  // type-6 volume touch
inline constexpr uint32_t kEntityFlagVehicleLoadoutZone = 0x800; // type-11 volume touch

// A test point (stride-4 record, xyz + spare — faithful to the caller layout).
struct CollisionPoint {
    int32_t x = 0, y = 0, z = 0, w = 0;
};

// The target side of a query: the placed entity's collision instance + the entity
// fields the original reads off GamePlayerEntity.
struct CollisionTargetView {
    const CollisionModel *model = nullptr;
    const CollisionMatrix *matrices = nullptr; // one per section
    int32_t pos[3] = {};                       // entity+4/+8/+12 (16.16)
    int32_t yaw_bam = 0;                       // entity+16 Yaw (BAM32) — platform-anchor leg
    int32_t pitch_bam = 0;                     // entity+20 Pitch (BAM32) — platform-anchor leg
    uint32_t entity_flags = 0;                 // entity+36 Flags (contact rejects flags & 1)
    int32_t bound_radius = 0;                  // entity+0 boundRadius (16.16)
    bool is_building = false;                  // itemDef type == 5 (blink gate)
    int32_t pool_index = 0;                    // pool index for the packed blink hit
    bool is_ground_of_source = false;          // source->groundEntity == target (type 13)
};

// ----------------------------------------------------------------------------
// Point-vs-blink query. Tests points (with per-point radii) against the TYPE-8
// volumes of a building-kind target; accumulates blink flags/hits into `blink`.
// Returns true when any point sits inside a blink volume.
// [orig: Entity_TestCollisionSections @ 0x4aef90]
// ----------------------------------------------------------------------------
bool collision_test_blink(const CollisionTargetView &target, const CollisionPoint *points,
                          const int32_t *radii, int32_t num_points, BlinkAccum &blink);

// ----------------------------------------------------------------------------
// Segment-vs-solid query. Clips ray.end to the nearest entry point into any
// TYPE-1 volume; returns true on hit. [orig: Entity_RaycastCollisionModel
// @ 0x413060 — per-section bound-sphere reject, then convex clip of the segment
// against the volume's plane run (dot >> 14 + dist), entry point written back.]
// ----------------------------------------------------------------------------
struct CollisionRay {
    int32_t start[3] = {};
    int32_t end[3] = {};   // clipped in place on hit
    int32_t mid[3] = {};
    int32_t half[3] = {};  // abs half extents
    int32_t dir[3] = {};   // normalized 16.16 direction (float-normalized like the orig)

    // Build mid/half/dir from start/end. [orig: prologue of raycast_entity_collision
    // @ 0x413760 / Entity_FindNearestByRay @ 0x413af0 (float normalize, ftol)]
    void refresh();
    // Recompute mid/half only — dir is built ONCE at ray construction and never
    // re-derived from a clipped segment. [orig: the hit tail @ 0x41370c-0x41374e
    // refreshes point[6..11] and leaves dir untouched]
    void refresh_bounds();
};

bool collision_raycast_model(const CollisionTargetView &target, CollisionRay &ray);

// ----------------------------------------------------------------------------
// Contact force query: capsule test points vs every volume of the target model.
// Faithful port of Entity_ComputeBoneCollisionForce @ 0x4ae150 (the SAT push-out
// over the plane run with prev-position gating, second-plane assist, per-type
// flag dispatch, blink accumulation, and the bound-radius force clamp).
//
// mask bits: 0x1 = on-platform (test type-4 seat volumes), 0x2 = player (test
// type-19), 0x8 = damage pass (types 7/12 only), 0x10 = type-12 pass.
// out_force is the world-space push (16.16, already >>5-scaled); out_flags is
// the contact-flag word listed in the type table above.
// ----------------------------------------------------------------------------
struct PlatformContact {
    // [orig: g_PlatformContactYaw/Pitch @ 0xB5AB74/0xB5AB70, anchor X/Y/Z
    // @ 0xB5AB78/7C/80 — written by the type-4 seat-volume hit, consumed by the
    // resolver's platform ride.]
    int32_t anchor[3] = {};
    int32_t yaw = 0;
    int32_t pitch = 0;
    bool valid = false;
};

struct ContactQuery {
    const CollisionPoint *points = nullptr;
    const int32_t *radii = nullptr;
    int32_t num_points = 0;
    int32_t prev_pos[3] = {};      // source savedLivePose (plane gating)
    int32_t source_bound_radius = 0;
    uint8_t mask = 0;
    bool query_is_player = false;  // [orig: g_CollisionQueryIsPlayer @ 0xB5AB84]
};

struct ContactResult {
    int32_t force[4] = {};         // world push force (16.16); [3] spare like the orig
    uint32_t flags = 0;            // contact flags (see the type table)
    uint32_t touched_sections = 0; // type-9 bit-per-section mask [orig: target+692]
};

bool collision_contact_force(const CollisionTargetView &target, const ContactQuery &q,
                             BlinkAccum &blink, PlatformContact &platform, ContactResult &out);

// ----------------------------------------------------------------------------
// CollisionWorld: the per-tick proximity tables + per-entity instances, and the
// world-level blink state. [orig: the g_StaticProx*/g_DynProx*/g_PersonProx*
// tables + g_ProxCandidateArena rebuilt each tick by
// Entity_BuildProximityLists_Pool2 @ 0x4b9430, _Pool01 @ 0x4b9340 and
// Entity_BuildProximityListsFromPools @ 0x4b8eb0.]
// ----------------------------------------------------------------------------
class CollisionWorld {
public:
    // --- model registry (host-fed, keyed by an opaque graphic id) ---
    int32_t add_model(CollisionModel model); // returns model id
    const CollisionModel *model(int32_t id) const;
    // Attach a model instance to a live entity (net_id keyed like the traits sweep).
    void assign_entity(EntityHandle h, int32_t model_id);
    bool has_instance(EntityHandle h) const;
    size_t instance_count() const { return instances_.size(); }

    // --- per-tick snapshots ---
    // [orig: Entity_BuildProximityLists_Pool2 @ 0x4b9430] statics (pool-2 style):
    // buildings first [0..static_building_count), all statics [0..static_count),
    // positions quantized (p + 0x8000) >> 16 as u16, radius padded +111876, cap 1200.
    // [orig: Entity_BuildProximityLists_Pool01 @ 0x4b9340] persons (pool 0) into the
    // full-precision repulsion table; dynamics (pool 1) into the dyn table.
    // [orig: Entity_BuildProximityListsFromPools @ 0x4b8eb0] per-entity candidate
    // slices (dyn radius+4.0u / statics; pool-1 radius+6.0u) into a 3000-entry arena.
    void build_tick_tables(World &world);

    // Per-entity blink refresh: test the entity position (one point, radius 0.5u)
    // against nearby buildings' blink volumes; stamp Entity.blink_hits + the
    // indoors flag; accumulate the local player's flags word.
    // [orig: Entity_BuildProximityList @ 0x4b3dc0]
    void refresh_blink(World &world, Entity &ent);

    // Point blink query at an arbitrary position (the camera-side analog of
    // refresh_blink): walk the building prefix with per-axis + euclid broad
    // phase at radius + 0x8000, run the point query (one point, radius 0x8000),
    // return the packed hit set in `accum`. Clears `accum` first.
    // [orig: Entity_QueryBlinkBoxesAtPoint @ 0x4af350]
    void query_blink_boxes_at_point(World &world, const int32_t pos[3], BlinkAccum &accum);

    // Ground-column probe through terrain + the entity's candidate models.
    // Builds the ray {x+dx, y+dy, z+z_up} down z_drop, clamps to the terrain
    // column, clips against candidate solids; returns the resolved ground Z and
    // (optionally) the hit entity. [orig: Entity_RaycastGroundHeight @ 0x4142c0 /
    // Entity_RaycastGroundHeightAndObject @ 0x414320 -> raycast_entity_collision
    // @ 0x413760]
    int32_t raycast_ground(World &world, EntityHandle source, const int32_t pos[3],
                           int32_t dx, int32_t dy, int32_t z_up, int32_t z_drop,
                           EntityHandle *out_hit_entity);

    // Segment LOS query, TRUE = CLEAR of terrain + sector solids — the AI mutual-LOS
    // seam. [orig: Physics_RaycastTerrainAndSectors @ 0x539910: terrain leg via
    // Terrain_RaycastHeightmapHiRes @ 0x60c760 (skipped when BOTH excluded entities
    // carry Flags & 0x800000 INDOORS — the heightmap has no interiors), then the
    // sector walk (raycast_against_entity_pool @ 0x538720) over pool 2 statics, then
    // pool 1 dynamics, excluding both entities; LOS callers pass ray radius 0.]
    // Tracked D-AI-7 residuals: the +0x28 owner-link exclusion, the Flags&4
    // destroyed-husk model swap, and the itemDef type-3 person sphere-block
    // (same-team within 3.0 u exempt) — person-kind residents of the walked pools
    // don't exist in our world yet (organics are pool 0, unwalked, like retail).
    bool raycast_clear(World &world, const int32_t a[3], const int32_t b[3],
                       EntityHandle exclude_a, EntityHandle exclude_b);

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
    // raycast_find_collision_entity @ 0x539a70 with allowAllTypes = 0 —
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
    // airborne. [orig: Entity_ProcessCollisionAndPlatformPhysics @ 0x4b2bd0]
    // capsule_bottom/top are the anim frame's 16.16 capsule extents (out[3]/out[4]).
    struct ResolveState {
        int32_t prev_pos[3] = {};   // savedLivePose stand-in (updated per resolve)
        bool prev_valid = false;
        uint8_t skip_counter = 0;   // [orig: entity pad_370[3] idle throttle]
    };
    // anim_state_flags = the state's g_animStateFlagsTable word (bit 0 forces a
    // full update; the id itself picks the repulsion-exempt states).
    int32_t resolve_entity(World &world, EntityHandle source, ResolveState &state,
                           int32_t pos[3], int32_t vel_xy[2], int32_t &vel_z,
                           int32_t capsule_bottom, int32_t capsule_top,
                           int32_t heading, int32_t body_pitch, bool is_player,
                           bool is_authority, uint32_t tick, int32_t anim_state_id,
                           uint32_t anim_state_flags, int16_t &health);

    // World-level blink state for the local player.
    // [orig: g_LocalPlayerBlinkFlags @ 0x24C1934]
    uint32_t local_player_blink_flags = 0;
    EntityHandle local_player;

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

    // Host-wired terrain (shared with the AI system's field).
    const terrain::TerrainHeightField *terrain = nullptr;

    // --- introspection for tests/host ---
    int32_t static_count() const { return static_count_; }
    int32_t static_building_count() const { return static_building_count_; }
    int32_t candidate_count(EntityHandle h) const;

    // Static prox-table slot view (quantized u16 coords/radius like the retail
    // tables) — the render-occlusion engine walks the building prefix through
    // this. [orig: g_StaticProx{X,Y,Z,Radius,Entity} @ 0xB55558/0xB54BF8/
    // 0xB54298/0xB55EB8/0xB52FD8]
    struct StaticSlotView {
        uint16_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    StaticSlotView static_slot(int32_t i) const;
    // The collision model attached to a live entity (nullptr when none).
    const CollisionModel *model_for(EntityHandle h) const;

    // Read-only world-space geometry snapshot for a host collision debug view.
    // Each instance's volumes are transformed through the SAME target_view /
    // collision_matrix_from_heading path every query uses, so what the host
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

private:
    // Contact-flag side effects shared by both resolver passes (damage tiers +
    // the type-6/type-11 entity flags). [orig: the dispatch @ 0x4b30b7-0x4b351e]
    void apply_touch_flags(Entity *ent, uint32_t flags, int16_t &health, bool is_authority);

    struct Instance {
        int32_t model_id = -1;
    };
    struct StaticSlot { // [orig: g_StaticProx* u16 tables + entity ptr array]
        uint16_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct DynSlot {    // [orig: g_DynProx* dword tables]
        int32_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct PersonSlot { // [orig: g_PersonProx* dword tables]
        int32_t x = 0, y = 0, z = 0, radius = 0;
        EntityHandle h;
    };
    struct CandidateSlice {
        int32_t start = 0;
        int32_t count = 0;
    };

    const CollisionTargetView *target_view(World &world, EntityHandle h,
                                           CollisionTargetView &scratch,
                                           std::vector<CollisionMatrix> &mat_scratch) const;

    // One sound-occlusion LOS ray (terrain + building legs); true = clear.
    // [orig: Entity_CheckLineOfSightTerrainAndEntities @ 0x53b130]
    bool sound_los_clear(World &world, EntityHandle listener, EntityHandle source,
                         const int32_t start[3], const int32_t end[3],
                         int32_t height_offset);

    std::vector<CollisionModel> models_;
    std::unordered_map<uint16_t, Instance> instances_; // key: EntityHandle.packed

    std::vector<StaticSlot> statics_;   // cap 1199 counted [orig: g_StaticProx*]
    int32_t static_count_ = 0;
    int32_t static_building_count_ = 0;
    // Candidate slices rebuild only every 17th tick — the counter increments per
    // tick and the rebuild fires (and resets it) once it reaches 16; pool tables
    // rebuild every tick. BSS-zero start: retail's first slice build lands on
    // tick 17 (mission starts run 16 sliceless ticks). [orig: dword_B57C84 vs
    // 0x10 @ 0x4c240f, zeroed by Entity_BuildProximityListsFromPools @ 0x4b8ed0]
    uint32_t slice_refresh_counter_ = 0;

    std::vector<DynSlot> dynamics_;     // [orig: g_DynProx*]
    std::vector<PersonSlot> persons_;   // [orig: g_PersonProx*]
    std::vector<EntityHandle> arena_;   // cap 3000 [orig: g_ProxCandidateArena]
    std::unordered_map<uint16_t, CandidateSlice> candidates_;
};

} // namespace opennova::world

#endif // OPENNOVA_WORLD_COLLISION_H
